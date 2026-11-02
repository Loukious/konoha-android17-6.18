// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>

#define CONFIG_XM_POWER_SUPPLY 1
#define CONFIG_MI_THERMAL_MULTI_CHARGE 1
#define CONFIG_HAVE_MULTI_SCREEN 1
#define IS_ENABLED(x) (x)
#define EPROBE_DEFER 517
#define S32_MAX INT32_MAX
#define PAGE_SIZE 4096
#define BOOST_BUFFER_SIZE 128
#define GFP_KERNEL 0
#define FREQ_QOS_MAX 1
#define HZ 100
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define nr_cpu_ids 4
#define cpu_possible(cpu) ((cpu) < nr_cpu_ids)
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < nr_cpu_ids; (cpu)++)
#define pr_err(...) ((void)0)
#define NOTIFY_OK 0
#define PSY_EVENT_PROP_CHANGED 1
#define POWER_SUPPLY_PROP_ONLINE 1
#define MCA_EVENT_TYPE_THERMAL_TEMP 1
#define MCA_EVENT_THERMAL_BOARD_TEMP_CHANGE 2
typedef uint32_t u32;
struct list_head { struct list_head *next, *prev; };
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
#define list_for_each_entry(p, h, member) \
    for (p = container_of((h)->next, __typeof__(*p), member); &(p)->member != (h); \
         p = container_of((p)->member.next, __typeof__(*p), member))
#define list_for_each_entry_safe(p, t, h, member) \
    for (p = container_of((h)->next, __typeof__(*p), member), \
         t = container_of((p)->member.next, __typeof__(*p), member); &(p)->member != (h); \
         p = t, t = container_of((t)->member.next, __typeof__(*t), member))
static void list_add_tail(struct list_head *e, struct list_head *h)
{ e->next = h; e->prev = h->prev; h->prev->next = e; h->prev = e; }
static void list_del(struct list_head *e) { e->next->prev = e->prev; e->prev->next = e->next; }
struct cpufreq_frequency_table { unsigned int frequency; };
struct cpumask { unsigned int bits; };
struct cpufreq_policy {
    int cpu, refs, requests;
    struct cpumask *related_cpus;
    struct cpufreq_frequency_table *freq_table;
    int constraints;
};
struct freq_qos_request { bool active; int value; struct cpufreq_policy *owner; };
struct freq_table { u32 frequency; };
struct cpufreq_device {
    int id; unsigned int cpufreq_state, max_level;
    struct freq_table *freq_table;
    struct cpufreq_policy *policy;
    struct list_head node;
    struct freq_qos_request qos_req;
};
static struct list_head cpufreq_dev_list = { &cpufreq_dev_list, &cpufreq_dev_list };
static pthread_mutex_t cpufreq_list_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t message_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t screen_lock = PTHREAD_MUTEX_INITIALIZER;
#define mutex_lock(p) assert(!pthread_mutex_lock(p))
#define mutex_unlock(p) assert(!pthread_mutex_unlock(p))
#define spin_lock_irqsave(p, flags) do { (flags) = 0; mutex_lock(p); } while (0)
#define spin_unlock_irqrestore(p, flags) do { (void)(flags); mutex_unlock(p); } while (0)
#define cpumask_test_cpu(cpu, mask) ((mask)->bits & (1U << (cpu)))
#define cpufreq_for_each_valid_entry(pos, table) \
    for ((pos) = (table); (pos)->frequency != UINT_MAX; (pos)++) if ((pos)->frequency != UINT_MAX - 1)
static struct cpumask masks[] = { {3}, {12} };
static struct cpufreq_frequency_table table0[] = { {1000}, {3000}, {3000}, {2000}, {UINT_MAX} };
static struct cpufreq_frequency_table table1[] = { {4000}, {1500}, {UINT_MAX} };
static struct cpufreq_policy policies[] = {
    { .cpu = 0, .related_cpus = &masks[0], .freq_table = table0 },
    { .cpu = 2, .related_cpus = &masks[1], .freq_table = table1 },
};
static int missing_cpu = -1, alloc_calls, alloc_fail, allocations, add_calls, add_fail, update_fail;
static unsigned int missing_cpus;
static struct cpufreq_policy *cpufreq_cpu_get(int cpu)
{ if (cpu == missing_cpu || (missing_cpus & (1U << cpu))) return NULL; policies[cpu / 2].refs++; return &policies[cpu / 2]; }
static void cpufreq_cpu_put(struct cpufreq_policy *p) { assert(p->refs > 0); p->refs--; }
static unsigned int cpufreq_table_count_valid_entries(struct cpufreq_policy *p)
{ struct cpufreq_frequency_table *pos; unsigned int count = 0; cpufreq_for_each_valid_entry(pos, p->freq_table) count++; return count; }
static void *kzalloc(size_t size, int flags)
{ (void)flags; if (++alloc_calls == alloc_fail) return NULL; void *p = calloc(1, size); assert(p); allocations++; return p; }
static void *kmalloc_array(size_t n, size_t size, int flags) { return kzalloc(n * size, flags); }
static char *kstrdup(const char *s, int flags) { char *p = kzalloc(strlen(s) + 1, flags); if (p) strcpy(p, s); return p; }
static void kfree(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
static void sort(void *p, size_t n, size_t size, int (*cmp)(const void *, const void *), void *swap)
{ (void)swap; qsort(p, n, size, cmp); }
static int freq_qos_add_request(int *constraints, struct freq_qos_request *r, int type, int value)
{
    (void)type; if (++add_calls == add_fail) return -EIO;
    r->owner = container_of(constraints, struct cpufreq_policy, constraints);
    r->owner->requests++; r->active = true; r->value = value; return 1;
}
static int freq_qos_update_request(struct freq_qos_request *r, int value)
{ assert(r->active); if (update_fail) return -EIO; r->value = value; return 1; }
static int freq_qos_remove_request(struct freq_qos_request *r)
{ assert(r->active && r->owner->requests); r->active = false; r->owner->requests--; return 1; }
struct device { int kobj; bool registered; };
struct device_attribute { int unused; };
struct platform_device { struct device dev; };
struct work_struct { int unused; };
struct delayed_work { bool initialized, queued; void (*fn)(struct work_struct *); };
struct notifier_block { int (*notifier_call)(struct notifier_block *, unsigned long, void *); };
struct mi_thermal_device { struct device *dev; struct delayed_work work; };
static struct mi_thermal_device mi_thermal_dev, mi_powersave_dev;
struct usb_monitor { struct notifier_block psy_nb; int usb_online; };
static struct usb_monitor usb_state;
static struct delayed_work screen_state_dw;
static unsigned int panel_retries;
static int screen_state, screen_light;
static int switch_mode;
static char *board_sensor;
static char board_sensor_temp[128];
#define atomic_set(p, v) (*(p) = (v))
static ssize_t sysfs_emit(char *buf, const char *fmt, ...)
{ va_list args; va_start(args, fmt); int n = vsnprintf(buf, PAGE_SIZE, fmt, args); va_end(args); return n; }
static int kstrtoint(const char *s, int base, int *val)
{
    char *end; errno = 0; long v = strtol(s, &end, base);
    if (end == s || errno || v > INT_MAX || v < INT_MIN) return -EINVAL;
    if (*end == '\n') end++;
    if (*end) return -EINVAL;
    *val = v; return 0;
}
static int scene, board_temp, events, notifications, panel_reg_calls, panel_unreg_calls, panel_fail;
static void mca_smartchg_set_scene(int v) { scene = v; }
static int mca_event_block_notify(int type, int event, int *v)
{ assert(type == MCA_EVENT_TYPE_THERMAL_TEMP && event == MCA_EVENT_THERMAL_BOARD_TEMP_CHANGE); events++; board_temp = *v; return 0; }
static void sysfs_notify(int *kobj, const char *group, const char *attr)
{ (void)group; (void)attr; struct device *dev = container_of(kobj, struct device, kobj); assert(dev->registered); notifications++; }
#define INIT_DELAYED_WORK(w, f) do { (w)->initialized = true; (w)->fn = (f); (w)->queued = false; } while (0)
static int schedule_delayed_work(struct delayed_work *w, int delay)
{ (void)delay; assert(w->initialized); w->queued = true; return 1; }
static int system_queue;
#define system_wq (&system_queue)
static int mod_delayed_work(void *wq, struct delayed_work *w, int delay)
{ (void)wq; return schedule_delayed_work(w, delay); }
static void cancel_delayed_work_sync(struct delayed_work *w) { assert(w->initialized); w->queued = false; }
struct device_node { int refs; };
static struct device_node dt;
static int dt_fail, sensor_fail, node_fail;
static struct device_node *of_find_node_by_name(void *np, const char *name)
{ (void)np; (void)name; if (dt_fail) return NULL; dt.refs++; return &dt; }
static int of_property_read_string(struct device_node *np, const char *key, const char **value)
{ (void)np; (void)key; if (sensor_fail) return -EINVAL; *value = "quiet_therm"; return 0; }
static void of_node_put(struct device_node *np) { assert(np->refs); np->refs--; }
enum panel_event_notifier_tag { PANEL_EVENT_NOTIFICATION_PRIMARY, PANEL_EVENT_NOTIFICATION_SECONDARY };
enum panel_event_notifier_client { PANEL_EVENT_NOTIFIER_CLIENT_THERMAL, PANEL_EVENT_NOTIFIER_CLIENT_THERMAL_SECOND };
enum { DRM_PANEL_EVENT_UNBLANK, DRM_PANEL_EVENT_BLANK, DRM_PANEL_EVENT_BLANK_LP, DRM_PANEL_EVENT_FPS_CHANGE };
struct panel_event_notification { int notif_type; struct { bool early_trigger; } notif_data; };
struct drm_panel { int unused; };
static struct drm_panel panel, *prim_panel, *sec_panel;
static void *cookie, *cookie_sec;
#define ERR_PTR(err) ((void *)(intptr_t)(err))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
static int of_count_phandle_with_args(struct device_node *n, const char *p, void *args)
{ (void)n; (void)p; (void)args; return 1; }
static struct device_node *of_parse_phandle(struct device_node *n, const char *p, int i)
{ (void)n; (void)p; (void)i; if (node_fail) return NULL; dt.refs++; return &dt; }
static struct drm_panel *of_drm_find_panel(struct device_node *n)
{ (void)n; return panel_fail == 1 ? ERR_PTR(-EPROBE_DEFER) : &panel; }
static void *panel_event_notifier_register(enum panel_event_notifier_tag tag,
    enum panel_event_notifier_client client, struct drm_panel *p,
    void (*fn)(enum panel_event_notifier_tag, struct panel_event_notification *, void *), void *data)
{
    (void)client; (void)p; (void)fn; (void)data; panel_reg_calls++;
    return panel_fail == 2 ? ERR_PTR(-EBUSY) : (void *)(uintptr_t)(tag + 1);
}
static void panel_event_notifier_unregister(void *p) { assert(p && !IS_ERR(p)); panel_unreg_calls++; }
struct power_supply_desc { const char *name; };
struct power_supply { struct power_supply_desc *desc; };
union power_supply_propval { int intval; };
static struct power_supply_desc usb_desc = { "usb" };
static struct power_supply usb_psy = { &usb_desc };
static int psy_refs, psy_online, psy_error, psy_absent, notifier_registered, notifier_fail;
static struct power_supply *power_supply_get_by_name(const char *name)
{ assert(!strcmp(name, "usb")); if (psy_absent) return NULL; psy_refs++; return &usb_psy; }
static void power_supply_put(struct power_supply *psy) { assert(psy == &usb_psy && psy_refs); psy_refs--; }
static int power_supply_get_property(struct power_supply *psy, int prop, union power_supply_propval *val)
{ (void)psy; (void)prop; if (psy_error) return -EIO; val->intval = psy_online; return 0; }
static int power_supply_reg_notifier(struct notifier_block *nb)
{ assert(mi_thermal_dev.work.initialized && nb->notifier_call); if (notifier_fail) return -EIO; notifier_registered++; return 0; }
static void power_supply_unreg_notifier(struct notifier_block *nb)
{ (void)nb; assert(notifier_registered); notifier_registered--; }
static struct device sysfs_devices[2];
static int devices, device_calls, device_fail;
static void *thermal_message_groups, *powersave_groups;
static struct device *thermal_device_register(const char *name, void *groups)
{
    (void)groups; if (++device_calls == device_fail) return ERR_PTR(-ENOMEM);
    int i = !strcmp(name, "power_save"); assert(!sysfs_devices[i].registered);
    sysfs_devices[i].registered = true; devices++; return &sysfs_devices[i];
}
static void device_unregister(struct device *d)
{
    assert(d && d->registered && !notifier_registered);
    assert(!mi_thermal_dev.work.queued && !screen_state_dw.queued);
    assert(!cookie && !cookie_sec);
    d->registered = false; devices--;
}
static int dev_err_probe(struct device *dev, int ret, const char *msg)
{ (void)dev; (void)msg; return ret; }

/* ACTUAL_FUNCTIONS */

static int cases;
static void clean(void)
{
    assert(!allocations && !devices && !dt.refs && !psy_refs && !notifier_registered);
    assert(!policies[0].refs && !policies[1].refs);
    assert(!policies[0].requests && !policies[1].requests);
    assert(cpufreq_dev_list.next == &cpufreq_dev_list);
    cases++;
}
static void faults_reset(void)
{
    alloc_calls = add_calls = device_calls = 0;
    alloc_fail = add_fail = device_fail = dt_fail = sensor_fail = notifier_fail = 0;
    missing_cpu = -1; missing_cpus = 0; update_fail = panel_fail = psy_error = psy_absent = node_fail = 0;
}
static struct cpufreq_device *first(void)
{ return container_of(cpufreq_dev_list.next, struct cpufreq_device, node); }
static void *writer(void *argument)
{
    unsigned int cpu = (uintptr_t)argument;
    for (int i = 0; i < 1000; i++) assert(!cpu_limits_set_level(cpu, (i & 1) ? 2000 : 3000));
    return NULL;
}
int main(void)
{
    struct platform_device pdev = {0}; char out[PAGE_SIZE]; ssize_t ret;
    clean();
    assert(!cpu_thermal_init());
    assert(policies[0].requests == 1 && policies[1].requests == 1);
    assert(policies[0].refs == 1 && policies[1].refs == 1);
    assert(first()->max_level == 2 && first()->freq_table[2].frequency == 1000);
    assert(!cpu_limits_set_level(1, 2500)); assert(first()->qos_req.value == 2000);
    update_fail = 1; assert(cpu_limits_set_level(0, 1000) == -EIO);
    assert(first()->cpufreq_state == 1 && first()->qos_req.value == 2000);
    update_fail = 0; assert(!cpu_limits_set_level(0, 500)); assert(first()->qos_req.value == 1000);
    assert(!cpu_limits_set_level(0, UINT_MAX)); assert(first()->qos_req.value == 3000);
    assert(cpu_limits_set_level(4, 1000) == -EINVAL);
    assert(cpu_limits_set_level(0, 0) == -EINVAL);
    const char *bad[] = { "cpu9 1000", "cpu0 0", "cpu0 1000 junk", "garbage", "cpu-1 1000", "cpu0 -1" };
    for (unsigned int i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        assert(cpu_limits_store(NULL, NULL, bad[i], strlen(bad[i])) < 0); cases++;
    }
    assert(cpu_limits_store(NULL, NULL, "cpu1 2000\n", 10) == 10);
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, writer, (void *)i));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    destroy_thermal_cpu(); clean();
    for (int i = 1; i <= 4; i++) {
        faults_reset(); alloc_fail = i; assert(cpu_thermal_init() == -ENOMEM); clean();
    }
    for (int i = 1; i <= 2; i++) {
        faults_reset(); add_fail = i; assert(cpu_thermal_init() == -EIO); clean();
    }
    faults_reset(); missing_cpu = 0; assert(!cpu_thermal_init());
    assert(policies[0].requests == 1 && !cpu_limits_set_level(0, 2000));
    destroy_thermal_cpu(); clean();
    faults_reset(); missing_cpus = 12; assert(cpu_thermal_init() == -EPROBE_DEFER); clean();
    faults_reset(); policies[1].freq_table = NULL; assert(cpu_thermal_init() == -ENODEV); clean();
    policies[1].freq_table = table1;
    faults_reset(); table1[0].frequency = 0; assert(cpu_thermal_init() == -EINVAL); clean();
    table1[0].frequency = 4000;
    faults_reset();
    assert(message_string_store(board_sensor_temp, "%s%n%p\n", 7) == 7);
    assert(message_string_show(out, board_sensor_temp) == 7 && !strcmp(out, "%s%n%p\n"));
    assert(message_string_store(board_sensor_temp, "x", 128) == -E2BIG);
    assert(thermal_sconfig_store(NULL, NULL, "19\n", 3) == 3 && scene == 19 && switch_mode == 19);
    assert(thermal_sconfig_store(NULL, NULL, "bad", 3) == -EINVAL && switch_mode == 19);
    assert(thermal_board_sensor_temp_store(NULL, NULL, "42000\n", 6) == 6 && board_temp == 42000);
    ret = events; assert(thermal_board_sensor_temp_store(NULL, NULL, "%n", 2) < 0 && ret == events);
    cases += 7;
    for (int i = 1; i <= 2; i++) {
        faults_reset(); device_fail = i; assert(mi_thermal_probe(&pdev) == -ENOMEM); clean();
    }
    faults_reset(); notifier_fail = 1; assert(mi_thermal_probe(&pdev) == -EIO); clean();
    faults_reset(); dt_fail = 1; assert(!mi_thermal_probe(&pdev));
    assert(!strcmp(board_sensor, "invalid")); mi_thermal_remove(&pdev); clean();
    faults_reset(); sensor_fail = 1; assert(!mi_thermal_probe(&pdev));
    assert(!strcmp(board_sensor, "invalid")); mi_thermal_remove(&pdev); clean();
    faults_reset(); missing_cpus = 12; assert(mi_thermal_probe(&pdev) == -EPROBE_DEFER); clean();
    for (int i = 1; i <= 5; i++) {
        faults_reset(); alloc_fail = i; assert(mi_thermal_probe(&pdev) == -ENOMEM); clean();
    }
    faults_reset(); assert(!mi_thermal_probe(&pdev));
    assert(devices == 2 && notifier_registered && mi_thermal_dev.work.queued && screen_state_dw.queued);
    psy_online = 1; usb_online_work(NULL); assert(usb_state.usb_online == 1 && !psy_refs);
    ret = notifications; psy_error = 1; usb_online_work(NULL); assert(notifications == ret && !psy_refs);
    psy_error = 0; psy_absent = 1; usb_online_work(NULL); assert(!psy_refs); psy_absent = 0;
    mi_thermal_dev.work.queued = false;
    assert(!usb_online_callback(NULL, PSY_EVENT_PROP_CHANGED, &usb_psy) && mi_thermal_dev.work.queued);
    panel_fail = 2; screen_state_check(NULL); assert(!cookie && !cookie_sec && !dt.refs);
    panel_fail = 1; screen_state_check(NULL); assert(!cookie && !cookie_sec && !dt.refs);
    panel_fail = 0; screen_state_check(NULL); assert(cookie && cookie_sec && !dt.refs);
    int registered = panel_reg_calls; screen_state_check(NULL); assert(panel_reg_calls == registered);
    struct panel_event_notification notification = { .notif_type = DRM_PANEL_EVENT_UNBLANK };
    screen_state_for_thermal_callback(PANEL_EVENT_NOTIFICATION_PRIMARY, &notification, NULL);
    assert(screen_state == 1);
    notification.notif_type = DRM_PANEL_EVENT_BLANK_LP; notification.notif_data.early_trigger = true;
    screen_state_for_thermal_callback(PANEL_EVENT_NOTIFICATION_PRIMARY, &notification, NULL);
    assert(screen_state == 1);
    notification.notif_data.early_trigger = false;
    screen_state_for_thermal_callback(PANEL_EVENT_NOTIFICATION_PRIMARY, &notification, NULL);
    assert(screen_state == 0);
    notification.notif_type = DRM_PANEL_EVENT_UNBLANK;
    screen_state_for_thermal_callback(PANEL_EVENT_NOTIFICATION_SECONDARY, &notification, NULL);
    assert(screen_state == 1);
    notification.notif_type = DRM_PANEL_EVENT_BLANK;
    screen_state_for_thermal_callback(PANEL_EVENT_NOTIFICATION_PRIMARY, &notification, NULL);
    assert(screen_state == 1);
    mi_thermal_remove(&pdev); assert(!cookie && !cookie_sec && panel_unreg_calls == 2);
    assert(!screen_state_dw.queued && !mi_thermal_dev.work.queued); clean(); cases += 13;
    printf("{\"cases\":%d,\"concurrent_qos_writes\":4000,\"result\":\"passed\"}\n", cases);
    return 0;
}
