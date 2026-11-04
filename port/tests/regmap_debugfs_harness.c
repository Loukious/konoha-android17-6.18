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
typedef unsigned long long u64;
typedef int64_t loff_t;
#define __user
#define GFP_KERNEL 0
#define PAGE_SIZE 4096
#define MAX_PAGE_ORDER 11
#define S64_MAX INT64_MAX
#define min(a,b) ((a)<(b)?(a):(b))
#define WARN_ON(c) ({ bool _c=(c);assert(!_c);_c; })
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((int)(intptr_t)(p))
static int PTR_ERR_OR_ZERO(const void *p) { return IS_ERR(p)?PTR_ERR(p):0; }
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define EXPORT_SYMBOL(n)
#define MODULE_DESCRIPTION(n)
#define MODULE_LICENSE(n)
#define THIS_MODULE ((void *)1)
#define pr_err(...) ((void)0)
#define dev_err(...) ((void)0)
struct mutex { pthread_mutex_t lock; };
#define DEFINE_MUTEX(n) struct mutex n = { PTHREAD_MUTEX_INITIALIZER }
static void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->lock,NULL)); }
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->lock)); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->lock)); }
static void mutex_destroy(struct mutex *m) { assert(!pthread_mutex_destroy(&m->lock)); }
struct list_head { struct list_head *next,*prev; };
#define LIST_HEAD(n) struct list_head n = {&n,&n}
#define INIT_LIST_HEAD(h) (*(h)=(struct list_head){h,h})
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
static bool list_empty(struct list_head *h) { return h->next==h; }
static void list_add(struct list_head *n,struct list_head *h) { n->next=h->next;n->prev=h;h->next->prev=n;h->next=n; }
static void list_add_tail(struct list_head *n,struct list_head *h) { n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n; }
static void list_del(struct list_head *n) { n->next->prev=n->prev;n->prev->next=n->next; }
#define list_entry(p,t,m) container_of(p,t,m)
#define list_first_entry(h,t,m) list_entry((h)->next,t,m)
#define list_for_each_entry(p,h,m) for(p=list_entry((h)->next,typeof(*p),m);&p->m!=(h);p=list_entry(p->m.next,typeof(*p),m))
#define list_for_each_entry_safe(p,n,h,m) for(p=list_entry((h)->next,typeof(*p),m),n=list_entry(p->m.next,typeof(*p),m);&p->m!=(h);p=n,n=list_entry(n->m.next,typeof(*n),m))
static unsigned allocations,alloc_calls,fail_alloc;
static void *kzalloc(size_t n,unsigned flags)
{
    if(++alloc_calls==fail_alloc)return NULL;
    void *p=calloc(1,n);assert(p);++allocations;return p;
}
static void kfree(void *p) { if(p){assert(allocations);--allocations;free(p);} }
static bool copy_error;
static unsigned copy_to_user(void *out,const void *in,size_t n) { if(copy_error)return 1;memcpy(out,in,n);return 0; }
static unsigned copy_from_user(void *out,const void *in,size_t n) { return copy_to_user(out,in,n); }
static int scnprintf(char *out,size_t size,const char *fmt,...)
{
    va_list args;va_start(args,fmt);int n=vsnprintf(out,size,fmt,args);va_end(args);return n<(int)size?n:(int)size-1;
}
static int kstrtoul(const char *text,unsigned base,unsigned long *value)
{
    char *end;errno=0;*value=strtoul(text,&end,base);return errno||end==text||(*end&&strcmp(end,"\n"))?-EINVAL:0;
}
#define TAINT_USER 1
#define LOCKDEP_STILL_OK 1
static void add_taint(unsigned a,unsigned b) {}
struct device { unsigned refs; };
static struct device *get_device(struct device *dev) { assert(dev&&dev->refs);++dev->refs;return dev; }
static void put_device(struct device *dev) { assert(dev->refs>1);--dev->refs; }
struct inode { void *i_private; };
struct file { void *private_data; };
static int simple_open(struct inode *i,struct file *f) { f->private_data=i->i_private;return 0; }
static int default_llseek(void) { return 0; }
struct file_operations { void *owner;int (*open)(struct inode*,struct file*);ssize_t (*read)(struct file*,char*,size_t,loff_t*);ssize_t (*write)(struct file*,const char*,size_t,loff_t*);void *llseek; };
#define DEFINE_DEBUGFS_ATTRIBUTE(n,get,set,fmt) static const struct file_operations n={.owner=THIS_MODULE}
struct dentry { bool live,protected;unsigned index;void *data;const struct file_operations *fops; };
static struct dentry root_directory={.live=true},debug_files[3];
static unsigned debug_calls,debug_fail,debug_null,debug_live;
static struct dentry *debugfs_create_file(const char *name,unsigned mode,struct dentry *parent,void *data,const struct file_operations *fops)
{
    assert(parent==&root_directory && parent->live);unsigned index=debug_calls++;
    if(debug_calls==debug_fail)return ERR_PTR(-ENOMEM);
    if(debug_calls==debug_null)return NULL;
    assert(index<3);struct dentry *d=&debug_files[index];*d=(struct dentry){true,true,index,data,fops};++debug_live;return d;
}
static struct dentry *debugfs_create_file_unsafe(const char *n,unsigned mode,struct dentry *p,void *data,const struct file_operations *fops)
{
    struct dentry *d=debugfs_create_file(n,mode,p,data,fops);if(!IS_ERR_OR_NULL(d))d->protected=false;return d;
}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned active_callbacks;
static bool block_read,read_entered,permit_read,removal_entered;
static void debugfs_remove(struct dentry *d)
{
    if(IS_ERR_OR_NULL(d))return;
    pthread_mutex_lock(&gate);removal_entered=true;pthread_cond_broadcast(&changed);
    if(d->protected)while(active_callbacks)pthread_cond_wait(&changed,&gate);
    assert(d->live);d->live=false;--debug_live;pthread_mutex_unlock(&gate);
}
struct regmap_access_table { bool deny; };
struct regmap;
struct regcache_ops { int (*read)(struct regmap*,unsigned,unsigned*); };
struct regmap_debugfs_off_cache { struct list_head list;loff_t min,max;unsigned base_reg,max_reg; };
struct regmap {
    struct device *dev;
    unsigned max_register;bool max_register_is_set;int reg_stride;
    struct { unsigned val_bytes;void *format_write; } format;
    struct dentry *debugfs;
    unsigned debugfs_reg_len,debugfs_val_len,debugfs_tot_len;
    struct list_head debugfs_off_cache;
    struct mutex cache_lock;
    int cache_type;
    const struct regcache_ops *cache_ops;
    void (*lock)(void*),(*unlock)(void*);void *lock_arg;
    int (*reg_read)(void*,unsigned,unsigned*);
    bool (*readable_reg)(struct device*,unsigned),(*volatile_reg)(struct device*,unsigned),(*precious_reg)(struct device*,unsigned);
    const struct regmap_access_table *rd_table,*volatile_table,*precious_table;
};
#define REGCACHE_NONE 0
static bool regmap_check_range_table(struct regmap *map,unsigned reg,const struct regmap_access_table *table) { return !table->deny; }
static unsigned register_reads,register_writes;
static int read_error;
static int regmap_read(struct regmap *map,unsigned reg,unsigned *val)
{
    assert(reg<=map->max_register && !(reg%map->reg_stride));++register_reads;
    pthread_mutex_lock(&gate);
    if(block_read){read_entered=true;pthread_cond_broadcast(&changed);while(!permit_read)pthread_cond_wait(&changed,&gate);}
    pthread_mutex_unlock(&gate);
    if(read_error)return read_error;*val=reg+1;return 0;
}
static int regmap_write(struct regmap *map,unsigned reg,unsigned value) { assert(reg<=map->max_register);++register_writes;return 0; }
typedef void (*dr_release_t)(struct device*,void*);
typedef int (*dr_match_t)(struct device*,void*,void*);
struct resource { dr_release_t release;bool added;void *data; };
static struct resource resources[8];
static unsigned resource_count;
static void *devres_alloc(dr_release_t release,size_t size,unsigned flags)
{
    assert(resource_count<ARRAY_SIZE(resources));void *p=kzalloc(size,flags);if(!p)return NULL;
    resources[resource_count++]=(struct resource){release,false,p};return p;
}
static void devres_free(void *p)
{
    for(unsigned i=0;i<resource_count;i++)if(resources[i].data==p){assert(!resources[i].added);kfree(p);resources[i]=resources[--resource_count];return;}
    assert(false);
}
static void devres_add(struct device *dev,void *p)
{
    for(unsigned i=0;i<resource_count;i++)if(resources[i].data==p){assert(!resources[i].added);resources[i].added=true;return;}
    assert(false);
}
static int devres_release(struct device *dev,dr_release_t release,dr_match_t match,void *key)
{
    for(unsigned i=0;i<resource_count;i++)if(resources[i].added&&resources[i].release==release&&match(dev,resources[i].data,key)){
        void *p=resources[i].data;resources[i]=resources[--resource_count];release(dev,p);kfree(p);return 0;
    }
    return -ENOENT;
}
static void release_all(struct device *dev)
{
    while(resource_count){struct resource r=resources[--resource_count];assert(r.added);r.release(dev,r.data);kfree(r.data);}
}
int regmap_qti_debugfs_register(struct device*,struct regmap*);
void regmap_qti_debugfs_unregister(struct regmap*);
int devm_regmap_qti_debugfs_register(struct device*,struct regmap*);
void devm_regmap_qti_debugfs_unregister(struct regmap*);
/* DRIVER */
static struct device dev={.refs=1};
static struct regmap map;
static void no_lock(void *p) {}
static int dummy_read(void *p,unsigned reg,unsigned *value) { return 0; }
static bool sparse_readable(struct device *d,unsigned reg) { return reg%3!=1; }
static bool precious(struct device *d,unsigned reg) { return reg==5; }
static int cache_read(struct regmap *m,unsigned reg,unsigned *value) { *value=7;return 0; }
static struct regcache_ops cache_ops={cache_read};
static void reset(void)
{
    assert(!allocations&&!resource_count&&!debug_live&&list_empty(&regmap_qti_debugfs_list)&&dev.refs==1);
    alloc_calls=fail_alloc=debug_calls=debug_fail=debug_null=register_reads=register_writes=0;
    read_error=0;copy_error=block_read=read_entered=permit_read=removal_entered=false;
    map=(struct regmap){.dev=&dev,.max_register=255,.max_register_is_set=true,.reg_stride=1,.format={.val_bytes=1},.debugfs=&root_directory,.lock=no_lock,.unlock=no_lock,.reg_read=dummy_read};
    INIT_LIST_HEAD(&map.debugfs_off_cache);mutex_init(&map.cache_lock);
}
static void cleanup_cache(void)
{
    regmap_debugfs_free_dump_cache(&map);mutex_destroy(&map.cache_lock);
}
static atomic_bool removal_done;
static struct file open_file;
static ssize_t protected_read(struct dentry *d,char *buf,size_t size,loff_t *pos)
{
    pthread_mutex_lock(&gate);
    assert(d->protected&&d->fops->owner==THIS_MODULE);
    if(!d->live){pthread_mutex_unlock(&gate);return -EIO;}
    ++active_callbacks;pthread_mutex_unlock(&gate);
    ssize_t rc=d->fops->read(&open_file,buf,size,pos);
    pthread_mutex_lock(&gate);--active_callbacks;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);return rc;
}
static void *reader_thread(void *unused) { char buf[128];loff_t pos=0;assert(protected_read(&debug_files[2],buf,sizeof(buf),&pos)>0);return NULL; }
static void *remove_thread(void *unused) { regmap_qti_debugfs_unregister(&map);atomic_store(&removal_done,true);return NULL; }
int main(void)
{
    unsigned cases=0;reset();
    assert(regmap_qti_debugfs_register(NULL,&map)==-EINVAL);
    assert(regmap_qti_debugfs_register(&dev,ERR_PTR(-EIO))==-EINVAL);
    map.debugfs=NULL;assert(regmap_qti_debugfs_register(&dev,&map)==-EOPNOTSUPP);map.debugfs=&root_directory;
    map.reg_stride=0;assert(regmap_qti_debugfs_register(&dev,&map)==-EINVAL);map.reg_stride=1;
    map.max_register=UINT_MAX;assert(regmap_qti_debugfs_register(&dev,&map)==-EINVAL);cleanup_cache();cases+=5;
    for(unsigned mode=0;mode<2;mode++)for(unsigned fault=0;fault<8;fault++) {
        reset();
        if(fault==0)fail_alloc=1;
        else if(fault==1 && mode)fail_alloc=2;
        else if(fault<5)debug_fail=fault-1;
        else debug_null=fault-4;
        int rc=mode?devm_regmap_qti_debugfs_register(&dev,&map):regmap_qti_debugfs_register(&dev,&map);
        if(rc==0){if(mode)release_all(&dev);else regmap_qti_debugfs_unregister(&map);}
        assert(!allocations&&!resource_count&&!debug_live&&list_empty(&regmap_qti_debugfs_list));cleanup_cache();++cases;
    }
    /* Explicit managed unregister must invoke the registered release callback now. */
    reset();assert(!devm_regmap_qti_debugfs_register(&dev,&map));
    assert(devm_regmap_qti_debugfs_register(&dev,&map)==-EINVAL);
    assert(debug_live==3 && allocations==2 && resource_count==1);
    devm_regmap_qti_debugfs_unregister(&map);assert(!debug_live&&!allocations&&!resource_count);
    devm_regmap_qti_debugfs_unregister(&map);release_all(&dev);cleanup_cache();++cases;
    reset();assert(!devm_regmap_qti_debugfs_register(&dev,&map));release_all(&dev);
    assert(!debug_live&&!allocations);cleanup_cache();++cases;
    /* Selector limits and register-read extents, including overflow inputs. */
    for(unsigned stride=1;stride<=4;stride++) {
        reset();map.reg_stride=stride;assert(!regmap_qti_debugfs_register(&dev,&map));
        struct regmap_qti_debugfs *d=debug_files[2].data;
        for(unsigned address=0;address<258;address++) for(unsigned count=0;count<260;count++) {
            int rc=address_set(d,address);assert((rc==0)==(address<=255 && !(address%stride)));
            if(rc){++cases;continue;}
            rc=count_set(d,count);assert((rc==0)==(count<=255/stride+1));
            if(rc){++cases;continue;}
            struct file f={d};char buf[4096];loff_t pos=0;
            unsigned before=register_reads;rc=regmap_data_read_file(&f,buf,sizeof(buf),&pos);
            unsigned entries=count?count:1;
            if(entries-1>(255-address)/stride)assert(rc==-EINVAL&&pos==0&&register_reads==before);
            else {assert(rc>0&&pos==rc&&register_reads-before==entries);}
            ++cases;
        }
        assert(address_set(d,UINT64_MAX)==-EINVAL && count_set(d,UINT64_MAX)==-EINVAL);
        regmap_qti_debugfs_unregister(&map);cleanup_cache();++cases;
    }
    /* Hidden/precious registers and read errors retain the native formatting. */
    reset();map.readable_reg=sparse_readable;map.precious_reg=precious;
    assert(!regmap_qti_debugfs_register(&dev,&map));struct regmap_qti_debugfs *d=debug_files[2].data;
    assert(!address_set(d,0)&&!count_set(d,8));struct file f={d};char buf[128]={0};loff_t pos=0;
    assert(regmap_data_read_file(&f,buf,sizeof(buf)-1,&pos)>0);
    assert(strstr(buf,"00: 01")&&!strstr(buf,"01:")&&!strstr(buf,"05:"));
    assert(!address_set(d,1));pos=0;assert(regmap_data_read_file(&f,buf,sizeof(buf),&pos)==-EINVAL&&pos==0);
    assert(!address_set(d,0)&&!count_set(d,1));pos=0;read_error=-EIO;memset(buf,0,sizeof(buf));
    assert(regmap_data_read_file(&f,buf,sizeof(buf),&pos)==7&&!strcmp(buf,"00: XX\n"));read_error=0;
    pos=0;copy_error=true;assert(regmap_data_read_file(&f,buf,sizeof(buf),&pos)==-EFAULT&&pos==0);copy_error=false;
    pos=INT64_MAX;assert(regmap_data_read_file(&f,buf,sizeof(buf),&pos)==0);
    assert(!address_set(d,2));pos=INT64_MAX;assert(regmap_data_read_file(&f,buf,sizeof(buf),&pos)==-EINVAL&&pos==INT64_MAX);
    regmap_qti_debugfs_unregister(&map);cleanup_cache();cases+=7;
    reset();map.max_register=0;assert(!_regmap_readable(&map,1));
    map.cache_type=1;map.cache_ops=&cache_ops;assert(_regmap_cached(&map,0));assert(!_regmap_cached(&map,1));cleanup_cache();cases+=3;
    /* Removal drains a real native read callback; stale files are denied by the proxy. */
    reset();assert(!regmap_qti_debugfs_register(&dev,&map));
    struct inode inode={debug_files[2].data};assert(!simple_open(&inode,&open_file));
    block_read=true;atomic_store(&removal_done,false);pthread_t reader,remover;
    assert(!pthread_create(&reader,NULL,reader_thread,NULL));
    pthread_mutex_lock(&gate);while(!read_entered)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);
    assert(!pthread_create(&remover,NULL,remove_thread,NULL));
    pthread_mutex_lock(&gate);while(!removal_entered)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);
    usleep(20000);assert(!atomic_load(&removal_done));
    pthread_mutex_lock(&gate);permit_read=true;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
    assert(!pthread_join(reader,NULL)&&!pthread_join(remover,NULL)&&atomic_load(&removal_done));
    pos=0;assert(protected_read(&debug_files[2],buf,sizeof(buf),&pos)==-EIO);
    assert(!register_writes&&!debug_live);cleanup_cache();++cases;
    assert(!allocations&&!resource_count);printf("%u\n",cases);
}
