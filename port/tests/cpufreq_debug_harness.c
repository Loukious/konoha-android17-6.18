#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
typedef uint16_t u16;typedef uint32_t u32;typedef uint32_t __be32;
#define __iomem
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MAX_FREQ_DOMAINS 16
#define PAGE_SIZE 4096
#define GFP_KERNEL 0
#define IORESOURCE_MEM 0x200
#define NOTIFY_OK 0
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define __ATTR(n,m,s,st) {.attr={.name=#n,.mode=m},.show=s}
struct device_node {int refs,id;};struct device {struct device_node*of_node;};
struct platform_device {struct device dev;void*data;};
struct resource {uint64_t start,end;unsigned flags;};
struct of_phandle_args {struct device_node*np;unsigned args[1];};
struct attribute {const char*name;unsigned mode;};
struct kobject;struct kobj_attribute {struct attribute attr;ssize_t(*show)(struct kobject*,struct kobj_attribute*,char*);};
struct notifier_block {int(*notifier_call)(struct notifier_block*,unsigned long,void*);int priority;};
struct kobj_type {const void*sysfs_ops;void(*release)(struct kobject*);};
struct kobject {const struct kobj_type*type;int added,refs;};
static int kobj_sysfs_ops,panic_notifier_list;static struct kobject parent,*kernel_kobj=&parent;
static struct device_node parent_node={.refs=1,.id=-1},domain_nodes[16];
static const void*match_data;static unsigned mmio[16][1024];
static int length,missing,alloc_fail,parse_fail,resource_fail,map_fail,kobj_fail,notifier_fail,sysfs_fail;
static int alloc_live,notifier_live,sysfs_live,mapped;static unsigned read_count;
static uint64_t region_size;static bool invalid_type,reverse_range;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int block_reads,entered,proceed,remove_entered;static atomic_int panic_active,sysfs_active,removed;
static struct notifier_block*registered_nb;
static void*platform_get_drvdata(struct platform_device*p){return p->data;}
static void platform_set_drvdata(struct platform_device*p,void*v){p->data=v;}
static void*kzalloc(size_t n,int flags){if(alloc_fail)return NULL;void*p=calloc(1,n);assert(p);alloc_live++;return p;}
static void kfree(void*p){assert(alloc_live);alloc_live--;free(p);}
static const void*of_device_get_match_data(struct device*d){return match_data;}
static const void*of_get_property(struct device_node*n,const char*name,int*l){static __be32 cells[34];assert(n==&parent_node);*l=length;return missing?NULL:cells;}
static int of_parse_phandle_with_fixed_args(struct device_node*n,const char*name,int cells,int i,struct of_phandle_args*a){assert(n==&parent_node&&n->refs==1&&cells==1&&i<16);if(parse_fail==i+1)return -ENOENT;a->np=&domain_nodes[i];a->np->refs++;a->args[0]=i;return 0;}
static void of_node_put(struct device_node*n){assert(n!=&parent_node&&n->refs==1);n->refs--;}
static int of_address_to_resource(struct device_node*n,unsigned i,struct resource*r){assert(n==&domain_nodes[i]&&n->refs==1);if(resource_fail==i+1)return -EINVAL;*r=(struct resource){.start=i*4096,.end=i*4096+region_size-1,.flags=invalid_type?0:IORESOURCE_MEM};if(reverse_range)r->end=r->start-1;return 0;}
static unsigned resource_type(struct resource*r){return r->flags;}
static uint64_t resource_size(struct resource*r){return r->end-r->start+1;}
static void*devm_ioremap(struct device*d,uint64_t start,uint64_t size){unsigned i=start/4096;assert(i<16&&size<=4096);if(map_fail==i+1)return NULL;mapped++;return mmio[i];}
static u32 readl_relaxed(void*p){uintptr_t addr=(uintptr_t)p,start=(uintptr_t)mmio;assert(addr>=start&&addr+4<=start+sizeof(mmio)&&!(addr%4));read_count++;if(block_reads){pthread_mutex_lock(&gate);entered=1;pthread_cond_broadcast(&cond);while(!proceed)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);}return *(u32*)p;}
static int sysfs_emit_at(char*b,int offset,const char*fmt,...){assert(offset>=0&&offset<PAGE_SIZE);va_list ap;va_start(ap,fmt);int n=vsnprintf(b+offset,PAGE_SIZE-offset,fmt,ap);va_end(ap);return n<PAGE_SIZE-offset?n:PAGE_SIZE-offset-1;}
static void log_ignore(const char*fmt,...){va_list ap;va_start(ap,fmt);va_end(ap);}
#define pr_err(...) log_ignore(__VA_ARGS__)
static int kobject_init_and_add(struct kobject*k,const struct kobj_type*t,struct kobject*p,const char*name){assert(p==kernel_kobj);k->type=t;k->refs=1;if(kobj_fail)return -EEXIST;k->added=1;return 0;}
static void kobject_del(struct kobject*k){assert(k->added);k->added=0;}
static void kobject_put(struct kobject*k){assert(k->refs>0);if(!--k->refs){assert(!k->added&&!notifier_live&&!sysfs_live&&!atomic_load(&panic_active)&&!atomic_load(&sysfs_active));k->type->release(k);}}
static int atomic_notifier_chain_register(int*l,struct notifier_block*n){assert(l==&panic_notifier_list&&!notifier_live);if(notifier_fail)return -EIO;registered_nb=n;notifier_live=1;return 0;}
static int atomic_notifier_chain_unregister(int*l,struct notifier_block*n){assert(notifier_live&&n==registered_nb);pthread_mutex_lock(&gate);while(atomic_load(&panic_active))pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);notifier_live=0;return 0;}
static int sysfs_create_file(struct kobject*k,struct attribute*a){assert(k->added&&notifier_live&&a->mode==0444);if(sysfs_fail)return -ENOMEM;sysfs_live=1;return 0;}
static void sysfs_remove_file(struct kobject*k,struct attribute*a){assert(k->added&&sysfs_live&&!notifier_live);pthread_mutex_lock(&gate);while(atomic_load(&sysfs_active))pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);sysfs_live=0;}
/* DRIVER */
static void reset(void){assert(!alloc_live&&!notifier_live&&!sysfs_live);for(int i=0;i<16;i++){assert(!domain_nodes[i].refs);domain_nodes[i].id=i;for(int j=0;j<1024;j++)mmio[i][j]=i*1024+j;}assert(parent_node.refs==1);match_data=&epss_config;length=32;missing=alloc_fail=parse_fail=resource_fail=map_fail=kobj_fail=notifier_fail=sysfs_fail=mapped=read_count=0;region_size=4096;invalid_type=reverse_range=false;block_reads=entered=proceed=remove_entered=0;atomic_store(&removed,0);}
static struct platform_device dev(void){return (struct platform_device){.dev={.of_node=&parent_node}};}
static void*run_reader(void*p){struct cpufreq_hwregs*r=((struct platform_device*)p)->data;char*b=aligned_alloc(PAGE_SIZE,PAGE_SIZE);assert(b);atomic_fetch_add(&sysfs_active,1);cpufreq_hwregs_show(&r->kobj,&cpufreq_hwregs_attr,b);free(b);pthread_mutex_lock(&gate);atomic_fetch_sub(&sysfs_active,1);pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);return NULL;}
static void*run_panic(void*p){atomic_fetch_add(&panic_active,1);registered_nb->notifier_call(registered_nb,0,NULL);pthread_mutex_lock(&gate);atomic_fetch_sub(&panic_active,1);pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);return NULL;}
static void*run_remove(void*p){pthread_mutex_lock(&gate);remove_entered=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);qcom_cpufreq_hw_debug_remove(p);atomic_store(&removed,1);return NULL;}
int main(void){unsigned cases=0;
 for(int len=-1;len<=140;len++){
  reset();length=len;struct platform_device p=dev();int ret=qcom_cpufreq_hw_debug_probe(&p);
  if(len<=0||len%8)assert(ret==-EINVAL&&!p.data);else if(len/8>16)assert(ret==-E2BIG&&!p.data);else{assert(!ret);qcom_cpufreq_hw_debug_remove(&p);assert(!p.data);}cases++;
 }
 for(int fail=0;fail<7;fail++){
  reset();struct platform_device p=dev();switch(fail){case 0:match_data=NULL;break;case 1:missing=1;break;case 2:alloc_fail=1;break;case 3:kobj_fail=1;break;case 4:notifier_fail=1;break;case 5:sysfs_fail=1;break;case 6:invalid_type=true;break;}
  assert(qcom_cpufreq_hw_debug_probe(&p)<0&&!p.data&&!alloc_live&&!notifier_live&&!sysfs_live);cases++;
 }
 for(int kind=0;kind<3;kind++)for(int domain=1;domain<=4;domain++){
  reset();struct platform_device p=dev();if(kind==0)parse_fail=domain;else if(kind==1)resource_fail=domain;else map_fail=domain;
  assert(qcom_cpufreq_hw_debug_probe(&p)<0&&!p.data&&!alloc_live);for(int i=0;i<16;i++)assert(!domain_nodes[i].refs);cases++;
 }
 for(int variant=0;variant<2;variant++)for(unsigned size=1;size<=4096;size++){
  reset();struct platform_device p=dev();match_data=variant?(void*)&epss_config:(void*)&std_config;region_size=size;length=8;
  int ret=qcom_cpufreq_hw_debug_probe(&p);if(size<(variant?0x3c8:0x9c4))assert(ret==-EINVAL);else{assert(!ret);qcom_cpufreq_hw_debug_remove(&p);}cases++;
 }
 for(int variant=0;variant<2;variant++)for(unsigned banks=1;banks<=16;banks++){
  reset();struct platform_device p=dev();match_data=variant?(void*)&epss_config:(void*)&std_config;length=banks*8;assert(!qcom_cpufreq_hw_debug_probe(&p));struct cpufreq_hwregs*r=p.data;
  char*b=aligned_alloc(PAGE_SIZE,2*PAGE_SIZE);assert(b);memset(b,0xaa,2*PAGE_SIZE);ssize_t n=cpufreq_hwregs_show(&r->kobj,&cpufreq_hwregs_attr,b);assert(n>0&&n<PAGE_SIZE&&b[n]==0);for(int j=PAGE_SIZE;j<2*PAGE_SIZE;j++)assert((unsigned char)b[j]==0xaa);assert(strstr(b,"FREQUENCY DOMAIN 0")&&strstr(b,"PERF_STATE_DESIRED"));if(variant)assert(strstr(b,"EPSS_DEBUG_LUT"));else assert(!strstr(b,"EPSS_DEBUG_LUT"));free(b);
  read_count=0;cpufreq_panic_callback(&r->panic_nb,0,NULL);assert(read_count==banks*(variant?6:3));qcom_cpufreq_hw_debug_remove(&p);cases++;
 }
 for(int reader=0;reader<2;reader++){
  reset();struct platform_device p=dev();assert(!qcom_cpufreq_hw_debug_probe(&p));block_reads=1;pthread_t active,remove;assert(!pthread_create(&active,NULL,reader?run_reader:run_panic,&p));pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);assert(!pthread_create(&remove,NULL,run_remove,&p));pthread_mutex_lock(&gate);while(!remove_entered)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);assert(alloc_live&&!atomic_load(&removed));pthread_mutex_lock(&gate);proceed=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);assert(!pthread_join(active,NULL)&&!pthread_join(remove,NULL));assert(!alloc_live&&!notifier_live&&!sysfs_live&&atomic_load(&removed));cases++;
 }
 reset();struct platform_device p=dev();assert(!qcom_cpufreq_hw_debug_probe(&p));struct cpufreq_hwregs*r=p.data;r->kobj.refs++;qcom_cpufreq_hw_debug_remove(&p);assert(alloc_live&&!notifier_live&&!sysfs_live);kobject_put(&r->kobj);assert(!alloc_live);cases++;
 printf("%u\n",cases);
}
