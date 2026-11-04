#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef int64_t s64;
typedef uint64_t resource_size_t;
typedef int64_t ktime_t;
#define __iomem
#define GFP_KERNEL 0
#define PAGE_SIZE 4096
#define MAX_ERRNO 4095
#define EPROBE_DEFER 517
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define READ_ONCE(x) (x)
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define EXPORT_SYMBOL_GPL(n)
#define MODULE_LICENSE(n)
#define MODULE_DESCRIPTION(n)
#define MODULE_ALIAS(n)
#define module_platform_driver(n)
#define THIS_MODULE ((void *)1)
#define pr_err(...) ((void)0)
#define dev_err(...) ((void)0)
static unsigned warnings;
#define pr_warn(...) (++warnings)
struct mutex { pthread_mutex_t lock; };
#define DEFINE_MUTEX(n) struct mutex n = { PTHREAD_MUTEX_INITIALIZER }
#define mutex_init(p) assert(!pthread_mutex_init(&(p)->lock, NULL))
#define mutex_lock(p) assert(!pthread_mutex_lock(&(p)->lock))
#define mutex_unlock(p) assert(!pthread_mutex_unlock(&(p)->lock))
static unsigned allocations, alloc_calls, fail_alloc;
static void *test_calloc(size_t n, size_t size)
{
    if (++alloc_calls == fail_alloc) return NULL;
    void *p = calloc(n, size); assert(p); ++allocations; return p;
}
#define kcalloc(n,s,g) test_calloc(n,s)
static void kfree(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
static u32 *firmware;
static size_t firmware_size;
static unsigned mmio_reads, change_header_at;
static u32 readl_relaxed(const void *p)
{
    assert((const char *)p >= (const char *)firmware);
    assert((const char *)p + 4 <= (const char *)firmware + firmware_size);
    if (++mmio_reads == change_header_at) firmware[0] ^= 1;
    u32 value; memcpy(&value, p, 4); return value;
}
#define readl(p) readl_relaxed(p)
struct resource { resource_size_t size; };
static resource_size_t resource_size(struct resource *r) { return r->size; }
struct device { void *drvdata; void *of_node; };
struct platform_device { struct device dev; };
static void *devm_allocation;
static void *devm_kzalloc(struct device *d, size_t s, unsigned flags)
{
    assert(!devm_allocation); devm_allocation = test_calloc(1, s); return devm_allocation;
}
static int map_error;
static struct resource resource;
static void *devm_platform_get_and_ioremap_resource(struct platform_device *p, int index, struct resource **r)
{
    assert(index == 0); *r = &resource; return map_error ? ERR_PTR(map_error) : firmware;
}
static void *dev_get_drvdata(struct device *d) { return d->drvdata; }
static void platform_set_drvdata(struct platform_device *p, void *d) { p->dev.drvdata = d; }
static void *platform_get_drvdata(struct platform_device *p) { return p->dev.drvdata; }
struct of_device_id { const char *compatible; const void *data; };
static unsigned match_index;
static bool missing_match;
static const struct of_device_id *of_match_node(const struct of_device_id *table, void *node)
{
    return missing_match ? NULL : &table[match_index];
}
struct qmp { bool live; };
static struct qmp qmp;
static int qmp_get_error, qmp_send_error;
static unsigned qmp_gets, qmp_puts, qmp_sends;
static char qmp_message[128];
static struct qmp *qmp_get(struct device *dev)
{
    if (qmp_get_error) return ERR_PTR(qmp_get_error);
    assert(!qmp.live); qmp.live = true; ++qmp_gets; return &qmp;
}
static void qmp_put(struct qmp *p) { assert(p == &qmp && p->live); p->live = false; ++qmp_puts; }
static int qmp_send(struct qmp *p, const char *fmt, ...)
{
    assert(p == &qmp && p->live && !strcmp(fmt, "%s"));
    va_list args; va_start(args, fmt); vsnprintf(qmp_message, sizeof(qmp_message), fmt, args); va_end(args);
    ++qmp_sends; return qmp_send_error;
}
static bool stats_debug, system_slept, subsystem_slept, set_aoss;
static unsigned stats_queries;
static void subsystem_sleep_debug_enable(bool enable) { stats_debug = enable; }
static bool has_system_slept(bool *aoss) { assert(aoss && !*aoss); *aoss = set_aoss; ++stats_queries; return system_slept; }
static bool has_subsystem_slept(void) { ++stats_queries; return subsystem_slept; }
static ktime_t now;
static ktime_t ktime_get_boottime(void) { return now; }
static s64 ktime_ms_delta(ktime_t a, ktime_t b) { return a - b; }
static int kstrtos32(const char *text, unsigned base, int *value)
{
    char *end; errno = 0; long n = strtol(text, &end, base);
    if (end == text || errno || n < INT_MIN || n > INT_MAX || (*end && strcmp(end, "\n"))) return -EINVAL;
    *value = n; return 0;
}
static int scnprintf(char *out, size_t size, const char *fmt, ...)
{
    va_list args; va_start(args, fmt); int n = vsnprintf(out, size, fmt, args); va_end(args);
    return n < (int)size ? n : size - 1;
}
struct seq_file { void *private; char buf[65536]; size_t used; };
static void seq_printf(struct seq_file *s, const char *fmt, ...)
{
    va_list args; va_start(args, fmt); int n = vsnprintf(s->buf+s->used, sizeof(s->buf)-s->used, fmt, args); va_end(args);
    assert(n >= 0 && n < (int)(sizeof(s->buf)-s->used)); s->used += n;
}
static void seq_puts(struct seq_file *s, const char *text) { seq_printf(s, "%s", text); }
struct inode { void *i_private; };
struct file { int unused; };
static int single_open(struct file *f, int (*show)(struct seq_file *, void *), void *data) { return 0; }
static int seq_read(void) { return 0; }
static int seq_lseek(void) { return 0; }
static int single_release(void) { return 0; }
struct file_operations { void *owner; int (*open)(struct inode *, struct file *); void *read, *llseek, *release; };
struct device_attribute { unsigned id; };
#define DEVICE_ATTR_RW(n) struct device_attribute dev_attr_##n
static unsigned sysfs_calls, sysfs_fail, sysfs_live;
static bool enable_during_publication;
static int device_create_file(struct device *, struct device_attribute *);
static void device_remove_file(struct device *d, struct device_attribute *a) { assert(sysfs_live); --sysfs_live; }
struct dentry { bool live; };
static struct dentry debug_entries[3];
static unsigned debug_calls, debug_fail, debug_null, debug_live;
static struct dentry *new_debug_entry(void)
{
    unsigned index = debug_calls++;
    if (debug_calls == debug_fail) return ERR_PTR(-ENOMEM);
    if (debug_calls == debug_null) return NULL;
    assert(index < 3); debug_entries[index].live = true; ++debug_live; return &debug_entries[index];
}
static struct dentry *debugfs_create_dir(const char *name, struct dentry *parent) { return new_debug_entry(); }
static struct dentry *debugfs_create_file(const char *name, unsigned mode, struct dentry *parent, void *data, const void *fops)
{
    assert(parent && parent->live); return new_debug_entry();
}
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed = PTHREAD_COND_INITIALIZER;
static bool block_removal, removal_entered, permit_removal;
static void debugfs_remove_recursive(struct dentry *d)
{
    if (IS_ERR_OR_NULL(d)) return;
    pthread_mutex_lock(&gate);
    if (block_removal) {
        removal_entered = true; pthread_cond_broadcast(&gate_changed);
        while (!permit_removal) pthread_cond_wait(&gate_changed, &gate);
    }
    pthread_mutex_unlock(&gate);
    debug_live = 0; memset(debug_entries, 0, sizeof(debug_entries));
}
#define DEFINE_DEBUGFS_ATTRIBUTE(n,get,set,fmt) static int n
struct dev_pm_ops { int (*suspend)(struct device *), (*resume)(struct device *); };
struct platform_driver {
    int (*probe)(struct platform_device *); void (*remove)(struct platform_device *);
    struct { const char *name; const struct of_device_id *of_match_table; const struct dev_pm_ops *pm; } driver;
};
/* DRIVER */
static int device_create_file(struct device *dev, struct device_attribute *attr)
{
    struct vx_platform_data *pd = dev->drvdata;
    assert(pd && pd->detect_time_ms == DEFAULT_DEBUG_TIME && pd->timer_ms == DEFAULT_TIMER && qmp.live);
    if (++sysfs_calls == sysfs_fail) return -ENOMEM;
    ++sysfs_live;
    if (enable_during_publication && attr == &dev_attr_debug_enable) assert(debug_enable_store(dev, attr, "1", 1) == 1);
    return 0;
}
static void reset(void)
{
    assert(!g_pd && !qmp.live && !allocations && !debug_live && !sysfs_live);
    alloc_calls = fail_alloc = debug_calls = debug_fail = debug_null = sysfs_calls = sysfs_fail = 0;
    mmio_reads = change_header_at = warnings = qmp_sends = stats_queries = 0;
    map_error = qmp_get_error = qmp_send_error = 0;
    missing_match = enable_during_publication = stats_debug = system_slept = subsystem_slept = set_aoss = false;
    match_index = 3; resource.size = firmware_size = 1024;
    memset(firmware, 0, 1024); now = 0;
}
static void release_devm(void)
{
    if (devm_allocation) {
        struct vx_platform_data *pd = devm_allocation;
        /* The first allocation failure does not construct a mutex. */
        pthread_mutex_destroy(&pd->lock.lock); kfree(devm_allocation); devm_allocation = NULL;
    }
}
static struct vx_platform_data pd_local(void)
{
    struct vx_platform_data pd = {.base=firmware, .size=1024, .n_cxpc_drv=26, .n_aoss_drv=15,
        .cxpc_drvs=drv_names_tuna[0], .aoss_drvs=drv_names_tuna[1], .timer_ms=DEFAULT_TIMER,
        .detect_time_ms=DEFAULT_DEBUG_TIME, .qmp=&qmp};
    return pd;
}
static void make_log(struct vx_platform_data *pd, unsigned mode, unsigned lines, unsigned shift)
{
    memset(firmware, 0, 1024); mmio_reads=0;
    firmware[0] = mode | (lines << 8); firmware[1] = 1000 | (shift << 16) | (5U << 24);
    size_t drivers = mode == MODE_AOSS ? pd->n_aoss_drv : pd->n_cxpc_drv;
    size_t words = DIV_ROUND_UP(drivers, 4), offset=2;
    for (unsigned i=0; i<lines && offset+1+words <= 256; i++) {
        firmware[offset++] = i+1;
        for (unsigned j=0; j<words; j++) firmware[offset++] = 0x04030201;
    }
}
static atomic_bool export_done;
static struct platform_device device;
static void *remove_thread(void *unused) { vx_remove(&device); return NULL; }
static void *export_thread(void *unused) { trigger_dump_enable(); atomic_store(&export_done, true); return NULL; }
int main(void)
{
    unsigned cases=0; firmware=calloc(256, 4); assert(firmware); reset();
    trigger_dump_enable(); trigger_dump_disable(); ++cases;
    /* Every SoC's two driver tables are bounded and the empty sentinel is present. */
    for (unsigned soc=0; soc<6; soc++) {
        reset(); match_index=soc; assert(!vx_probe(&device));
        struct vx_platform_data *pd=device.dev.drvdata;
        assert(pd->n_cxpc_drv && pd->n_cxpc_drv<MAX_DRV_NAMES && pd->n_aoss_drv && pd->n_aoss_drv<MAX_DRV_NAMES);
        assert(g_pd==pd && !qmp_sends && !pd->debug_enable && !pd->debug_dump_enable);
        char buf[64]; assert(debug_time_ms_show(&device.dev, NULL, buf)>0 && !strcmp(buf,"10000\n"));
        assert(set_timer_ms_show(&device.dev,NULL,buf)>0 && !strcmp(buf,"5000\n"));
        assert(debug_enable_show(&device.dev,NULL,buf)>0 && !strcmp(buf,"0\n"));
        assert(!vx_suspend(&device.dev) && !vx_resume(&device.dev) && !qmp_sends && !stats_queries);
        trigger_dump_enable(); assert(pd->debug_dump_enable); trigger_dump_disable(); assert(!pd->debug_dump_enable);
        vx_remove(&device); release_devm(); assert(!g_pd && !qmp.live && !stats_debug); ++cases;
    }
    /* Probe fault unwind, including sysfs use while later publication fails. */
    for (unsigned stage=0; stage<13; stage++) {
        reset();
        if (stage==0) fail_alloc=1;
        else if (stage==1) map_error=-EIO;
        else if (stage==2) resource.size=4;
        else if (stage==3) missing_match=true;
        else if (stage==4) qmp_get_error=-EPROBE_DEFER;
        else if (stage<8) debug_fail=stage-4;
        else if (stage<11) debug_null=stage-7;
        else { sysfs_fail=stage-10; enable_during_publication=true; }
        assert(vx_probe(&device)<0); release_devm();
        assert(!g_pd && !allocations && !debug_live && !sysfs_live && !qmp.live && !stats_debug); ++cases;
    }
    reset(); sysfs_fail=3; enable_during_publication=true;
    assert(vx_probe(&device)==-ENOMEM && !stats_debug); release_devm(); ++cases;
    /* Bound every firmware mode/count/shift combination before payload access. */
    for (unsigned mode_index=0; mode_index<4; mode_index++) {
        unsigned mode=(unsigned[]){MODE_CXPC,MODE_AOSS,MODE_DDR,0x99}[mode_index];
        for (unsigned lines=0; lines<256; lines++) for (unsigned shift=0; shift<34; shift++) {
            struct vx_platform_data pd=pd_local(); struct vx_log log;
            make_log(&pd,mode,lines,shift); unsigned char original[1024]; memcpy(original,firmware,1024);
            size_t n=mode==MODE_AOSS?15:26, extent=8+lines*(4+DIV_ROUND_UP(n,4)*4);
            int rc=read_vx_data(&pd,&log);
            if (mode==0x99) assert(rc==-ENODEV);
            else if (!lines || shift>=32) assert(rc==-EINVAL);
            else if (extent>1024) assert(rc==-EMSGSIZE);
            else {
                assert(!rc && log.loglines==(int)lines);
                assert(log.data[lines-1].ts == (u64)lines<<shift);
                for (unsigned i=0;i<lines;i++) for(unsigned j=0;j<n;j++) assert(log.data[i].drv_vx[j]==1+j%4);
                free_vx_data(&log);
            }
            assert(!allocations && !memcmp(original,firmware,1024)); ++cases;
        }
    }
    reset(); struct vx_platform_data pd=pd_local(); struct vx_log log;
    for (unsigned bytes=0;bytes<40;bytes++) {
        make_log(&pd,MODE_CXPC,1,1); pd.size=firmware_size=bytes;
        assert(read_vx_data(&pd,&log)==-EMSGSIZE && !allocations); ++cases;
    }
    pd.size=firmware_size=1024;
    make_log(&pd,MODE_AOSS,2,31); firmware[2]=0;
    assert(!read_vx_data(&pd,&log) && !log.loglines); free_vx_data(&log); ++cases;
    make_log(&pd,MODE_AOSS,2,31); firmware[2]=UINT32_MAX;
    assert(!read_vx_data(&pd,&log) && log.data[0].ts==((u64)UINT32_MAX<<31));
    free_vx_data(&log); ++cases;
    for (unsigned fail=1;fail<=4;fail++) {
        alloc_calls=0; fail_alloc=fail; make_log(&pd,MODE_CXPC,3,0);
        assert(read_vx_data(&pd,&log)==-ENOMEM && !allocations && !log.data && !log.loglines); ++cases;
    }
    fail_alloc=0; make_log(&pd,MODE_CXPC,1,0); change_header_at=4;
    assert(read_vx_data(&pd,&log)==-EAGAIN && !allocations); change_header_at=0; ++cases;
    make_log(&pd,MODE_CXPC,1,0); firmware[1]=0;
    assert(read_vx_data(&pd,&log)==-ENOENT); firmware[0]=0;
    assert(read_vx_data(&pd,&log)==-ENOENT); cases+=2;
    /* Show and blocking-driver checks use each log mode's own table. */
    reset(); assert(!vx_probe(&device)); struct vx_platform_data *live=device.dev.drvdata;
    make_log(live,MODE_AOSS,1,0); struct seq_file seq={.private=live};
    assert(!vx_show(&seq,NULL) && strstr(seq.buf,"AOSS") && strstr(seq.buf,"WPSS"));
    assert(allocations==1); vx_check_drv(live); assert(warnings==live->n_aoss_drv && !qmp_sends); ++cases;
    for (const char **x=(const char*[]){"0","-1","junk","2147483648",NULL};*x;x++) {
        assert(debug_time_ms_store(&device.dev,NULL,*x,strlen(*x))==-EINVAL);
        assert(set_timer_ms_store(&device.dev,NULL,*x,strlen(*x))==-EINVAL); ++cases;
    }
    assert(debug_time_ms_store(&device.dev,NULL,"5",1)==1);
    assert(set_timer_ms_store(&device.dev,NULL,"7",1)==1);
    assert(debug_enable_store(&device.dev,NULL,"2",1)==-EINVAL);
    assert(trigger_dump_enable_set(live,2)==-EINVAL);
    assert(debug_enable_store(&device.dev,NULL,"1",1)==1 && stats_debug);
    now=100; assert(!vx_suspend(&device.dev)); now=102; assert(!vx_resume(&device.dev) && !stats_queries);
    now=110; subsystem_slept=true; assert(!vx_resume(&device.dev) && live->monitor_status==CXPC_MSG);
    assert(!vx_suspend(&device.dev) && strstr(qmp_message,"type: cxpc"));
    now=120; set_aoss=true; warnings=0; assert(!vx_resume(&device.dev) && live->monitor_status==AOSS_SHUTDOWN_MSG);
    assert(warnings==live->n_aoss_drv && !strstr(qmp_message,"do_crash_dump"));
    assert(!vx_suspend(&device.dev) && strstr(qmp_message,"type: rbsc"));
    now=130; system_slept=true; assert(!vx_resume(&device.dev) && live->monitor_status==DISABLE_MSG && strstr(qmp_message,"log_once"));
    live->debug_dump_enable=true; trigger_dump(live); assert(strstr(qmp_message,"do_crash_dump") && strstr(qmp_message,"tmr: 7"));
    assert(trigger_dump_enable_set(live,0)==0); u64 val=9; assert(!trigger_dump_enable_get(live,&val) && val==0);
    unsigned sends=qmp_sends; live->monitor_status=77; sys_pm_vx_send_msg(live); assert(qmp_sends==sends);
    qmp_send_error=-EIO; live->monitor_status=CXPC_MSG; sys_pm_vx_send_msg(live); ++cases;
    /* An export racing teardown must wait, then observe the withdrawn instance. */
    block_removal=true; removal_entered=permit_removal=false; atomic_store(&export_done,false);
    pthread_t remover, caller; assert(!pthread_create(&remover,NULL,remove_thread,NULL));
    pthread_mutex_lock(&gate); while(!removal_entered) pthread_cond_wait(&gate_changed,&gate); pthread_mutex_unlock(&gate);
    assert(!pthread_create(&caller,NULL,export_thread,NULL)); usleep(20000); assert(!atomic_load(&export_done));
    pthread_mutex_lock(&gate); permit_removal=true; pthread_cond_broadcast(&gate_changed); pthread_mutex_unlock(&gate);
    assert(!pthread_join(remover,NULL) && !pthread_join(caller,NULL) && atomic_load(&export_done));
    block_removal=false; release_devm(); assert(!g_pd && !allocations && !qmp.live && !stats_debug); ++cases;
    assert(qmp_gets==qmp_puts); free(firmware); printf("%u\n",cases);
}
