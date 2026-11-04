#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef unsigned int cpumask_t;
#define nr_cpu_ids 8
#define EPROBE_DEFER 517
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,x) ((v)=(x))
#define GFP_KERNEL 0
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define for_each_cpu(c,m) for((c)=0;(c)<nr_cpu_ids;(c)++)if((*(m))&(1U<<(c)))
struct device {void *data;void *of_node;};
struct platform_device {struct device dev;};
struct mbox_chan {int unused;};
struct mbox_client {struct device *dev;void (*rx_callback)(struct mbox_client *,void *);};
struct cpufreq_policy {cpumask_t *related_cpus;};
/* INSERT_DECLARATIONS */
static int cases, policy_gets, policy_puts, mailbox_gets, mailbox_frees, calls, last_cpu;
static int property_error, policy_error, mailbox_error, action_error, allocation_error;
static u32 property_cpu=6;
static cpumask_t possible=255,policy_mask=192;
#define cpu_possible_mask (&possible)
static struct cpufreq_policy policy={&policy_mask};
static struct mbox_chan channel;
static struct qcom_cpucp_fast *allocated;
static void (*cleanup)(void *);
static void *cleanup_arg;
static bool cpumask_test_cpu(int cpu,const cpumask_t *mask) {assert(cpu>=0 && cpu<nr_cpu_ids);return (*mask)&(1U<<cpu);}
static bool cpu_possible(int cpu) {return cpu>=0 && cpu<nr_cpu_ids && (possible&(1U<<cpu));}
#define cpu_present cpu_possible
static void cpumask_copy(cpumask_t *to,const cpumask_t *from) {*to=*from;}
static bool cpumask_empty(const cpumask_t *m) {return !*m;}
static bool cpumask_subset(const cpumask_t *a,const cpumask_t *b) {return !(*a&~*b);}
static int of_property_read_u32(void *node,const char *name,u32 *value) {assert(!strcmp(name,"qcom,policy-cpus"));if(property_error)return property_error;*value=property_cpu;return 0;}
static void *devm_kzalloc(struct device *d,size_t bytes,int flags) {if(allocation_error)return NULL;allocated=calloc(1,bytes);assert(allocated);return allocated;}
static struct cpufreq_policy *cpufreq_cpu_get(unsigned int cpu) {assert(cpu<nr_cpu_ids);if(policy_error)return NULL;policy_gets++;return &policy;}
static void cpufreq_cpu_put(struct cpufreq_policy *p) {assert(p==&policy);policy_puts++;}
static void dev_set_drvdata(struct device *d,void *data) {d->data=data;}
static void *dev_get_drvdata(struct device *d) {return d->data;}
static void sched_walt_oscillate(unsigned int cpu) {assert(cpu==255 || cpu<nr_cpu_ids);calls++;last_cpu=cpu;}
static struct mbox_chan *mbox_request_channel(struct mbox_client *c,int index) {assert(!index);if(mailbox_error)return ERR_PTR(mailbox_error);mailbox_gets++;u64 payload=6;c->rx_callback(c,&payload);return &channel;}
static void mbox_free_channel(struct mbox_chan *c) {assert(c==&channel);mailbox_frees++;}
static int devm_add_action_or_reset(struct device *d,void (*fn)(void *),void *arg) {if(action_error){fn(arg);return -ENOMEM;}cleanup=fn;cleanup_arg=arg;return 0;}
static int dev_err_probe(struct device *d,int ret,const char *message) {return ret;}
/* INSERT_FUNCTIONS */
#define CHECK(c) do {assert(c);cases++;}while(0)
static void reset(void) {if(cleanup){cleanup(cleanup_arg);cleanup=NULL;}free(allocated);allocated=NULL;policy_gets=policy_puts=mailbox_gets=mailbox_frees=calls=last_cpu=0;property_error=policy_error=mailbox_error=action_error=allocation_error=0;property_cpu=6;possible=255;policy_mask=192;}
int main(void) {
 struct platform_device dev={0};reset();allocation_error=1;CHECK(qcom_cpucp_fast_probe(&dev)==-ENOMEM && !policy_gets);
 reset();property_error=-EINVAL;CHECK(qcom_cpucp_fast_probe(&dev)==-EINVAL && !policy_gets && !mailbox_gets);
 reset();property_cpu=255;CHECK(qcom_cpucp_fast_probe(&dev)==-EINVAL && !mailbox_gets);
 reset();possible&=~(1U<<6);CHECK(qcom_cpucp_fast_probe(&dev)==-EINVAL && !mailbox_gets);
 reset();policy_error=1;CHECK(qcom_cpucp_fast_probe(&dev)==-EPROBE_DEFER && !mailbox_gets);
 reset();policy_mask=0;CHECK(qcom_cpucp_fast_probe(&dev)==-EINVAL && policy_gets==policy_puts && !mailbox_gets);
 reset();policy_mask=128;CHECK(qcom_cpucp_fast_probe(&dev)==-EINVAL && policy_gets==policy_puts && !mailbox_gets);
 reset();possible=127;CHECK(qcom_cpucp_fast_probe(&dev)==-EINVAL && policy_gets==policy_puts && !mailbox_gets);
 reset();mailbox_error=-EPROBE_DEFER;CHECK(qcom_cpucp_fast_probe(&dev)==-EPROBE_DEFER && policy_gets==policy_puts && !mailbox_frees);
 reset();mailbox_error=-EIO;CHECK(qcom_cpucp_fast_probe(&dev)==-EIO && policy_gets==policy_puts && !mailbox_frees);
 reset();action_error=1;CHECK(qcom_cpucp_fast_probe(&dev)==-ENOMEM && mailbox_frees==1 && !allocated->ready);
 reset();CHECK(!qcom_cpucp_fast_probe(&dev) && allocated->ready && calls==1 && last_cpu==6 && policy_gets==policy_puts);
 u64 payload=255;qcom_cpucp_fast_rx(&allocated->cl,&payload);CHECK(calls==2 && last_cpu==255 && allocated->last_cpu==255);
 qcom_cpucp_fast_rx(&allocated->cl,&payload);CHECK(calls==2);
 payload=8;qcom_cpucp_fast_rx(&allocated->cl,&payload);payload=0;qcom_cpucp_fast_rx(&allocated->cl,&payload);qcom_cpucp_fast_rx(&allocated->cl,NULL);CHECK(calls==2);
 payload=7;qcom_cpucp_fast_rx(&allocated->cl,&payload);CHECK(calls==3 && last_cpu==7);
 qcom_cpucp_fast_shutdown(&dev);CHECK(!allocated->ready && mailbox_frees==1);
 payload=6;qcom_cpucp_fast_rx(&allocated->cl,&payload);CHECK(calls==3);
 qcom_cpucp_fast_stop(allocated);CHECK(mailbox_frees==1);
 reset();printf("%d\n",cases);return 0;
}
