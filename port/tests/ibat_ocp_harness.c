#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
typedef uint32_t u32;
typedef int64_t s64;
#define __init
#define __exit
#define THIS_MODULE NULL
#define GFP_KERNEL 0
#define PAGE_SIZE 4096
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define max_t(t,a,b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define msecs_to_jiffies(a) (a)
#define scnprintf snprintf
#define MAIN_BUCK_CHARGER 0
#define WIRELESS_ROLE_MASTER 0
#define LOADSW_ROLE_MASTER 0
#define FG_IC_MASTER 0
#define FG_IC_SLAVE 1
#define MCA_EVENT_TYPE_HW_INFO 0
#define MCA_EVENT_IBAT_OCP_CHANGE 1
#define CHARGE_DFX_LOAD_SWITCH_I2C_ERR 2
#define SYSFS_DEV_5 "charger"
#define mca_log_info(...) ((void)0)
#define mca_log_err(...) ((void)0)
#define MODULE_DEVICE_TABLE(...)
#define MODULE_DESCRIPTION(...)
#define MODULE_AUTHOR(...)
#define MODULE_LICENSE(...)
#define module_init(...)
#define module_exit(...)
struct mutex { bool held; };
struct work_struct { int unused; };
struct delayed_work { struct work_struct work; bool initialized; };
struct device_node { int unused; };
struct device { struct device_node *of_node; void *data; };
struct platform_device { struct device dev; };
struct attribute { const char *name; };
struct device_attribute { struct attribute attr; };
struct attribute_group { struct attribute **attrs; };
struct mca_sysfs_attr_info { int sysfs_attr_name; struct device_attribute attr; };
#define mca_sysfs_attr_rw(p,mode,id,name) {id, {{#name}}}
struct of_device_id { const char *compatible; };
struct platform_driver {
    struct { void *owner; const char *name; const struct of_device_id *of_match_table; } driver;
    int (*probe)(struct platform_device *);
    void (*remove)(struct platform_device *), (*shutdown)(struct platform_device *);
};
static int currents[2], current_errors[2], usb = 1, wireless, ffc = 1;
static int usb_error, wireless_error, loadsw_error, events, event_value, dfx, wireless_calls;
static bool loadsw = true, charger_exists = true, charger_available = true;
static u32 dt_fg = 1, dt_thresholds[2] = {9280000,3540000}, dt_support;
static int dt_fg_error, dt_array_error, dt_support_error, sysfs_error, alloc_fail;
static bool dt_fg_present = true;
static int queued, delay, removed, canceled, sysfs_created;
static void *system_wq;
static struct device_node node, charger;
static void mutex_init(struct mutex *m) { m->held = false; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static int platform_fg_ops_get_curr(unsigned role, int *p) {
    assert(role < 2); if (!current_errors[role]) *p = currents[role]; return current_errors[role];
}
static int platform_class_buckchg_ops_get_online(unsigned role, int *p) {
    (void)role; if (!usb_error) *p = usb; return usb_error;
}
static int platform_class_wireless_is_present(unsigned role, int *p) {
    (void)role; wireless_calls++; if (!wireless_error) *p = wireless; return wireless_error;
}
static int platform_class_loadsw_get_present(unsigned role, bool *p) {
    (void)role; if (!loadsw_error) *p = loadsw; return loadsw_error;
}
static int strategy_class_fg_get_fastcharge(void) { return ffc; }
static void mca_charge_mievent_report(int event, void *data, int count) {
    assert(event == CHARGE_DFX_LOAD_SWITCH_I2C_ERR && !data && !count); dfx++;
}
static void mca_event_block_notify(int type, int event, int *value) {
    assert(type == MCA_EVENT_TYPE_HW_INFO && event == MCA_EVENT_IBAT_OCP_CHANGE);
    assert(*value >= 0 && *value <= 3); events++; event_value = *value;
}
static void *of_find_property(struct device_node *n, const char *name, void *length) {
    assert(n == &node && !strcmp(name,"fg_type") && !length); return dt_fg_present ? n : NULL;
}
static int of_property_read_u32(struct device_node *n, const char *name, u32 *p) {
    if (!strcmp(name,"fg_type")) { assert(n == &node); if (!dt_fg_error) *p = dt_fg; return dt_fg_error; }
    assert(n == &charger && !strcmp(name,"support-wls"));
    if (!dt_support_error) *p = dt_support; return dt_support_error;
}
static int of_property_read_u32_array(struct device_node *n, const char *name, u32 *p, size_t count) {
    assert(n == &node && !strcmp(name,"ocp_threshold") && count == 2);
    if (!dt_array_error) memcpy(p,dt_thresholds,sizeof(dt_thresholds)); return dt_array_error;
}
static struct device_node *of_find_compatible_node(void *from, void *type, const char *name) {
    assert(!from && !type && !strcmp(name,"mca,business_charger")); return charger_exists ? &charger : NULL;
}
static bool of_device_is_available(struct device_node *n) { assert(n == &charger); return charger_available; }
static void of_node_put(struct device_node *n) { assert(n == &charger); }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static void *platform_get_drvdata(struct platform_device *p) { return p->dev.data; }
static void platform_set_drvdata(struct platform_device *p, void *data) { p->dev.data = data; }
static struct mca_sysfs_attr_info *mca_sysfs_lookup_attr(const char *name, struct mca_sysfs_attr_info *table, size_t count) {
    for (size_t i = 0; i < count; i++) if (!strcmp(name,table[i].attr.attr.name)) return table+i;
    return NULL;
}
static void mca_sysfs_init_attrs(struct attribute **attrs, struct mca_sysfs_attr_info *table, size_t count) {
    for (size_t i = 0; i < count; i++) attrs[i] = &table[i].attr.attr; attrs[count] = NULL;
}
static int mca_sysfs_create_link_group(const char *class, const char *name, struct device *d, const struct attribute_group *g) {
    (void)d; assert(!strcmp(class,"charger") && !strcmp(name,"ibat_ocp") && g->attrs[0]);
    if (!sysfs_error) sysfs_created++; return sysfs_error;
}
static void mca_sysfs_remove_link_group(const char *class, const char *name, struct device *d, const struct attribute_group *g) {
    (void)d; (void)g; assert(!strcmp(class,"charger") && !strcmp(name,"ibat_ocp")); removed++;
}
#define INIT_DELAYED_WORK(w,fn) ((w)->initialized = true)
static void schedule_delayed_work(struct delayed_work *w, int ms) { assert(w->initialized); queued++; delay=ms; }
static void mod_delayed_work(void *queue, struct delayed_work *w, int ms) { assert(queue == system_wq); schedule_delayed_work(w,ms); }
static void cancel_delayed_work_sync(struct delayed_work *w) { assert(w->initialized); canceled++; }
static void *devm_kzalloc(struct device *d, size_t size, int flags) { (void)d; (void)flags; return alloc_fail ? NULL : calloc(1,size); }
static int dev_err_probe(struct device *d, int ret, const char *message) { (void)d; (void)message; return ret; }
static int platform_driver_register(struct platform_driver *d) { (void)d; return 0; }
static void platform_driver_unregister(struct platform_driver *d) { (void)d; }
static int kstrtoint(const char *text, unsigned base, int *result) {
    char *end; errno=0; long value=strtol(text,&end,base);
    if (errno==ERANGE || value<INT_MIN || value>INT_MAX) return -ERANGE;
    if (end==text || (*end && (*end!='\n' || end[1]))) return -EINVAL;
    *result=value;return 0;
}
/* INSERT_DRIVER */
static struct mca_ibat_ocp_mon_dev fresh(void) {
    return (struct mca_ibat_ocp_mon_dev){.fg_type=MCA_FG_TYPE_PARALLEL,
        .ocp_threshold={9280000,3540000}, .fake_ibat_for_debug=-22,
        .fake_master_ibat_override_ma=-22,.fake_slave_ibat_override_ma=-22,
        .monitor_ibat_ocp_work={.initialized=true}};
}
int main(void) {
    unsigned cases=0;
    struct mca_ibat_ocp_mon_dev info=fresh();
    int edges[2][9]={{0,9279999,9280000,9280001,-9279999,-9280000,-9280001,INT_MIN,INT_MAX},
                     {0,3539999,3540000,3540001,-3539999,-3540000,-3540001,INT_MIN,INT_MAX}};
    for (unsigned i=0;i<9;i++) for (unsigned j=0;j<9;j++) {
        currents[0]=edges[0][i];currents[1]=edges[1][j];
        s64 master=currents[0]<0?-(s64)currents[0]:currents[0];
        s64 slave=currents[1]<0?-(s64)currents[1]:currents[1];
        int expected=(master>9280000?2:0)|(slave>3540000?1:0);
        assert(mca_ibat_mon_get_ibat_ocp_flag(&info)==expected);
        assert(mca_ibat_mon_get_ibat_ocp_status(&info)==expected);
        mca_ibat_ocp_monitor_workfunc(&info.monitor_ibat_ocp_work.work);
        assert(info.batt_ocp_status==expected);cases++;
    }
    assert(!wireless_calls);
    /* An error from any real provider cannot clear protection or emit a false clear. */
    int *failures[]={&usb_error,&loadsw_error,&ffc,&current_errors[0],&current_errors[1]};
    for (unsigned i=0;i<ARRAY_SIZE(failures);i++) for (int e=1;e<=3;e++) {
        int error=e==1?-EIO:e==2?-ENODEV:-EAGAIN; *failures[i]=error;
        info.batt_ocp_status=3;int before=events;
        assert(mca_ibat_mon_get_ibat_ocp_status(&info)==error);
        mca_ibat_ocp_monitor_workfunc(&info.monitor_ibat_ocp_work.work);
        assert(info.batt_ocp_status==3 && events==before && delay==10000);
        *failures[i]=i==2?1:0;cases++;
    }
    currents[0]=10000000;currents[1]=4000000;
    info.fake_master_ibat_override_ma=1;info.fake_slave_ibat_override_ma=1;
    assert(mca_ibat_mon_get_ibat_ocp_flag(&info)==3);cases++;
    currents[0]=currents[1]=0;info.fake_master_ibat_override_ma=10000000;
    assert(mca_ibat_mon_get_ibat_ocp_flag(&info)==2);cases++;
    info.fake_master_ibat_override_ma=-22;info.fake_slave_ibat_override_ma=-22;
    info.fake_ibat_for_debug=13000001;
    mca_ibat_ocp_monitor_workfunc(&info.monitor_ibat_ocp_work.work);assert(info.batt_ocp_status==1);
    info.fake_ibat_for_debug=-22;currents[0]=10000000;
    mca_ibat_ocp_monitor_workfunc(&info.monitor_ibat_ocp_work.work);assert(info.batt_ocp_status==2);cases+=2;
    usb=0;info.has_wireless=true;wireless_error=-EIO;info.batt_ocp_status=2;
    mca_ibat_ocp_monitor_workfunc(&info.monitor_ibat_ocp_work.work);assert(info.batt_ocp_status==2);
    wireless_error=0;wireless=1;assert(mca_ibat_mon_get_ibat_ocp_status(&info)==2);
    wireless=0;assert(!mca_ibat_mon_get_ibat_ocp_status(&info));
    usb=1;ffc=0;assert(!mca_ibat_mon_get_ibat_ocp_status(&info));ffc=1;
    loadsw=false;assert(mca_ibat_mon_get_ibat_ocp_status(&info)==2 && dfx);loadsw=true;cases+=5;
    struct platform_device p={.dev={.of_node=&node}};info.dev=&p.dev;p.dev.data=&info;
    char out[PAGE_SIZE];
    const char *values[]={"0","1","13000001","-22","-1","2147483648","bad"};
    for (unsigned i=0;i<ARRAY_SIZE(ibat_ocp_sysfs_field_tbl);i++) for(unsigned j=0;j<ARRAY_SIZE(values);j++) {
        struct device_attribute *attr=&ibat_ocp_sysfs_field_tbl[i].attr;
        ssize_t ret=ibat_ocp_sysfs_store(&p.dev,attr,values[j],strlen(values[j]));
        assert(j<4?ret==(ssize_t)strlen(values[j]):ret<0);
        assert(ibat_ocp_sysfs_show(&p.dev,attr,out)>0);cases++;
    }
    info.sysfs_created=true;int old_removed=removed,old_canceled=canceled;
    mca_ibat_ocp_mon_shutdown(&p);mca_ibat_ocp_mon_remove(&p);
    assert(removed==old_removed+1 && canceled==old_canceled+2);
    int before=queued;mca_ibat_ocp_monitor_workfunc(&info.monitor_ibat_ocp_work.work);assert(queued==before);
    assert(ibat_ocp_sysfs_store(&p.dev,&ibat_ocp_sysfs_field_tbl[0].attr,"1",1)==-ENODEV);
    assert(ibat_ocp_sysfs_show(&p.dev,&ibat_ocp_sysfs_field_tbl[0].attr,out)==-ENODEV);cases+=4;
    for(int failure=0;failure<9;failure++) {
        struct platform_device probe={.dev={.of_node=&node}};
        alloc_fail=failure==1;dt_fg_error=failure==2?-EIO:0;
        dt_array_error=failure==3?-EINVAL:0;dt_support_error=failure==4?-EIO:0;
        dt_fg=failure==5?0:1;dt_thresholds[0]=failure==6?0:failure==7?UINT_MAX:9280000;
        sysfs_error=failure==8?-ENOMEM:0;before=queued;
        int ret=mca_ibat_ocp_mon_probe(&probe);
        if(failure) { assert(ret<0 && queued==before); }
        else { assert(!ret && queued==before+1);mca_ibat_ocp_mon_remove(&probe); }
        free(probe.dev.data);cases++;
    }
    alloc_fail=sysfs_error=dt_fg_error=dt_array_error=dt_support_error=0;
    dt_thresholds[0]=9280000;dt_fg=1;
    info=fresh();info.dev=&p.dev;
    dt_fg_present=false;assert(mca_ibat_ocp_mon_parse_dt(&info)==-EOPNOTSUPP);
    dt_fg_present=true;
    for (u32 type=0;type<5;type++) if(type!=1) {
        dt_fg=type;assert(mca_ibat_ocp_mon_parse_dt(&info)==-EOPNOTSUPP);cases++;
    }
    dt_fg=1;
    for (unsigned role=0;role<2;role++) {
        u32 saved=dt_thresholds[role];
        dt_thresholds[role]=0;assert(mca_ibat_ocp_mon_parse_dt(&info)==-ERANGE);
        dt_thresholds[role]=UINT_MAX;assert(mca_ibat_ocp_mon_parse_dt(&info)==-ERANGE);
        dt_thresholds[role]=saved;cases+=2;
    }
    charger_exists=false;assert(mca_ibat_ocp_mon_parse_dt(&info)==-ENODEV);charger_exists=true;
    dt_support=2;assert(mca_ibat_ocp_mon_parse_dt(&info)==-EINVAL);
    dt_support=1;assert(!mca_ibat_ocp_mon_parse_dt(&info) && info.has_wireless);
    charger_available=false;assert(!mca_ibat_ocp_mon_parse_dt(&info) && !info.has_wireless);
    charger_available=true;dt_support=0;assert(!mca_ibat_ocp_mon_parse_dt(&info) && !info.has_wireless);
    p.dev.of_node=NULL;assert(mca_ibat_ocp_mon_parse_dt(&info)==-EINVAL);p.dev.of_node=&node;
    struct device_attribute unknown={{"unknown"}};
    assert(ibat_ocp_sysfs_store(&p.dev,&unknown,"1",1)==-EINVAL);
    assert(ibat_ocp_sysfs_show(&p.dev,&unknown,out)==-EINVAL);
    p.dev.data=NULL;
    assert(ibat_ocp_sysfs_show(&p.dev,&ibat_ocp_sysfs_field_tbl[0].attr,out)==-ENODEV);
    cases+=10;
    printf("%u\n",cases);
}
