#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
typedef uint32_t u32;
#define GFP_KERNEL 0
#define PAGE_SIZE 4096
#define EPROBE_DEFER 517
#define QCOM_SCMI_VENDOR_PROTOCOL 128
#define current get_current()
#define min(a,b) ((a)<(b)?(a):(b))
#define cpu_to_le32(v) (v)
#define le32_to_cpu(v) (v)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define dev_err(...) ((void)0)
#define DEFINE_MUTEX(m) struct mutex m
#define DECLARE_COMPLETION(c) struct completion c
struct mutex {int held;};
struct completion {bool done;};
static void mutex_lock(struct mutex *m) {assert(!m->held);m->held=1;}
static void mutex_unlock(struct mutex *m) {assert(m->held);m->held=0;}
static void complete(struct completion *c) {c->done=true;}
static void reinit_completion(struct completion *c) {c->done=false;}
static void wait_for_completion(struct completion *c) {assert(c->done);}
struct kobject {int refs;};
struct attribute {int unused;};
struct device {struct kobject kobj;int refs;};
struct platform_device {struct device dev;};
struct scmi_protocol_handle {int unused;};
struct scmi_device;
struct qcom_scmi_vendor_ops {
 int (*start_activity)(struct scmi_protocol_handle *,void *,uint64_t,u32,size_t);
 int (*set_param)(struct scmi_protocol_handle *,void *,uint64_t,u32,size_t);
 int (*get_param)(struct scmi_protocol_handle *,void *,uint64_t,u32,size_t,size_t);
};
struct scmi_handle {
 const struct qcom_scmi_vendor_ops *(*devm_protocol_get)(struct scmi_device *,u32,struct scmi_protocol_handle **);
 void (*devm_protocol_put)(struct scmi_device *,u32);
};
struct scmi_device {struct device dev;struct scmi_handle *handle;};
/* INSERT_DECLARATIONS */
#if C1_KIND
#define ALGORITHM C1DCVS_ALGO_STR
#define PROBE scmi_c1dcvs_probe
#define READY c1dcvs_ready
#define RELEASE c1dcvs_kobject_release
#define ENTRY_TYPE struct qcom_c1dcvs_attr
#define ENABLE_STORE store_enable_c1dcvs
static int c1dcvs_settings_ktype;
#else
#define ALGORITHM DYNPF_ALGO_STR
#define PROBE qcom_dynpf_probe
#define READY dynpf_ready
#define RELEASE dynpf_kobject_release
#define ENTRY_TYPE struct qcom_dynpf_attr
#define ENABLE_STORE store_enable_dynpf
static int dynpf_settings_ktype;
#endif
static int cases, failure, fw_error, fw_calls, protocol_gets, protocol_puts, kobj_puts, heap_count;
static u32 last_values[2],last_id;
static struct device cpu_root;
static int cpu_subsys;
static struct scmi_device scmi;
static struct {void (*fn)(void *);void *arg;}actions[4];
static int action_count;
static int firmware_set(struct scmi_protocol_handle *ph,void *data,uint64_t algo,u32 id,size_t size) {assert(algo==ALGORITHM && (size==4 || size==8));fw_calls++;last_id=id;last_values[0]=*(u32 *)data;if(size==8)last_values[1]=((u32 *)data)[1];return fw_error;}
static int firmware_get(struct scmi_protocol_handle *ph,void *data,uint64_t algo,u32 id,size_t tx,size_t rx) {assert(algo==ALGORITHM && !tx && rx==4);fw_calls++;if(!fw_error)*(u32 *)data=17;return fw_error;}
static struct qcom_scmi_vendor_ops firmware_ops={firmware_set,firmware_set,firmware_get};
static const struct qcom_scmi_vendor_ops *protocol_get(struct scmi_device *sd,u32 id,struct scmi_protocol_handle **ph) {assert(sd==&scmi && id==128);if(failure==2)return ERR_PTR(-EOPNOTSUPP);protocol_gets++;*ph=NULL;return &firmware_ops;}
static void protocol_put(struct scmi_device *sd,u32 id) {assert(sd==&scmi && id==128);protocol_puts++;}
static struct scmi_handle handle={protocol_get,protocol_put};
static struct scmi_device *get_qcom_scmi_device(void) {return failure==1?ERR_PTR(-EPROBE_DEFER):&scmi;}
static void get_device(struct device *dev) {dev->refs++;}
static void put_device(struct device *dev) {assert(dev->refs>0);dev->refs--;}
static struct device *bus_get_dev_root(void *bus) {if(failure==4)return NULL;cpu_root.refs++;return &cpu_root;}
static int devm_add_action(struct device *dev,void (*fn)(void *),void *arg) {if(failure==3)return -ENOMEM;assert(action_count<4);actions[action_count++]=(typeof(actions[0])){fn,arg};return 0;}
static int devm_add_action_or_reset(struct device *dev,void (*fn)(void *),void *arg) {if(failure==6){fn(arg);return -ENOMEM;}return devm_add_action(dev,fn,arg);}
static int kobject_init_and_add(struct kobject *kobj,void *type,struct kobject *parent,const char *name) {assert(parent==&cpu_root.kobj);kobj->refs=1;return failure==5?-EINVAL:0;}
static void RELEASE(struct kobject *kobj);
static void kobject_put(struct kobject *kobj) {assert(kobj->refs==1);kobj->refs=0;kobj_puts++;RELEASE(kobj);}
static void kobject_del(struct kobject *kobj) {assert(kobj->refs==1);}
static int kstrtouint(const char *src,int base,u32 *value) {char *end;errno=0;unsigned long long v=strtoull(src,&end,base);if(errno || end==src || *end || *src=='-' || v>UINT32_MAX)return -EINVAL;*value=v;return 0;}
#define kstrtou32 kstrtouint
#if C1_KIND
static char *kstrdup(const char *src,int flags) {if(failure==7)return NULL;char *p=strdup(src);assert(p);heap_count++;return p;}
static void kfree(void *p) {if(p){assert(heap_count>0);heap_count--;free(p);}}
static char *strim(char *p) {while(isspace((unsigned char)*p))p++;size_t n=strlen(p);while(n && isspace((unsigned char)p[n-1]))p[--n]=0;return p;}
#else
static int scnprintf(char *b,size_t size,const char *fmt,...) {va_list ap;va_start(ap,fmt);int n=vsnprintf(b,size,fmt,ap);va_end(ap);return n<(int)size?n:(size?(int)size-1:0);}
#endif
/* INSERT_FUNCTIONS */
#define CHECK(c) do {assert(c);cases++;}while(0)
static void unwind(void) {while(action_count){action_count--;actions[action_count].fn(actions[action_count].arg);}}
int main(void) {
 scmi.handle=&handle;ENTRY_TYPE entry={.store=ENABLE_STORE};struct platform_device p={0};char output[80];
 CHECK(attr_store(NULL,&entry.attr,"1",1)==-ENODEV && !fw_calls);CHECK(attr_show(NULL,&entry.attr,output)==-ENODEV);
 for(failure=1;failure<=6;failure++){int ret=PROBE(&p);CHECK(ret<0 && !READY);unwind();CHECK(!ops && scmi.dev.refs==0 && cpu_root.refs==0 && protocol_gets==protocol_puts);}
 failure=0;CHECK(!PROBE(&p) && READY && ops && scmi.dev.refs==1);
 CHECK(PROBE(&p)==-EBUSY && scmi.dev.refs==1);
 fw_error=-EIO;CHECK(attr_store(NULL,&entry.attr,"0",1)==-EIO);
#if C1_KIND
 CHECK(user_c1dcvs_en==1 && kernel_c1dcvs_en==1);
 CHECK(c1dcvs_enable(false)==-EIO && kernel_c1dcvs_en==1);
 fw_error=0;CHECK(!c1dcvs_enable(false) && kernel_c1dcvs_en==0 && last_values[0]==0);
 CHECK(attr_store(NULL,&entry.attr,"1",1)==1 && last_values[0]==0 && user_c1dcvs_en==1);
 CHECK(!c1dcvs_enable(true) && last_values[0]==1);
 u32 pair[2];CHECK(!c1dcvs_parse_threshold(" 0 1234\n",pair) && pair[0]==0 && pair[1]==1234 && !heap_count);
 CHECK(c1dcvs_parse_threshold("0",pair)==-EINVAL && !heap_count);CHECK(c1dcvs_parse_threshold("0 1 2",pair)==-EINVAL && !heap_count);CHECK(c1dcvs_parse_threshold("-1 2",pair)==-EINVAL && !heap_count);CHECK(c1dcvs_parse_threshold("0 4294967296",pair)==-EINVAL && !heap_count);
 failure=7;CHECK(c1dcvs_parse_threshold("0 1",pair)==-ENOMEM);failure=0;
 fw_error=-EOPNOTSUPP;CHECK(store_ipc_thresh(NULL,NULL,"2 500",5)==-EOPNOTSUPP && last_id==PARAM_IPC_THRESH && last_values[0]==2 && last_values[1]==500);
 fw_error=0;CHECK(store_ipc_thresh(NULL,NULL,"2 500",5)==5);
#else
 fw_error=0;CHECK(attr_store(NULL,&entry.attr,"1",1)==1 && last_id==PARAM_ENABLE_DYNPF && last_values[0]==1);
 entry.show=show_enable_dynpf;CHECK(attr_show(NULL,&entry.attr,output)>0 && !strcmp(output,"17\n"));fw_error=-EOPNOTSUPP;CHECK(attr_show(NULL,&entry.attr,output)==-EOPNOTSUPP);
#endif
 CHECK(attr_store(NULL,&entry.attr,"-1",2)==-EINVAL);
 unwind();CHECK(!READY && !ops && !scmi.dev.refs && protocol_gets==protocol_puts);
 CHECK(attr_store(NULL,&entry.attr,"1",1)==-ENODEV);
#if C1_KIND
 CHECK(c1dcvs_enable(true)==-EPROBE_DEFER);
#endif
 fw_error=0;CHECK(!PROBE(&p) && READY);unwind();CHECK(!READY && !ops && protocol_gets==protocol_puts && !cpu_root.refs);
 CHECK(!heap_count);printf("%d\n",cases);return 0;
}
