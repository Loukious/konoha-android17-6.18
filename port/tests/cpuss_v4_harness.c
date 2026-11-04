#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef unsigned long long u64;
#define __iomem
#define GFP_KERNEL 0
#define IORESOURCE_MEM 0
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define MODULE_DEVICE_TABLE(...)
#define module_platform_driver(...)
#define MODULE_LICENSE(...)
#define MODULE_DESCRIPTION(...)
struct resource { u64 start,end; };
struct device { int unused; };
struct platform_device { struct device dev;void *data;struct resource r; };
struct of_device_id { const char *compatible; const void *data; };
struct platform_driver { int (*probe)(struct platform_device *);void (*remove)(struct platform_device *);struct {const char *name;const struct of_device_id *of_match_table;}driver; };
struct dentry { int id; };
struct seq_file { void *private;char out[64]; };
struct file_operations { int (*show)(struct seq_file *,void *); };
#define DEFINE_SHOW_ATTRIBUTE(name) static const struct file_operations name##_fops={.show=name##_show}
static int count,alloc_fail,alloc_calls,file_fail,file_calls,match_fail,res_fail,map_fail,action_fail,files_live,removes;
#define CHECK(v) do {assert(v);count++;}while(0)
static unsigned char mmio[0x2000];
static size_t mmio_size;
static int script_n,script_pos;
static u32 script[512];
static void *allocs[16];static int nr_allocs;
static void (*cleanup_fn)(void *);static void *cleanup_data;
static struct dentry entries[7];
static int seq_printf(struct seq_file *s,const char *fmt,...){va_list a;va_start(a,fmt);int n=vsnprintf(s->out,sizeof(s->out),fmt,a);va_end(a);return n;}
static u32 readl(const void *p){assert(p>= (void *)mmio&&(const char *)p+4<=(char *)mmio+mmio_size);assert(!((uintptr_t)p&3));if(script_n){assert(script_pos<script_n);return script[script_pos++];}u32 n;memcpy(&n,p,4);return n;}
#define readl_relaxed readl
static void *devm_kzalloc(struct device *d,size_t n,int g){(void)d;(void)g;alloc_calls++;if(alloc_fail==alloc_calls)return NULL;void *p=calloc(1,n);assert(p&&nr_allocs<16);allocs[nr_allocs++]=p;return p;}
static void *devm_kmemdup(struct device *d,const void *s,size_t n,int g){void *p=devm_kzalloc(d,n,g);if(p)memcpy(p,s,n);return p;}
static const void *of_device_get_match_data(struct device *d);
static struct resource *platform_get_resource(struct platform_device *p,int kind,int n){(void)kind;(void)n;return res_fail?NULL:&p->r;}
static u64 resource_size(struct resource *r){return r->end-r->start+1;}
static void *devm_platform_get_and_ioremap_resource(struct platform_device *p,int n,void *r){(void)n;(void)r;mmio_size=resource_size(&p->r);return map_fail?ERR_PTR(-EBUSY):mmio;}
static struct dentry *debugfs_create_dir(const char *n,void *parent){(void)parent;assert(!strcmp(n,"qcom_cpuss_sleep_stats"));file_calls++;if(file_fail==file_calls)return ERR_PTR(-ENOSPC);files_live++;return &entries[0];}
static struct dentry *debugfs_create_file(const char *n,int mode,struct dentry *root,void *data,const struct file_operations *f){assert(n&&mode==0400&&root==entries&&data&&f->show);file_calls++;if(file_fail==file_calls)return ERR_PTR(-ENOSPC);assert(file_calls<=7);files_live++;return &entries[file_calls-1];}
static void debugfs_remove_recursive(struct dentry *d){if(d){assert(d==entries);files_live=0;removes++;}}
static int devm_add_action_or_reset(struct device *d,void (*fn)(void *),void *data){(void)d;if(action_fail){fn(data);return -ENOMEM;}cleanup_fn=fn;cleanup_data=data;return 0;}
static void platform_set_drvdata(struct platform_device *p,void *d){p->data=d;}
static void *platform_get_drvdata(struct platform_device *p){return p->data;}
/* INSERT_DRIVER */
static const void *of_device_get_match_data(struct device *d){(void)d;return match_fail?NULL:&qcom_cpuss_cntr_offsets;}
static void reset(void){assert(!nr_allocs&&!files_live);alloc_fail=alloc_calls=file_fail=file_calls=match_fail=res_fail=map_fail=action_fail=removes=script_n=script_pos=0;cleanup_fn=NULL;cleanup_data=NULL;memset(mmio,0,sizeof(mmio));}
static void release_devm(void){if(cleanup_fn){cleanup_fn(cleanup_data);cleanup_fn=NULL;}for(int i=0;i<nr_allocs;i++)free(allocs[i]);nr_allocs=0;CHECK(!files_live);}
static void probe_failures(void){struct platform_device p={.r={.start=0,.end=sizeof(mmio)-1}};for(int fail=1;fail<=7;fail++){reset();file_fail=fail;CHECK(qcom_cpuss_sleep_stats_v4_probe(&p)==-ENOSPC);release_devm();}for(int fail=1;fail<=2;fail++){reset();alloc_fail=fail;CHECK(qcom_cpuss_sleep_stats_v4_probe(&p)==-ENOMEM);release_devm();}reset();map_fail=1;CHECK(qcom_cpuss_sleep_stats_v4_probe(&p)==-EBUSY);release_devm();reset();action_fail=1;CHECK(qcom_cpuss_sleep_stats_v4_probe(&p)==-ENOMEM&&removes==1);release_devm();reset();match_fail=1;CHECK(qcom_cpuss_sleep_stats_v4_probe(&p)==-ENODEV);release_devm();reset();res_fail=1;CHECK(qcom_cpuss_sleep_stats_v4_probe(&p)==-ENODEV);release_devm();for(unsigned n=0;n<0x1338;n+=31){reset();p.r.end=n;CHECK(qcom_cpuss_sleep_stats_v4_probe(&p)==-EINVAL&&!alloc_calls);release_devm();}}
static void counters(void){reset();struct platform_device p={.r={.end=sizeof(mmio)-1}};CHECK(!qcom_cpuss_sleep_stats_v4_probe(&p));struct qcom_stats_prvdata *a=p.data;CHECK(a->offset!=&qcom_cpuss_cntr_offsets&&files_live==7);for(unsigned i=0;i<REGS_MAX;i++){u32 low=UINT32_MAX-i,high=17+i;memcpy(mmio+a->offset->count_regs[i].offset,&low,4);memcpy(mmio+a->offset->residency_regs[i].offset,&low,4);memcpy(mmio+a->offset->residency_regs[i].offset+a->offset->residency_regs[i].residency_lo_hi_distance,&high,4);struct seq_file s={.private=&a->offset->count_regs[i]};CHECK(!qcom_stats_count_show(&s,NULL));CHECK(strtoull(s.out,NULL,10)==low);s.private=&a->offset->residency_regs[i];CHECK(!qcom_stats_residency_show(&s,NULL));CHECK(strtoull(s.out,NULL,10)==((u64)high<<32|low));}script_n=6;script_pos=0;u32 rollover[]={0,UINT32_MAX,1,1,7,1};memcpy(script,rollover,sizeof(rollover));struct seq_file s={.private=&a->offset->residency_regs[0]};CHECK(!qcom_stats_residency_show(&s,NULL)&&strtoull(s.out,NULL,10)==((1ULL<<32)|7)&&script_pos==6);script_n=384;script_pos=0;for(int i=0;i<script_n;i++)script[i]=i;s.out[0]=0;CHECK(qcom_stats_residency_show(&s,NULL)==-EAGAIN&&script_pos==384&&!s.out[0]);qcom_cpuss_sleep_stats_remove(&p);CHECK(!files_live&&removes==1);release_devm();CHECK(removes==1);}
static void bounds(void){for(unsigned i=0;i<REGS_MAX;i++){struct stats_config c=qcom_cpuss_cntr_offsets;c.count_regs[i].offset=UINT32_MAX;CHECK(qcom_cpuss_v4_validate_config(&c,sizeof(mmio))==-EINVAL);c=qcom_cpuss_cntr_offsets;c.residency_regs[i].residency_lo_hi_distance=UINT32_MAX;CHECK(qcom_cpuss_v4_validate_config(&c,sizeof(mmio))==-EINVAL);}CHECK(!qcom_cpuss_v4_validate_config(&qcom_cpuss_cntr_offsets,0x1338));CHECK(qcom_cpuss_v4_validate_config(&qcom_cpuss_cntr_offsets,0x1337)==-EINVAL);}
int main(void){probe_failures();counters();bounds();printf("%d\n",count);}
