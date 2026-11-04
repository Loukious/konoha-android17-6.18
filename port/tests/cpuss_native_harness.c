#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef uint64_t phys_addr_t;
typedef uint64_t resource_size_t;
typedef unsigned int umode_t;
#define __iomem
#define __user
#define U8_MAX UINT8_MAX
#define U64_MAX UINT64_MAX
#define BIT(n) (1U << (n))
#define IORESOURCE_MEM 0
#define GFP_KERNEL 0
#define THIS_MODULE NULL
#define MODULE_DEVICE_TABLE(...)
#define module_platform_driver(...)
#define MODULE_DESCRIPTION(...)
#define MODULE_LICENSE(...)
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define MPIDR_AFFINITY_LEVEL(id, n) (((id) >> ((n) * 8)) & 255)
#define for_each_possible_cpu(cpu) for ((cpu)=0; (cpu)<possible; (cpu)++)
#define pr_err(...) ((void)0)
struct list_head { struct list_head *next,*prev; };
#define INIT_LIST_HEAD(n) ((n)->next=(n)->prev=(n))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define list_for_each_entry(p,h,m) for ((p)=container_of((h)->next,__typeof__(*(p)),m); &(p)->m!=(h); (p)=container_of((p)->m.next,__typeof__(*(p)),m))
static void list_add_tail(struct list_head *n,struct list_head *h){n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
struct mutex { int held; };
static int mutex_held;
static void mutex_init(struct mutex *m){m->held=0;}
static void mutex_lock(struct mutex *m){assert(!m->held);m->held=1;mutex_held++;}
static void mutex_unlock(struct mutex *m){assert(m->held);m->held=0;mutex_held--;}
struct resource { resource_size_t start,end; };
static resource_size_t resource_size(const struct resource *r){return r->end-r->start+1;}
struct device { void *of_node; };
struct platform_device { struct device dev;void *data;struct resource resources[10]; };
struct of_device_id { const char *compatible;const void *data; };
struct platform_driver { int (*probe)(struct platform_device *);void (*remove)(struct platform_device *);struct { const char *name;const struct of_device_id *of_match_table; } driver; };
struct seq_file { void *private;char out[4096];size_t pos; };
struct inode { void *i_private; };
struct file { struct inode *f_inode; };
struct file_operations { void *owner;int (*show)(struct seq_file *,void *);ssize_t (*write)(struct file *,const char *,size_t,loff_t *); };
#define DEFINE_SHOW_ATTRIBUTE(name) static const struct file_operations name##_fops={.show=name##_show}
struct dentry { int id;char name[25];void *data;const struct file_operations *fops;struct dentry *parent; };
static struct dentry entries[100];
static void *allocs[100];static int nr_allocs,alloc_calls,alloc_fail;
static int file_calls,file_fail,file_null,files_live,removes,map_fail,maps,possible=8,variant,ncpu=8,property_fail,match_fail,missing_resource=-1,missing_cpu=-1,copy_fault;
static u64 mpidrs[10];
static unsigned checks;
static void (*cleanup)(void *);static void *cleanup_data;
static unsigned char mmio[0x10000] __attribute__((aligned(8)));
static size_t mmio_size;
static u32 cfg_mmio[9];
static int config_mmio,mmio_reads;
static int io_calls,io_fail,io_writes;
static phys_addr_t io_addresses[200];static u32 io_values[200];static int io_stored;
#define CHECK(x) do { assert(x);checks++; } while(0)
static void *devm_kzalloc(struct device *d,size_t n,int g){(void)d;(void)g;alloc_calls++;if(alloc_calls==alloc_fail)return NULL;assert(nr_allocs<100);void *p=calloc(1,n);assert(p);allocs[nr_allocs++]=p;return p;}
static void *devm_ioremap(struct device *d,phys_addr_t base,size_t n){(void)d;maps++;if(maps==map_fail)return NULL;if(base==0x100000){assert(n<=sizeof(mmio));mmio_size=n;return mmio;}assert(config_mmio&&n==sizeof(u32));for(int cpu=0;cpu<8;cpu++)if(base==0x200000+0x10000*cpu)return &cfg_mmio[cpu];assert(base==0x300000);return &cfg_mmio[8];}
static const void *device_get_match_data(struct device *d);
static int of_property_read_u32(void *dn,const char *name,u32 *v){(void)dn;assert(!strcmp(name,"num-cpus"));if(property_fail)return -ENOENT;*v=ncpu;return 0;}
static bool of_property_read_bool(void *dn,const char *name){(void)dn;assert(!strcmp(name,"qcom,config-mmio"));return config_mmio;}
static u64 cpu_logical_map(unsigned cpu){assert(cpu<(unsigned)possible&&cpu<10);return mpidrs[cpu];}
static int dev_err_probe(struct device *d,int ret,const char *fmt,...){(void)d;(void)fmt;return ret;}
static struct platform_device *current;
static struct resource *platform_get_resource_byname(struct platform_device *p,int type,const char *name){assert(p==current&&type==IORESOURCE_MEM);int index=-1;if(!strcmp(name,"apss_seq_mem_base"))index=8;else if(!strcmp(name,"l3_seq_lpm_cntr_cfg"))index=9;else {int cpu=-1;assert(sscanf(name,"seq_lpm_cntr_cfg_cpu%d",&cpu)==1);if(cpu<0||cpu>=8)return NULL;if(cpu==missing_cpu)return NULL;index=cpu;}if(index==missing_resource)return NULL;return &p->resources[index];}
static struct dentry *create(const char *name,struct dentry *parent){file_calls++;if(file_calls==file_fail)return file_null?NULL:ERR_PTR(-ENOSPC);assert(files_live<100);struct dentry *d=&entries[files_live++];d->id=file_calls;snprintf(d->name,sizeof(d->name),"%s",name);d->parent=parent;return d;}
static struct dentry *debugfs_create_dir(const char *name,struct dentry *parent){if(!parent)assert(!strcmp(name,"qcom_cpuss_sleep_stats"));return create(name,parent);}
static struct dentry *debugfs_create_file(const char *name,umode_t mode,struct dentry *parent,void *data,const struct file_operations *fops){assert(parent&&!IS_ERR(parent));assert(mode==0444||mode==0220);assert(data&&fops);struct dentry *d=create(name,parent);if(!IS_ERR_OR_NULL(d)){d->data=data;d->fops=fops;}return d;}
static void debugfs_remove_recursive(struct dentry *d){if(d){assert(d==entries);files_live=0;removes++;}}
static int devm_add_action_or_reset(struct device *d,void (*fn)(void *),void *arg);
static int action_fail;
static int devm_add_action_or_reset(struct device *d,void (*fn)(void *),void *arg){(void)d;if(action_fail){fn(arg);return -ENOMEM;}cleanup=fn;cleanup_data=arg;return 0;}
static void platform_set_drvdata(struct platform_device *p,void *d){p->data=d;}
static void *platform_get_drvdata(struct platform_device *p){return p->data;}
static size_t copy_from_user(void *to,const void *from,size_t n){if(copy_fault)return n;memcpy(to,from,n);return 0;}
static ssize_t strscpy(char *to,const char *from,size_t n){snprintf(to,n,"%s",from);return strlen(to);}
static int seq_printf(struct seq_file *s,const char *fmt,...){va_list a;va_start(a,fmt);int n=vsnprintf(s->out+s->pos,sizeof(s->out)-s->pos,fmt,a);va_end(a);assert(n>=0&&(size_t)n<sizeof(s->out)-s->pos);s->pos+=n;return n;}
static u64 readq_relaxed(const void *p){assert((const char *)p>=(const char *)mmio&&(const char *)p+8<=(const char *)mmio+mmio_size);assert(!((uintptr_t)p&7));u64 val;memcpy(&val,p,8);return val;}
static u32 readl_relaxed(const void *p){assert(config_mmio&&!((uintptr_t)p&3));mmio_reads++;uintptr_t a=(uintptr_t)p;if(a>=(uintptr_t)mmio&&a+4<=(uintptr_t)mmio+mmio_size){u32 val;memcpy(&val,p,4);return val;}assert(a>=(uintptr_t)cfg_mmio&&a+4<=(uintptr_t)(cfg_mmio+9));u32 val;memcpy(&val,p,4);return val;}
static int qcom_scm_io_readl(phys_addr_t addr,u32 *val);
static int qcom_scm_io_writel(phys_addr_t addr,u32 val);
/* INSERT_DRIVER */
static const void *device_get_match_data(struct device *d){(void)d;return match_fail?NULL:variant==0?qcom_cpuss_cntr_v1_offsets:variant==1?qcom_cpuss_cntr_v2_offsets:qcom_cpuss_cntr_v3_offsets;}
static u32 *scm_value(phys_addr_t addr){for(int i=0;i<io_stored;i++)if(io_addresses[i]==addr)return &io_values[i];assert(io_stored<200);const u32 *offsets=device_get_match_data(&current->dev);u32 value=0;bool known=false;for(int i=0;i<8;i++)if(addr==current->resources[i].start){value=15;known=true;}if(addr==current->resources[9].start){value=15;known=true;}for(int i=0;i<ncpu;i++)if(addr==0x100000+offsets[APSS_CPU_LPM_RESIDENCY_CNTR_CFG_n]+4*i){value=21;known=true;}if(addr==0x100000+offsets[APSS_CL_LPM_RESIDENCY_CNTR_CFG]){value=5;known=true;}assert(known&&!(addr&3));io_addresses[io_stored]=addr;io_values[io_stored]=value|0x80000000;return &io_values[io_stored++];}
static int qcom_scm_io_readl(phys_addr_t addr,u32 *val){io_calls++;if(io_calls==io_fail)return -EACCES;*val=*scm_value(addr);return 0;}
static int qcom_scm_io_writel(phys_addr_t addr,u32 val){assert(mutex_held==1);io_calls++;if(io_calls==io_fail)return -EACCES;*scm_value(addr)=val;io_writes++;return 0;}
static void release(void){if(cleanup){cleanup(cleanup_data);cleanup=NULL;}CHECK(!files_live);for(int i=nr_allocs-1;i>=0;i--)free(allocs[i]);nr_allocs=0;CHECK(!mutex_held);}
static struct platform_device device;
static void reset(int v){assert(!nr_allocs&&!files_live);memset(&device,0,sizeof(device));current=&device;variant=v;config_mmio=mmio_reads=0;possible=ncpu=v==0?8:v==1?4:6;for(int i=0;i<10;i++)mpidrs[i]=(u64)i<<8;for(int i=0;i<8;i++)device.resources[i]=(struct resource){0x200000+0x10000*i,0x200003+0x10000*i};device.resources[8]=(struct resource){0x100000,0x10ffff};device.resources[9]=(struct resource){0x300000,0x300003};alloc_calls=alloc_fail=file_calls=file_fail=file_null=files_live=removes=map_fail=maps=property_fail=match_fail=copy_fault=action_fail=io_calls=io_fail=io_writes=io_stored=0;missing_resource=missing_cpu=-1;cleanup=NULL;cleanup_data=NULL;memset(mmio,0,sizeof(mmio));for(int i=0;i<9;i++)cfg_mmio[i]=15|0x80000000;for(int i=0;i<ncpu;i++){u32 value=21|0x80000000;memcpy(mmio+0xc004+4*i,&value,4);}u32 value=5|0x80000000;memcpy(mmio+0xc030,&value,4);memset(entries,0,sizeof(entries));}
static void fail_probe(int expected){CHECK(qcom_cpuss_sleep_stats_probe(&device)==expected);CHECK(!io_writes);release();}
static void publication_failures(void){reset(0);CHECK(!qcom_cpuss_sleep_stats_probe(&device));int publications=file_calls,allocations=alloc_calls;CHECK(publications==74&&allocations==63);release();for(int i=1;i<=publications;i++){reset(0);file_fail=i;fail_probe(-ENOSPC);reset(0);file_fail=i;file_null=1;fail_probe(-ENOMEM);}for(int i=1;i<=allocations;i++){reset(0);alloc_fail=i;fail_probe(-ENOMEM);}reset(0);map_fail=1;fail_probe(-ENOMEM);CHECK(!file_calls);reset(0);action_fail=1;fail_probe(-ENOMEM);CHECK(removes==1);}
static void metadata_bounds(void){reset(0);property_fail=1;fail_probe(-ENOENT);reset(0);match_fail=1;fail_probe(-ENODEV);for(int n=0;n<=10;n++)if(n==0||n>8){reset(0);ncpu=n;fail_probe(-EINVAL);}for(int index=8;index<=9;index++){reset(0);missing_resource=index;fail_probe(-ENODEV);}reset(0);possible=9;fail_probe(-EINVAL);reset(0);mpidrs[0]=U64_MAX;fail_probe(-EINVAL);reset(0);mpidrs[1]=mpidrs[0];fail_probe(-EINVAL);for(int i=0;i<10;i++){reset(0);device.resources[i].end=device.resources[i].start+2;fail_probe(-EINVAL);reset(0);device.resources[i].start++;fail_probe(-EINVAL);}for(unsigned size=1;size<0xc118;size+=257){reset(0);device.resources[8].end=0x100000+size-1;fail_probe(-EINVAL);CHECK(!maps&&!file_calls&&!io_calls);}}
static void firmware_failures(void){for(int i=1;i<=18;i++){reset(0);io_fail=i;fail_probe(-EACCES);CHECK(!io_writes);}reset(0);CHECK(!qcom_cpuss_sleep_stats_probe(&device));struct qcom_target_info *info=device.data;struct inode inode={.i_private=info};struct file f={.f_inode=&inode};for(int i=1;i<=54;i++){io_calls=io_writes=io_stored=0;io_fail=i;loff_t off=0;CHECK(qcom_cpuss_stats_reset_write(&f,"1\n",2,&off)==-EACCES);CHECK(!mutex_held);}qcom_cpuss_sleep_stats_remove(&device);release();}
static void values_and_mapping(void){for(int v=0;v<3;v++){reset(v);for(int cpu=0;cpu<possible;cpu++)mpidrs[cpu]=(u64)(possible-1-cpu)<<8;device.resources[8].end=0x10c117;CHECK(!qcom_cpuss_sleep_stats_probe(&device));struct qcom_target_info *info=device.data;CHECK(info->apss_base==mmio&&maps==1);for(int cpu=0;cpu<possible;cpu++)CHECK(info->cpu_pcpu_map[cpu]==possible-1-cpu);struct qcom_cpuss_stats *node;u64 value=0x1234567800000000ULL;int count=0;list_for_each_entry(node,&info->complete_stats.node,node){u64 n=value+count;memcpy(node->reg,&n,8);struct seq_file seq={.private=node->reg};CHECK(!qcom_cpuss_sleep_stats_show(&seq,NULL));CHECK(strtoull(seq.out,NULL,10)==n);count++;}CHECK(count==7*ncpu+6);struct seq_file all={.private=&info->complete_stats.node};CHECK(!qcom_cpuss_all_stats_show(&all,NULL));int lines=0;for(char *p=all.out;*p;p++)lines+=*p=='\n';CHECK(lines==count);struct inode inode={.i_private=info};struct file f={.f_inode=&inode};loff_t off=0;io_calls=io_writes=io_stored=0;CHECK(qcom_cpuss_stats_reset_write(&f,"1\n",2,&off)==2);CHECK(io_writes==4*ncpu+4);for(int i=0;i<io_stored;i++)CHECK(io_values[i]&0x80000000);qcom_cpuss_sleep_stats_remove(&device);CHECK(removes==1);release();CHECK(removes==1);}reset(0);missing_cpu=7;CHECK(!qcom_cpuss_sleep_stats_probe(&device));struct qcom_target_info *info=device.data;CHECK(info->cpu_pcpu_map[7]==U8_MAX&&!info->cpu_dir[7]);release();}
static void parser(void){reset(0);CHECK(!qcom_cpuss_sleep_stats_probe(&device));struct inode inode={.i_private=device.data};struct file f={.f_inode=&inode};loff_t off=0;CHECK(!check_val("1",1));CHECK(!check_val("1\n",2));const char *bad[]={"", "0", "2", "11", "1x", "1\0", "1\n\n"};size_t sizes[]={0,1,1,2,2,2,3};for(int i=0;i<7;i++){int before=io_calls;CHECK(qcom_cpuss_stats_reset_write(&f,bad[i],sizes[i],&off)==-EINVAL);CHECK(io_calls==before);}copy_fault=1;int before=io_calls;CHECK(qcom_cpuss_stats_reset_write(&f,NULL,2,&off)==-EFAULT);CHECK(io_calls==before);release();}
static void mmio_mode(void){
 for(int v=0;v<3;v++){
  reset(v);config_mmio=1;io_fail=1;
  for(int cpu=0;cpu<possible;cpu++)mpidrs[cpu]=(u64)(possible-1-cpu)<<8;
  device.resources[8].end=0x10c117;
  CHECK(!qcom_cpuss_sleep_stats_probe(&device));
  struct qcom_target_info *info=device.data;
  CHECK(info->config_mmio&&maps==ncpu+2&&mmio_reads==2*ncpu+2);
  CHECK(!io_calls&&!io_writes);
  int publications=file_calls,allocations=alloc_calls;
  for(int cpu=0;cpu<ncpu;cpu++)CHECK(info->per_cpu_cfg_base[cpu]==&cfg_mmio[ncpu-1-cpu]);
  struct qcom_cpuss_stats *node;int count=0;
  list_for_each_entry(node,&info->complete_stats.node,node){
   u64 value=0x1234567800000000ULL+count++;memcpy(node->reg,&value,8);
   struct seq_file seq={.private=node->reg};CHECK(!qcom_cpuss_sleep_stats_show(&seq,NULL));CHECK(strtoull(seq.out,NULL,10)==value);
  }
  CHECK(count==7*ncpu+6);
  struct inode inode={.i_private=info};struct file f={.f_inode=&inode};loff_t off=0;
  int reads=mmio_reads;
  CHECK(qcom_cpuss_stats_reset_write(&f,"1\n",2,&off)==-EACCES);
  CHECK(io_calls==1&&!io_writes&&mmio_reads==reads&&!mutex_held);
  qcom_cpuss_sleep_stats_remove(&device);release();CHECK(removes==1);
  for(int i=1;i<=ncpu+2;i++){reset(v);config_mmio=1;map_fail=i;fail_probe(-ENOMEM);CHECK(!file_calls&&!io_calls&&!mmio_reads);}
  for(int i=1;i<=publications;i++){reset(v);config_mmio=1;file_fail=i;fail_probe(-ENOSPC);reset(v);config_mmio=1;file_fail=i;file_null=1;fail_probe(-ENOMEM);}
  for(int i=1;i<=allocations;i++){reset(v);config_mmio=1;alloc_fail=i;fail_probe(-ENOMEM);}
  reset(v);config_mmio=1;action_fail=1;fail_probe(-ENOMEM);CHECK(removes==1);
 }
 reset(0);config_mmio=1;missing_cpu=7;CHECK(!qcom_cpuss_sleep_stats_probe(&device));struct qcom_target_info *info=device.data;
 CHECK(info->cpu_pcpu_map[7]==U8_MAX&&!info->per_cpu_cfg_base[7]&&maps==9&&mmio_reads==16);release();
 reset(0);config_mmio=1;for(int i=0;i<9;i++)cfg_mmio[i]=0;for(int i=0;i<8;i++){u32 value=0x14;memcpy(mmio+0xc004+4*i,&value,4);}u32 value=4;memcpy(mmio+0xc030,&value,4);
 CHECK(!qcom_cpuss_sleep_stats_probe(&device));CHECK(!io_calls&&!io_writes&&file_calls==29);
 info=device.data;int count=0;struct qcom_cpuss_stats *node;list_for_each_entry(node,&info->complete_stats.node,node){CHECK(strstr(node->mode_name,"_residency"));count++;}CHECK(count==17);release();
}
int main(void){publication_failures();metadata_bounds();firmware_failures();values_and_mapping();parser();mmio_mode();printf("%u\n",checks);}
