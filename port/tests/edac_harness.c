#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
typedef uint32_t u32;typedef uint8_t u8;typedef int irqreturn_t;
#define U32_MAX UINT32_MAX
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h)))& (UINT32_MAX<<(l)))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define IS_ERR_OR_NULL(p) (!(p)||(uintptr_t)(p)>(uintptr_t)-4096)
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define IRQF_ONESHOT 1
#define IRQF_TRIGGER_HIGH 2
#define KERN_CRIT ""
#define edac_printk(...) ((void)0)
#define dev_err(...) ((void)0)
#define pr_err(...) ((void)0)
struct mutex {pthread_mutex_t value;};
#define mutex_init(p) assert(!pthread_mutex_init(&(p)->value,NULL))
#define mutex_lock(p) assert(!pthread_mutex_lock(&(p)->value))
#define mutex_unlock(p) assert(!pthread_mutex_unlock(&(p)->value))
struct regmap {u32 values[512];bool broadcast;};
struct llcc_edac_reg_offset;
struct llcc_drv_data {struct regmap**regmaps;struct regmap*bcast_regmap;struct llcc_edac_reg_offset*edac_reg_offset;u32 num_banks;int ecc_irq;};
struct device {void*platform_data;};
struct platform_device {struct device dev;void*data;};
struct edac_device_ctl_info {struct device*dev;char*mod_name,*dev_name,*ctl_name;bool panic_on_ue;unsigned poll_msec;void(*edac_check)(struct edac_device_ctl_info*);void*pvt_info;};
static char*dev_name(struct device*d){return "qcom_llcc_edac";}
static void platform_set_drvdata(struct platform_device*p,void*d){p->data=d;}
static void*platform_get_drvdata(struct platform_device*p){return p->data;}
static struct regmap bank_maps[16],broadcast_map;static struct regmap*map_ptrs[16];
static int alloc_error,add_error,irq_error,alloc_live,registered,irq_registered;
static unsigned read_calls,write_calls,read_fail_at,write_fail_at,ce,ue,event_count;
static int events[32];
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int block_read,entered,resume,remove_entered;
static atomic_int irq_active,removed;
static struct edac_device_ctl_info*irq_ctl;
static irqreturn_t(*irq_fn)(int,void*);
/* DECLARATIONS */
static struct llcc_edac_reg_offset offsets;
static int regmap_read(struct regmap*m,u32 addr,void*out){
 assert(m && addr%4==0&&addr/4<512);read_calls++;
 if(block_read&&!m->broadcast){assert(!pthread_mutex_lock(&gate));entered=1;pthread_cond_broadcast(&cond);while(!resume)pthread_cond_wait(&cond,&gate);assert(!pthread_mutex_unlock(&gate));}
 if(read_calls==read_fail_at)return -EIO;memcpy(out,&m->values[addr/4],4);return 0;
}
static int regmap_write(struct regmap*m,u32 addr,u32 val){
 assert(m && addr%4==0&&addr/4<512);if(++write_calls==write_fail_at)return -EIO;m->values[addr/4]=val;
 if(m->broadcast && (addr==offsets.drp_interrupt_clear||addr==offsets.trp_interrupt_0_clear))for(unsigned i=0;i<16;i++)bank_maps[i].values[(addr==offsets.drp_interrupt_clear?offsets.drp_interrupt_status:offsets.trp_interrupt_0_status)/4]=0;
 return 0;
}
static int regmap_update_bits(struct regmap*m,u32 addr,u32 mask,u32 val){return regmap_write(m,addr,(m->values[addr/4]&~mask)|(val&mask));}
static struct edac_device_ctl_info*edac_device_alloc_ctl_info(unsigned size,char*a,unsigned b,char*c,unsigned banks,unsigned offset,int idx){
 if(alloc_error)return NULL;assert(!alloc_live);struct edac_device_ctl_info*ctl=calloc(1,sizeof(*ctl));assert(ctl);ctl->pvt_info=calloc(1,size);assert(ctl->pvt_info);alloc_live++;return ctl;
}
static int edac_device_alloc_index(void){return 0;}
static int edac_device_add_device(struct edac_device_ctl_info*c){assert(!registered&&c->panic_on_ue);if(c->edac_check)c->edac_check(c);assert(!read_calls&&!write_calls);if(add_error)return add_error;registered=1;return 0;}
static struct edac_device_ctl_info*edac_device_del_device(struct device*d){assert(registered);registered=0;return NULL;}
static void edac_device_free_ctl_info(struct edac_device_ctl_info*c){assert(alloc_live&&!registered&&!irq_registered&&!atomic_load(&irq_active));struct qcom_llcc_edac*st=c->pvt_info;assert(!st->active);assert(!pthread_mutex_destroy(&st->lock.value));free(c->pvt_info);free(c);alloc_live--;}
static int devm_request_threaded_irq(struct device*d,int irq,void*top,irqreturn_t(*fn)(int,void*),int flags,char*n,void*ctl){assert(!top&&flags==(IRQF_ONESHOT|IRQF_TRIGGER_HIGH));if(irq_error)return irq_error;irq_ctl=ctl;irq_fn=fn;irq_registered=1;assert(fn(irq,ctl)==IRQ_NONE);return 0;}
static void devm_free_irq(struct device*d,int irq,void*ctl){assert(irq_registered&&ctl==irq_ctl);assert(!pthread_mutex_lock(&gate));while(atomic_load(&irq_active))pthread_cond_wait(&cond,&gate);assert(!pthread_mutex_unlock(&gate));irq_registered=0;}
static void edac_device_handle_ce(struct edac_device_ctl_info*c,int inst,unsigned bank,char*msg){assert(registered);ce++;events[event_count++]=0;}
static void edac_device_handle_ue(struct edac_device_ctl_info*c,int inst,unsigned bank,char*msg){assert(registered&&c->panic_on_ue);ue++;events[event_count++]=1;}
/* FUNCTIONS */
static void reset(void){
 assert(!alloc_live&&!registered&&!irq_registered);memset(bank_maps,0,sizeof(bank_maps));memset(&broadcast_map,0,sizeof(broadcast_map));broadcast_map.broadcast=true;
 u32*fields=(u32*)&offsets;for(unsigned i=0;i<sizeof(offsets)/4;i++)fields[i]=i*64;
 for(unsigned i=0;i<512;i++)broadcast_map.values[i]=0xa5000000+i;
 for(unsigned i=0;i<16;i++)map_ptrs[i]=&bank_maps[i];
 alloc_error=add_error=irq_error=read_calls=write_calls=read_fail_at=write_fail_at=ce=ue=event_count=0;block_read=entered=resume=remove_entered=0;atomic_store(&removed,0);
}
static struct platform_device device(struct llcc_drv_data*d){return (struct platform_device){.dev={.platform_data=d}};}
static void*run_irq(void*unused){atomic_fetch_add(&irq_active,1);irq_fn(1,irq_ctl);assert(!pthread_mutex_lock(&gate));atomic_fetch_sub(&irq_active,1);pthread_cond_broadcast(&cond);assert(!pthread_mutex_unlock(&gate));return NULL;}
static void*run_remove(void*p){assert(!pthread_mutex_lock(&gate));remove_entered=1;pthread_cond_broadcast(&cond);assert(!pthread_mutex_unlock(&gate));qcom_llcc_edac_remove(p);atomic_store(&removed,1);return NULL;}
int main(void){
 unsigned cases=0;struct llcc_drv_data drv={.regmaps=map_ptrs,.bcast_regmap=&broadcast_map,.edac_reg_offset=&offsets,.num_banks=3,.ecc_irq=1};
 for(unsigned banks=0;banks<=17;banks++)for(int irq=0;irq<2;irq++){
  reset();drv.num_banks=banks;drv.ecc_irq=irq;struct platform_device p=device(&drv);u32 saved[512];memcpy(saved,broadcast_map.values,sizeof(saved));
  int ret=qcom_llcc_edac_probe(&p);if(!banks||banks>16)assert(ret==-EINVAL&&!alloc_live&&!read_calls);else{assert(!ret&&p.data);struct edac_device_ctl_info*c=p.data;assert(((struct qcom_llcc_edac*)c->pvt_info)->active);assert(irq?irq_registered:!!c->edac_check);qcom_llcc_edac_remove(&p);assert(!p.data&&!alloc_live&&!memcmp(saved,broadcast_map.values,sizeof(saved)));}cases++;
 }
 drv.num_banks=3;drv.ecc_irq=1;
 for(unsigned fail=0;fail<11;fail++){
  reset();struct platform_device p=device(&drv);u32 saved[512];memcpy(saved,broadcast_map.values,sizeof(saved));
  if(fail==0)alloc_error=1;else if(fail==1)add_error=-ENOMEM;else if(fail<=5)read_fail_at=fail-1;else if(fail<=9)write_fail_at=fail-5;else irq_error=-EBUSY;
  int ret=qcom_llcc_edac_probe(&p);if(fail==10){assert(!ret&&!irq_registered&&((struct edac_device_ctl_info*)p.data)->edac_check);qcom_llcc_edac_remove(&p);}else assert(ret<0);
  assert(!alloc_live&&!registered&&!irq_registered&&!memcmp(saved,broadcast_map.values,sizeof(saved)));cases++;
 }
 reset();struct platform_device p=device(&drv);assert(!qcom_llcc_edac_probe(&p));
 for(unsigned pattern=0;pattern<4096;pattern++){
  read_calls=write_calls=ce=ue=event_count=0;unsigned expected_ce=0,expected_ue=0;
  for(unsigned i=0;i<3;i++){
   unsigned drp=(pattern>>(4*i))&3,trp=(pattern>>(4*i+2))&3;
   bank_maps[i].values[offsets.drp_interrupt_status/4]=drp;bank_maps[i].values[offsets.trp_interrupt_0_status/4]=trp;
   expected_ue+=!!(drp&2)+!!(trp&2);expected_ce+=!!(drp&1&&!(drp&2))+!!(trp&1&&!(trp&2));
  }
  assert(llcc_ecc_irq_handler(1,p.data)==(pattern?IRQ_HANDLED:IRQ_NONE));assert(ce==expected_ce&&ue==expected_ue);
  for(unsigned i=0;i<expected_ue;i++)assert(events[i]==1);cases++;
 }
 for(unsigned fail=1;fail<=14;fail++){
  memset(bank_maps,0,sizeof(bank_maps));bank_maps[0].values[offsets.drp_interrupt_status/4]=2;read_calls=write_calls=ce=ue=event_count=0;read_fail_at=fail;
  llcc_ecc_irq_handler(1,p.data);assert(ue==(fail!=1));cases++;
 }
 read_fail_at=0;read_calls=write_calls=0;write_fail_at=1;bank_maps[0].values[offsets.drp_interrupt_status/4]=2;ce=ue=event_count=0;llcc_ecc_irq_handler(1,p.data);assert(ue==1);write_fail_at=0;cases++;
 memset(bank_maps,0,sizeof(bank_maps));read_calls=write_calls=0;block_read=1;pthread_t irq,remove;assert(!pthread_create(&irq,NULL,run_irq,NULL));assert(!pthread_mutex_lock(&gate));while(!entered)pthread_cond_wait(&cond,&gate);assert(!pthread_mutex_unlock(&gate));assert(!pthread_create(&remove,NULL,run_remove,&p));assert(!pthread_mutex_lock(&gate));while(!remove_entered)pthread_cond_wait(&cond,&gate);assert(!pthread_mutex_unlock(&gate));assert(!atomic_load(&removed)&&alloc_live);
 assert(!pthread_mutex_lock(&gate));resume=1;pthread_cond_broadcast(&cond);assert(!pthread_mutex_unlock(&gate));assert(!pthread_join(irq,NULL));assert(!pthread_join(remove,NULL));assert(atomic_load(&removed)&&!alloc_live&&!registered&&!irq_registered);cases++;
 printf("%u\n",cases);
}
