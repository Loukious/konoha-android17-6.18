#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
typedef uint32_t u32;
typedef unsigned long long u64;
#define __packed __attribute__((packed))
#define __iomem
#define __user
#define U32_MAX UINT32_MAX
#define GFP_KERNEL 0
#define PAGE_SIZE 4096
#define CONFIG_QTI_SCMI_VENDOR_PROTOCOL 1
#define IS_ENABLED(c) (c)
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,x) ((v)=(x))
#define cpu_to_le32(v) (v)
#define le32_to_cpu(v) (v)
#define cpu_to_le64(v) (v)
#define le64_to_cpu(v) (v)
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h)))& (UINT32_MAX<<(l)))
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define check_add_overflow(a,b,p) __builtin_add_overflow((a),(b),(p))
#define check_mul_overflow(a,b,p) __builtin_mul_overflow((a),(b),(p))
#define current get_current()
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define dev_err_ratelimited(...) ((void)0)
struct mutex {int held;};
#define mutex_lock(m) (++(m)->held)
#define mutex_unlock(m) (--(m)->held)
typedef int spinlock_t;
#define spin_lock_irqsave(p,f) ((f)=0,(void)(p))
#define spin_unlock_irqrestore(p,f) ((void)(p),(void)(f))
struct list_head {struct list_head *next,*prev;};
#define INIT_LIST_HEAD(p) ((p)->next=(p),(p)->prev=(p))
static bool list_empty(struct list_head *p) {return p->next==p;}
static void list_add_tail(struct list_head *n,struct list_head *h) {n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
static void list_del(struct list_head *n) {n->prev->next=n->next;n->next->prev=n->prev;}
#define list_first_entry(h,t,m) container_of((h)->next,t,m)
struct work_struct {int unused;};
struct delayed_work {struct work_struct work;};
struct workqueue_struct {int unused;};
struct dentry {int unused;};
struct device {void *data;};
struct mbox_chan {int unused;};
struct mbox_client {struct device *dev;};
struct scmi_device {int unused;};
struct scmi_protocol_handle {int unused;};
struct qcom_scmi_vendor_ops {
 int (*set_param)(struct scmi_protocol_handle *,void *,u64,u32,size_t);
 int (*get_param)(struct scmi_protocol_handle *,void *,u64,u32,size_t,size_t);
};
struct file {void *private_data;};
struct resource {u64 start,end;};
struct platform_device {struct device dev;struct resource resources[4];int count;};
#define IORESOURCE_MEM 0
/* INSERT_DECLARATIONS */
static int cases, heap_count, allocations_count, allocation_call, allocation_failure;
static int fw_result, fw_calls, mbox_result, mbox_calls, txdone_calls, queue_calls, traces, frees, cancels, destroys;
static bool infinite_modules;
static char last_trace[1025];
static struct scmi_module_log_level_t last_configuration;
static void *allocations[64];
static unsigned char remote[2][4096];
static struct workqueue_struct queue;
static int scnprintf(char *b,size_t size,const char *fmt,...) {va_list ap;va_start(ap,fmt);int n=vsnprintf(b,size,fmt,ap);va_end(ap);return n<(int)size?n:(size?(int)size-1:0);}
static bool fail_alloc(void) {return ++allocation_call==allocation_failure;}
static void *kzalloc(size_t size,int flags) {if(fail_alloc())return NULL;void *p=calloc(1,size);assert(p);heap_count++;return p;}
static void kfree(void *p) {if(p){assert(heap_count>0);heap_count--;free(p);}}
static void *devm_kzalloc(struct device *d,size_t size,int flags) {if(fail_alloc())return NULL;void *p=calloc(1,size);assert(p && allocations_count<64);allocations[allocations_count++]=p;return p;}
static void *devm_kcalloc(struct device *d,size_t n,size_t size,int flags) {return devm_kzalloc(d,n*size,flags);}
static void *devm_ioremap(struct device *d,u64 start,u32 size) {assert(size<=sizeof(remote[0]));return start==0x81200000?remote[0]:remote[1];}
static struct resource *platform_get_resource(struct platform_device *p,int type,unsigned int index) {return index<(unsigned int)p->count ? &p->resources[index] : NULL;}
static u64 resource_size(struct resource *r) {return r->end-r->start+1;}
static void *dev_get_drvdata(struct device *dev) {return dev->data;}
static u32 readl(void *p) {u32 value;memcpy(&value,p,4);return value;}
static void memcpy_fromio(void *dst,void *src,size_t size) {assert((unsigned char *)src>=remote[0] && (unsigned char *)src+size<=remote[1]+4096);memcpy(dst,src,size);}
static void queue_delayed_work(struct workqueue_struct *q,struct delayed_work *w,int delay) {assert(q==&queue && !delay);queue_calls++;}
static void trace_cpucp_log(char *text) {size_t size=strlen(text);assert(size<=1024);memcpy(last_trace,text,size+1);traces++;}
#define trace_pdp0_log trace_cpucp_log
#define trace_pdp1_log trace_cpucp_log
static void mbox_free_channel(struct mbox_chan *c) {assert(!IS_ERR_OR_NULL(c));frees++;}
static void cancel_delayed_work_sync(struct delayed_work *w) {cancels++;}
static void destroy_workqueue(struct workqueue_struct *q) {assert(q==&queue);destroys++;}
static int mbox_send_message(struct mbox_chan *c,void *value) {assert(c);mbox_calls++;return mbox_result;}
static void mbox_client_txdone(struct mbox_chan *c,int ret) {assert(c && !ret);txdone_calls++;}
static int scmi_set(struct scmi_protocol_handle *ph,void *value,u64 algo,u32 id,size_t size) {assert(algo==CPUCP_CTRL_ALGO_STR && size==8);fw_calls++;if(id==MODULE_LOG_LEVEL)memcpy(&last_configuration,value,size);return fw_result;}
static int scmi_get(struct scmi_protocol_handle *ph,void *value,u64 algo,u32 id,size_t tx,size_t rx) {assert(algo==CPUCP_CTRL_ALGO_STR);fw_calls++;if(fw_result)return fw_result;if(id==LOGBUF_IDX){assert(!tx && rx==8);*(u64 *)value=7;}else{assert(id==MODULE_LOG_LEVEL && tx==4 && rx==sizeof(struct scmi_module_log_data_t));struct scmi_module_log_data_t *v=value;memset(v->module_name,'X',sizeof(v->module_name));v->remaining_modules=infinite_modules?1:0;}return 0;}
static struct qcom_scmi_vendor_ops ops={scmi_set,scmi_get};
static ssize_t simple_read_from_buffer(char *to,size_t count,loff_t *pos,const void *from,size_t available) {if(*pos<0)return -EINVAL;if((size_t)*pos>=available)return 0;size_t length=available-*pos;if(length>count)length=count;memcpy(to,(char *)from+*pos,length);*pos+=length;return length;}
static void *memdup_user_nul(const char *src,size_t size) {char *p=kzalloc(size+1,0);if(!p)return ERR_PTR(-ENOMEM);memcpy(p,src,size);return p;}
static char *strim(char *str) {while(isspace((unsigned char)*str))str++;size_t n=strlen(str);while(n && isspace((unsigned char)str[n-1]))str[--n]=0;return str;}
static int kstrtou32(const char *src,int base,u32 *value) {char *end;errno=0;unsigned long long v=strtoull(src,&end,base);if(errno || src==end || *end || *src=='-' || v>UINT32_MAX)return -EINVAL;*value=v;return 0;}
/* INSERT_FUNCTIONS */
#define CHECK(e) do {assert(e);cases++;}while(0)
static int list_count(struct list_head *h) {int count=0;for(struct list_head *p=h->next;p!=h;p=p->next){assert(count<5);count++;}return count;}
static void reset(void) {while(allocations_count)free(allocations[--allocations_count]);assert(!heap_count);allocation_call=allocation_failure=fw_result=fw_calls=mbox_result=mbox_calls=txdone_calls=queue_calls=traces=frees=cancels=destroys=0;infinite_modules=false;memset(remote,0,sizeof(remote));}
static struct platform_device pdev;
static struct cpucp_log_info info;
static struct mbox_chan rx,tx;
static void setup(int type) {reset();memset(&info,0,sizeof(info));memset(&pdev,0,sizeof(pdev));pdev.count=2;pdev.resources[0]=(struct resource){0x81200000,0x812007ff};pdev.resources[1]=(struct resource){0x81200800,0x81200fff};info.dev=&pdev.dev;info.cl.dev=&pdev.dev;pdev.dev.data=&info;info.type=type;info.wq=&queue;info.ch=&rx;info.tx_ch=&tx;info.ops=&ops;assert(!populate_free_buffers(&pdev,&info));}
int main(void) {
 CHECK(!strcmp(cpucp_log_names[CPUCP],"cpucp"));
 setup(CPUCP);CHECK(list_count(&info.free_buffers_list)==MAX_BUF_NUM);
 for(u32 marker=0;marker<4;marker++){memcpy(remote[0],&marker,4);cpucp_log_rx(&info.cl,NULL);CHECK(list_count(&info.free_buffers_list)==MAX_BUF_NUM && !queue_calls);}
 u32 marker=4097;memcpy(remote[0],&marker,4);cpucp_log_rx(&info.cl,NULL);CHECK(list_count(&info.free_buffers_list)==MAX_BUF_NUM && !queue_calls);
 marker=4;memcpy(remote[0],&marker,4);cpucp_log_rx(&info.cl,NULL);CHECK(list_count(&info.free_buffers_list)==MAX_BUF_NUM && !queue_calls);
 marker=10;memcpy(remote[0],&marker,4);memcpy(remote[0]+4,"hello\n",6);cpucp_log_rx(&info.cl,NULL);CHECK(list_count(&info.full_buffers_list)==1 && queue_calls==1);
 cpucp_log_work(&info.work.work);CHECK(!strcmp(last_trace,"hello") && traces==1 && !info.rem_len && list_count(&info.free_buffers_list)==MAX_BUF_NUM);
 marker=7;memcpy(remote[0],&marker,4);memcpy(remote[0]+4,"abc",3);cpucp_log_rx(&info.cl,NULL);cpucp_log_work(&info.work.work);CHECK(info.rem_len==3 && traces==1);
 marker=8;memcpy(remote[0],&marker,4);memcpy(remote[0]+4,"def\n",4);cpucp_log_rx(&info.cl,NULL);cpucp_log_work(&info.work.work);CHECK(!strcmp(last_trace,"abcdef") && !info.rem_len);
 marker=1028;memcpy(remote[0],&marker,4);memset(remote[0]+4,'A',1024);cpucp_log_rx(&info.cl,NULL);cpucp_log_work(&info.work.work);CHECK(strlen(last_trace)==1024 && !info.rem_len);
 marker=2054;memcpy(remote[0],&marker,4);memcpy(remote[1],"later\n",6);cpucp_log_rx(&info.cl,NULL);cpucp_log_work(&info.work.work);CHECK(!strcmp(last_trace,"later"));
 info.stopping=true;int before=queue_calls;cpucp_log_rx(&info.cl,NULL);CHECK(queue_calls==before);
 setup(PDP0);u64 message=((u64)(((0x81200000>>8)&0xffffff) | (0U<<24)))<<32,original=message;memcpy(remote[0],"pdp\n",4);cpucp_log_rx(&info.cl,&message);CHECK(message==original && list_count(&info.full_buffers_list)==1);
 cpucp_log_work(&info.work.work);CHECK(traces==1 && !strcmp(last_trace,"pdp") && list_count(&info.free_buffers_list)==MAX_BUF_NUM);
 message=0;cpucp_log_rx(&info.cl,&message);cpucp_log_rx(&info.cl,NULL);CHECK(list_count(&info.free_buffers_list)==MAX_BUF_NUM);
 setup(CPUCP);fw_result=-EIO;CHECK(set_log_level(&info,4)==-EIO && flush_cpucp_log(&info,1)==-EIO);u64 value=55;CHECK(get_log_level(&info,&value)==-EIO && value==55);
 fw_result=0;CHECK(!get_log_level(&info,&value) && value==7);
 struct file file={.private_data=&info};loff_t pos=0;CHECK(set_module_log_level(&file,"12 3",4,&pos)==4 && last_configuration.module_id==12 && last_configuration.log_level==3 && !heap_count);
 CHECK(set_module_log_level(&file,"12",2,&pos)==-EINVAL && !heap_count);CHECK(set_module_log_level(&file,"12 3 4",6,&pos)==-EINVAL && !heap_count);CHECK(set_module_log_level(&file,"-1 3",4,&pos)==-EINVAL && !heap_count);CHECK(set_module_log_level(&file,"4294967296 3",12,&pos)==-EINVAL && !heap_count);
 fw_result=-EOPNOTSUPP;CHECK(set_module_log_level(&file,"12 3",4,&pos)==-EOPNOTSUPP && !heap_count);char output[8192]={0};CHECK(get_module_ids(&file,output,sizeof(output),&pos)==-EOPNOTSUPP && !heap_count);
 fw_result=0;CHECK(get_module_ids(&file,output,sizeof(output),&pos)>0 && strstr(output,"XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX") && !heap_count);
 infinite_modules=true;pos=0;CHECK(get_module_ids(&file,output,sizeof(output),&pos)==-EOVERFLOW && !heap_count);
 mbox_result=-EIO;CHECK(set_log_enabled(&info,1)==-EIO && !info.enabled && !txdone_calls);
 mbox_result=1;CHECK(!set_log_enabled(&info,1) && info.enabled && txdone_calls==1);CHECK(!get_log_enabled(&info,&value) && value==1);
 cpucp_log_stop(&info);CHECK(info.stopping && !info.wq && frees==2 && cancels==1 && destroys==1);cpucp_log_stop(&info);CHECK(frees==2 && cancels==1);CHECK(set_log_level(&info,1)==-ENODEV && get_log_enabled(&info,&value)==-ENODEV);
 setup(CPUCP);pdev.count=1;CHECK(populate_free_buffers(&pdev,&info)==-EINVAL);
 setup(CPUCP);pdev.resources[1].end--;CHECK(populate_free_buffers(&pdev,&info)==-EINVAL);
 setup(CPUCP);pdev.resources[0].start=UINT64_MAX-1;CHECK(populate_free_buffers(&pdev,&info)==-EINVAL);
 setup(CPUCP);allocation_call=0;allocation_failure=1;CHECK(populate_free_buffers(&pdev,&info)==-ENOMEM);
 setup(CPUCP);info.tx_ch=info.ch;cpucp_log_stop(&info);CHECK(frees==1);
 reset();printf("%d\n",cases);return 0;
}
