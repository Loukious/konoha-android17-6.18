#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define GFP_KERNEL 0
#define __init
#define __exit
#define THIS_MODULE NULL
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define MCA_VOTE_OR 0
#define MCA_VOTE_MIN 1
#define mca_log_info(...) ((void)0)
#define mca_log_err(...) ((void)0)
#define MODULE_DEVICE_TABLE(...)
#define MODULE_DESCRIPTION(...)
#define MODULE_AUTHOR(...)
#define MODULE_LICENSE(...)
#define module_init(...)
#define module_exit(...)
struct device {
    void *data, *allocations[4], *action_data;
    void (*action)(void *);
    bool wakeup, awake;
    unsigned alloc_count;
};
struct platform_device { struct device dev; };
struct of_device_id { const char *compatible; };
struct platform_driver {
    struct { void *owner; const char *name; const struct of_device_id *of_match_table; } driver;
    int (*probe)(struct platform_device *);
    void (*remove)(struct platform_device *), (*shutdown)(struct platform_device *);
};
struct mca_votable {
    const char *name;
    int (*callback)(struct mca_votable *, void *, int, const char *);
    void *data;
};
static struct mca_votable *voters[2];
static int alloc_fail, alloc_calls, wake_error, action_error, voter_fail, voter_calls;
static unsigned destroyed, relaxed;
static void *devm_kzalloc(struct device *d, size_t size, int flags) {
    (void)flags;
    if (++alloc_calls == alloc_fail) return NULL;
    void *p = calloc(1, size); assert(p && d->alloc_count < 4);
    d->allocations[d->alloc_count++] = p; return p;
}
static void platform_set_drvdata(struct platform_device *p, void *data) { p->dev.data = data; }
static void *platform_get_drvdata(struct platform_device *p) { return p->dev.data; }
static int device_init_wakeup(struct device *d, bool enable) {
    if (enable && wake_error) return wake_error;
    d->wakeup = enable; return 0;
}
static void pm_stay_awake(struct device *d) { assert(d->wakeup); d->awake = true; }
static void pm_relax(struct device *d) { d->awake = false; relaxed++; }
static int devm_add_action_or_reset(struct device *d, void (*fn)(void *), void *data) {
    assert(!d->action);
    if (action_error) { fn(data); return action_error; }
    d->action = fn; d->action_data = data; return 0;
}
static void devm_release_action(struct device *d, void (*fn)(void *), void *data) {
    assert(d->action == fn && d->action_data == data);
    d->action = NULL; fn(data);
}
static struct mca_votable *mca_create_votable(const char *name, int type,
    int (*fn)(struct mca_votable *, void *, int, const char *), int def, void *data) {
    (void)type; assert(def == 0 && data);
    if (++voter_calls == voter_fail) return ERR_PTR(-EEXIST);
    unsigned slot = !strcmp(name, "AWAKE") ? 0 : 1; assert(!voters[slot]);
    voters[slot] = calloc(1, sizeof(**voters)); assert(voters[slot]);
    *voters[slot] = (struct mca_votable){name, fn, data}; return voters[slot];
}
static void mca_destroy_votable(struct mca_votable *v) {
    if (!v) return;
    unsigned slot = !strcmp(v->name, "AWAKE") ? 0 : 1;
    assert(voters[slot] == v && !IS_ERR(v));
    voters[slot] = NULL; destroyed++; free(v);
}
static int platform_driver_register(struct platform_driver *d) { (void)d; return 0; }
static void platform_driver_unregister(struct platform_driver *d) { (void)d; }
/* INSERT_DRIVER */
static void release(struct platform_device *p) {
    if (p->dev.action) { p->dev.action(p->dev.action_data); p->dev.action = NULL; }
    for (unsigned i = 0; i < p->dev.alloc_count; i++) free(p->dev.allocations[i]);
    assert(!voters[0] && !voters[1] && !p->dev.wakeup && !p->dev.awake);
}
int main(void) {
    unsigned cases = 0;
    for (int fail = 0; fail < 7; fail++) {
        struct platform_device p = {0};
        alloc_fail = fail == 1 ? 1 : fail == 2 ? 2 : 0;
        wake_error = fail == 3 ? -EIO : 0;
        action_error = fail == 4 ? -ENOMEM : 0;
        voter_fail = fail == 5 ? 1 : fail == 6 ? 2 : 0;
        alloc_calls = voter_calls = 0; destroyed = 0;
        int ret = business_misc_probe(&p);
        int expected = fail == 1 || fail == 2 || fail == 4 ? -ENOMEM :
                       fail == 3 ? -EIO : fail >= 5 ? -EEXIST : 0;
        assert(ret == expected);
        if (!ret) {
            assert(p.dev.wakeup && voters[0] && voters[1]);
            assert(!voters[0]->callback(voters[0], voters[0]->data, 1, "test"));
            assert(p.dev.awake);
            assert(!voters[0]->callback(voters[0], voters[0]->data, 0, "test"));
            assert(!p.dev.awake);
            for (int value = -10; value <= 100; value++) {
                assert(voters[1]->callback(voters[1], voters[1]->data, value, "test") == -EOPNOTSUPP);
                cases++;
            }
            voters[0]->callback(voters[0], voters[0]->data, 1, "test");
            business_misc_shutdown(&p);
            assert(destroyed == 2 && !p.dev.awake && !p.dev.wakeup);
            business_misc_remove(&p); assert(destroyed == 2);
        } else {
            assert(!voters[0] && !voters[1]);
            assert(destroyed == (fail == 6 ? 1U : 0U));
        }
        release(&p); cases++;
    }
    /* Direct remove and failed probe followed by devm release are both safe. */
    struct platform_device p = {0};
    alloc_fail = wake_error = action_error = voter_fail = alloc_calls = voter_calls = 0;
    assert(!business_misc_probe(&p));
    business_misc_remove(&p); release(&p); cases++;
    printf("%u\n", cases);
}
