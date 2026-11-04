#include <assert.h>
#include <stdio.h>
#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
struct mutex { unsigned held; };
struct wiphy { struct mutex mtx; };
struct wireless_dev { struct wiphy *wiphy; struct mutex mtx; };
static struct mutex *expected;
static unsigned locks, unlocks;
static void mutex_lock(struct mutex *m)
{
    assert(m && m == expected && !m->held);
    m->held = 1;
    locks++;
}
static void mutex_unlock(struct mutex *m)
{
    assert(m && m == expected && m->held == 1);
    m->held = 0;
    unlocks++;
}
static void wiphy_lock(struct wiphy *w)
{
    assert(w);
    mutex_lock(&w->mtx);
}
static void wiphy_unlock(struct wiphy *w)
{
    assert(w);
    mutex_unlock(&w->mtx);
}

/* INSERT_HELPERS */

static void exercise(struct wiphy *w, struct wireless_dev *wd,
                     struct mutex *wanted)
{
    expected = wanted;
    unsigned before = locks;
    hdd_wiphy_lock(w, wd);
    /* A cfg80211 notification requires the intended object's lock. */
    assert(wanted->held == 1 && locks == before + 1);
    hdd_wiphy_unlock(w, wd);
    assert(!wanted->held && locks == unlocks);
}

int main(void)
{
    struct wiphy w = {0}, other = {0};
    struct wireless_dev wd = {.wiphy = &w};
    /* Regulatory path supplies only wiphy. */
    exercise(&w, NULL, &w.mtx);
    /* Both channel-change-started and channel-change-completed use wdev. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)
    expected = &wd.mtx;
#else
    expected = &w.mtx;
#endif
    exercise(NULL, &wd, expected);
    exercise(NULL, &wd, expected);
    /* Explicit wiphy takes precedence over a supplied device. */
    exercise(&other, &wd, &other.mtx);
    assert(!w.mtx.held && !other.mtx.held && !wd.mtx.held);
    puts("4");
}
