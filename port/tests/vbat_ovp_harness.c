/* Actual monitor functions are inserted before main by verify_vbat_ovp.py. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define MAIN_BUCK_CHARGER 0
#define WIRELESS_ROLE_MASTER 0
#define MCA_EVENT_TYPE_HW_INFO 0
#define MCA_EVENT_VBAT_OVP_CHANGE 1
#define FAST_HEARTBEAT_TIMER_MS 10000
#define NORMAL_HEARTBEAT_TIMER_MS 60000
#define SYSFS_DEV_5 "charger"
#define max(a,b) ((a) > (b) ? (a) : (b))
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define msecs_to_jiffies(x) (x)
struct mutex { int held; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; };
struct device { int unused; };
struct platform_device { void *data; struct device dev; };
struct mca_vbat_ovp_mon_dev {
    struct device *dev;
    struct mutex lock;
    struct delayed_work monitor_vbat_ovp_work;
    bool stopping, sysfs_created, has_wireless, triggered;
    int vbat_ovp_thr_ffr_mv, vbat_ovp_thr_nor_mv;
    int vbat_ovp_thr_hys_mv, vbat_ovp_recharge_delta_mv;
    int fake_vbat_override_mv, fake_vbat_for_debug;
    int fake_vbat_ovp_status, batt_ovp_status;
};
static int temp = 250, voltage = 4400, usb = 1, wireless, ffc;
static int temp_error, voltage_error, usb_error, wireless_error;
static int wireless_calls, events, last_event, queued, delay, removed, drained;
static int vbat_ovp_sysfs_attr_group;
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = 1; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = 0; }
static int strategy_class_fg_ops_get_temperature(int *p) {
    if (!temp_error) *p = temp;
    return temp_error;
}
static int strategy_class_fg_ops_get_voltage(int *p) {
    if (!voltage_error) *p = voltage;
    return voltage_error;
}
static int platform_class_buckchg_ops_get_online(unsigned role, int *p) {
    (void)role;
    if (!usb_error) *p = usb;
    return usb_error;
}
static int platform_class_wireless_is_present(unsigned role, int *p) {
    (void)role;
    wireless_calls++;
    if (!wireless_error) *p = wireless;
    return wireless_error;
}
static int strategy_class_fg_get_fastcharge(void) { return ffc; }
static void mca_event_block_notify(int type, int event, int *status) {
    assert(type == MCA_EVENT_TYPE_HW_INFO && event == MCA_EVENT_VBAT_OVP_CHANGE);
    events++;
    last_event = *status;
}
static void schedule_delayed_work(struct delayed_work *w, int ms) {
    (void)w;
    queued++;
    delay = ms;
}
static void *platform_get_drvdata(struct platform_device *p) { return p->data; }
static void mca_sysfs_remove_link_group(const char *devname, const char *name,
                                      struct device *dev, const int *group) {
    (void)devname; (void)name; (void)dev; (void)group;
    removed++;
}
static void cancel_delayed_work_sync(struct delayed_work *w) {
    (void)w;
    drained++;
}
#define GFP_KERNEL 0
#define EPROBE_DEFER 517
#define mca_log_err(...) ((void)0)
#define mca_log_info(...) ((void)0)
#define INIT_DELAYED_WORK(w, fn) memset(w, 0, sizeof(*(w)))
static int parse_error, sysfs_error, alloc_fail, publications;
static struct mca_vbat_ovp_mon_dev *published;
static void mutex_init(struct mutex *m) { m->held = 0; }
static void *devm_kzalloc(struct device *dev, size_t size, int flags) {
    (void)dev; (void)flags;
    return alloc_fail ? NULL : calloc(1, size);
}
static void platform_set_drvdata(struct platform_device *p, void *data) {
    p->data = data;
    published = data;
}
static int mca_vbat_ovp_mon_parse_dt(struct mca_vbat_ovp_mon_dev *info) {
    (void)info;
    return parse_error;
}
static int vbat_ovp_mon_sysfs_create_group(struct device *dev) {
    (void)dev;
    /* Controls must remain inactive while a group/link can still fail. */
    assert(published->stopping && !published->sysfs_created);
    publications++;
    return sysfs_error;
}
static int dev_err_probe(struct device *dev, int error, const char *format) {
    (void)dev; (void)format;
    return error;
}
/* INSERT_FUNCTIONS */
static struct mca_vbat_ovp_mon_dev fresh(void) {
    return (struct mca_vbat_ovp_mon_dev) {
        .vbat_ovp_thr_ffr_mv = 4470, .vbat_ovp_thr_nor_mv = 4420,
        .vbat_ovp_thr_hys_mv = 15, .vbat_ovp_recharge_delta_mv = 50,
        .fake_vbat_override_mv = -22, .fake_vbat_for_debug = -22,
    };
}
int main(void) {
    unsigned tests = 0;
    struct mca_vbat_ovp_mon_dev d = fresh(), other = fresh();
    wireless_error = -EOPNOTSUPP;
    /* The unfitted wireless provider cannot disable wired monitoring. */
    for (int mv = 4200; mv <= 4600; mv++) {
        d.triggered = false;
        voltage = mv;
        assert(mca_vbat_mon_get_vbat_ovp_status(&d) == (mv > 4435));
        assert(!wireless_calls);
        tests++;
    }
    d.triggered = true;
    for (int mv = 4500; mv >= 4200; mv--) {
        voltage = mv;
        assert(mca_vbat_mon_get_vbat_ovp_status(&d) == (mv > 4370));
        tests++;
    }
    ffc = 1;
    for (int mv = 4400; mv <= 4500; mv++) {
        d.triggered = false;
        voltage = mv;
        assert(mca_vbat_mon_get_vbat_ovp_status(&d) == (mv > 4485));
        tests++;
    }
    ffc = 0;
    voltage = 4500;
    d.fake_vbat_override_mv = 4200;
    assert(mca_vbat_mon_get_vbat_ovp_status(&d) == 1);
    voltage = 4300;
    assert(mca_vbat_mon_get_vbat_ovp_status(&other) == 0);
    assert(d.triggered && !other.triggered);
    tests += 2;
    d.fake_vbat_override_mv = -22;
    /* Every transport/sensor error must preserve both the latch and event. */
    int *failures[] = { &temp_error, &voltage_error, &usb_error, &ffc };
    for (unsigned i = 0; i < sizeof(failures)/sizeof(failures[0]); i++) {
        for (int j = 1; j <= 3; j++) {
            int err = j == 1 ? -EIO : j == 2 ? -ENODEV : -EAGAIN;
            d.triggered = true;
            d.batt_ovp_status = 1;
            *failures[i] = err;
            int before = events;
            assert(mca_vbat_mon_get_vbat_ovp_status(&d) == err);
            mca_vbat_ovp_monitor_workfunc(&d.monitor_vbat_ovp_work.work);
            assert(d.triggered && d.batt_ovp_status == 1 && events == before);
            assert(delay == FAST_HEARTBEAT_TIMER_MS);
            *failures[i] = 0;
            tests++;
        }
    }
    usb = 0;
    d.has_wireless = true;
    d.triggered = true;
    assert(mca_vbat_mon_get_vbat_ovp_status(&d) == -EOPNOTSUPP && d.triggered);
    wireless_error = 0;
    wireless = 1;
    voltage = 4500;
    assert(mca_vbat_mon_get_vbat_ovp_status(&d) == 1);
    wireless = 0;
    assert(mca_vbat_mon_get_vbat_ovp_status(&d) == 0 && !d.triggered);
    tests += 3;
    usb = 1;
    d.has_wireless = false;
    d.triggered = true;
    temp = 480;
    assert(mca_vbat_mon_get_vbat_ovp_status(&d) == 1);
    temp = 250;
    voltage = 0;
    assert(mca_vbat_mon_get_vbat_ovp_status(&d) == -ENODATA && d.triggered);
    tests += 2;
    voltage = 4500;
    d.batt_ovp_status = 0;
    d.fake_vbat_for_debug = 5000;
    int before = events;
    mca_vbat_ovp_monitor_workfunc(&d.monitor_vbat_ovp_work.work);
    assert(events == before + 1 && last_event == 1 && d.batt_ovp_status == 1);
    d.fake_vbat_for_debug = -22;
    mca_vbat_ovp_monitor_workfunc(&d.monitor_vbat_ovp_work.work);
    assert(events == before + 1 && d.batt_ovp_status == 1);
    voltage = 4370;
    mca_vbat_ovp_monitor_workfunc(&d.monitor_vbat_ovp_work.work);
    assert(events == before + 2 && last_event == 0 && !d.batt_ovp_status);
    assert(delay == NORMAL_HEARTBEAT_TIMER_MS);
    tests += 3;
    /* Repeated teardown removes sysfs once and prevents work from rearming. */
    struct platform_device p = { .data = &d };
    d.sysfs_created = true;
    mca_vbat_ovp_mon_stop(&p);
    assert(d.stopping && !d.sysfs_created && removed == 1 && drained == 1);
    mca_vbat_ovp_mon_stop(&p);
    assert(removed == 1 && drained == 2);
    before = queued;
    mca_vbat_ovp_monitor_workfunc(&d.monitor_vbat_ovp_work.work);
    assert(queued == before);
    tests += 3;
    /* Failed publication cannot leave a scheduled worker behind. */
    p = (struct platform_device) {0};
    alloc_fail = 1;
    assert(mca_vbat_ovp_mon_probe(&p) == -ENOMEM && !p.data);
    alloc_fail = 0;
    parse_error = -EINVAL;
    before = queued;
    assert(mca_vbat_ovp_mon_probe(&p) == -EINVAL && queued == before);
    assert(!publications);
    free(p.data);
    parse_error = 0;
    int errors[] = { -ENODEV, -EIO, -ENOMEM };
    for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); i++) {
        sysfs_error = errors[i];
        before = queued;
        int expected = errors[i] == -ENODEV ? -EPROBE_DEFER : errors[i];
        assert(mca_vbat_ovp_mon_probe(&p) == expected && queued == before);
        assert(published->stopping && !published->sysfs_created);
        free(p.data);
        tests++;
    }
    sysfs_error = 0;
    before = queued;
    assert(!mca_vbat_ovp_mon_probe(&p) && queued == before + 1);
    assert(!published->stopping && published->sysfs_created);
    assert(delay == NORMAL_HEARTBEAT_TIMER_MS);
    mca_vbat_ovp_mon_stop(&p);
    free(p.data);
    tests += 3;
    printf("%u\n", tests);
}
