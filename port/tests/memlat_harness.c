/* Host services only: all tested policy, parsing, transaction and teardown functions
 * are extracted verbatim from the staged kernel implementation. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <limits.h>
#include <stdarg.h>
#include <time.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef int64_t ktime_t;
typedef int spinlock_t;
struct mutex { int held; };
struct completion { bool done; };
struct kobject { int refs; };
struct attribute { int unused; };
struct work_struct { int unused; };
struct workqueue_struct { int live; };
struct hrtimer { bool initialized; };
struct rq { int unused; };
struct scmi_protocol_handle { int unused; };
struct device_node { const char *name; u32 *cells; int len, cpu, physical, refs; struct device_node *cpus[8]; int cpu_count; };
struct device { struct device_node *of_node; const char *name; };
struct platform_device { struct device dev; };
typedef unsigned int cpumask_t;
enum dcvs_hw_type { DCVS_DDR, DCVS_LLCC, DCVS_L3, DCVS_DDRQOS, DCVS_UBWCP, NUM_DCVS_HW_TYPES };
enum dcvs_path_type { DCVS_SLOW_PATH, DCVS_FAST_PATH, DCVS_PERCPU_PATH, NUM_DCVS_PATHS };
struct dcvs_freq { u32 ib, ab; enum dcvs_hw_type hw_type; };
#define QCOM_PMU_MAX_EVS 8
#define MAX_CPUCP_EVT 9
struct qcom_pmu_data { u32 event_ids[8]; u64 ev_data[8]; u32 num_evs; };
struct qcom_pmu_notif_node { void (*idle_cb)(struct qcom_pmu_data *, int, int); };
struct scmi_device;
struct scmi_handle { void (*devm_protocol_put)(struct scmi_device *, unsigned int); };
struct scmi_device { struct device dev; struct scmi_handle *handle; };
struct qcom_scmi_vendor_ops {
 int (*set_param)(struct scmi_protocol_handle *, void *, u64, u32, size_t);
 int (*stop_activity)(struct scmi_protocol_handle *, void *, u64, u32, size_t);
};
#define __packed __attribute__((packed))
#define BIT(n) (1U << (n))
#define U32_MAX UINT32_MAX
#define U16_MAX UINT16_MAX
#define GFP_KERNEL 0
#define WQ_FREEZABLE 1
#define WQ_UNBOUND 2
#define HRTIMER_MODE_REL 0
#define QCOM_SCMI_VENDOR_PROTOCOL 0x80
#define HZ 250
#define NSEC_PER_SEC 1000000000LL
#define HALF_TICK_NS ((NSEC_PER_SEC/HZ)>>1)
#define COREDEV_TBL_PROP "qcom,cpufreq-memfreq-tbl"
#define NUM_COLS 2
#define MIN_SAMPLE_MS 4U
#define MAX_SAMPLE_MS 1000U
#define NR_CPUS 4
#define nr_cpu_ids NR_CPUS
#define num_possible_cpus() NR_CPUS
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define max_t(t,a,b) max((t)(a),(t)(b))
#define unlikely(a) (a)
#define READ_ONCE(a) (a)
#define WRITE_ONCE(a,b) ((a)=(b))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define to_memlat_mon(p) container_of(p,struct memlat_mon,kobj)
#define to_memlat_grp(p) container_of(p,struct memlat_group,kobj)
#define per_cpu(v,c) ((v)[c])
#define for_each_possible_cpu(c) for ((c)=0;(c)<NR_CPUS;(c)++)
#define for_each_cpu(c,m) for_each_possible_cpu(c) if ((*(m))&BIT(c))
#define cpu_possible(c) ((c)>=0 && (c)<NR_CPUS)
#define cpumask_clear(m) (*(m)=0)
#define cpumask_set_cpu(c,m) (*(m)|=BIT(c))
#define cpumask_empty(m) (!*(m))
static int cpumask_first(const cpumask_t *m) { return *m ? __builtin_ctz(*m) : NR_CPUS; }
#define local_irq_save(f) ((f)=0)
#define local_irq_restore(f) ((void)(f))
#define spin_lock(p) ((void)(p))
#define spin_lock_nested(p,l) ((void)(p),(void)(l))
#define spin_unlock(p) ((void)(p))
#define spin_lock_irqsave(p,f) ((f)=0,(void)(p))
#define spin_unlock_irqrestore(p,f) ((void)(p),(void)(f))
#define spin_lock_init(p) (*(p)=0)
static void fake_log(const char *fmt,...) { (void)fmt; }
#define pr_err(...) fake_log(__VA_ARGS__)
#define pr_debug(...) ((void)0)
#define dev_err(...) ((void)0)
#define trace_memlat_dev_meas(...) ((void)0)
#define trace_memlat_dev_update(...) ((void)0)
#define mutex_lock(p) (++(p)->held)
#define mutex_unlock(p) (--(p)->held)
#define INIT_WORK(w,f) ((void)(w),(void)(f))
static u64 div64_u64(u64 a,u64 b) { assert(b); return a/b; }
static u64 mul_u64_u64_div_u64(u64 a,u64 m,u64 d) { assert(d); return ((__uint128_t)a*m)/d; }
static ktime_t fake_now;
static ktime_t ktime_get(void) { return fake_now; }
static s64 ktime_us_delta(ktime_t a,ktime_t b) { return (a-b)/1000; }
static ktime_t ms_to_ktime(u32 ms) { return (u64)ms*1000000; }
static const char *dev_name(struct device *d) { return d->name; }
static int kstrtouint(const char *s,int base,u32 *v) { char *e; unsigned long long x; errno=0; x=strtoull(s,&e,base); if(errno || e==s || *e || x>UINT_MAX || *s=='-') return -EINVAL; *v=x; return 0; }
/* INSERT_DECLARATIONS */
static struct memlat_dev_data root;
static struct memlat_dev_data *memlat_data=&root;
static struct cpu_stats *sampling_stats[NR_CPUS];
static struct mutex memlat_lock;
static struct qcom_pmu_notif_node memlat_idle_notif;
static void memlat_jiffies_update_cb(void *u,void *e) { (void)u;(void)e; }
enum hrtimer_restart { HRTIMER_NORESTART };
static int cases, allocation_fail, queue_live, hook_fail, scheduler_live, jiffies_live, idle_live;
static int trace_sync, timer_cancel, work_cancel, scmi_put, device_put, stop_calls;
static int fw_calls, mapping_result, mapping_event;
static unsigned long possible_mask=15;
#define cpu_possible_mask (&possible_mask)
#define cpumask_bits(p) (p)
static int qcom_get_cpucp_id(u32 event,int cpu) { (void)cpu;return event==(u32)mapping_event ? mapping_result : 2; }
static int fw_result, vote_result, vote_calls, voter_unregister, pmu_result, logical_cpu, physical_cpu;
static struct dcvs_freq last_votes[MAX_MEMLAT_GRPS];
static u32 last_mask, last_fw_value;
static enum dcvs_path_type last_path;
static void *allocations[64];
static int allocation_count;
static void *devm_kzalloc(struct device *d,size_t bytes,int flags) { (void)d;(void)flags; if(allocation_fail)return NULL; void *p=calloc(1,bytes); assert(p && allocation_count<64); allocations[allocation_count++]=p; return p; }
static void free_allocations(void) { while(allocation_count)free(allocations[--allocation_count]); }
static struct workqueue_struct *alloc_workqueue(const char *n,int f,int parallel) { (void)n;(void)f;assert(parallel==1); if(hook_fail==4)return NULL; queue_live++; return calloc(1,sizeof(struct workqueue_struct)); }
static void destroy_workqueue(struct workqueue_struct *q) { assert(q && queue_live>0);queue_live--;free(q); }
static void hrtimer_setup(struct hrtimer *t,enum hrtimer_restart (*fn)(struct hrtimer *),int clock,int mode) { (void)fn;(void)clock;(void)mode;t->initialized=true; }
static void hrtimer_cancel(struct hrtimer *t) { assert(t->initialized);timer_cancel++; }
static void cancel_work_sync(struct work_struct *w) { (void)w;work_cancel++; }
static void queue_work(struct workqueue_struct *q,struct work_struct *w) { (void)w;assert(q); }
static int register_trace_android_vh_scheduler_tick(void (*f)(void *,struct rq *),void *u) { (void)f;(void)u;if(hook_fail==1)return -EIO;scheduler_live++;return 0; }
static void unregister_trace_android_vh_scheduler_tick(void (*f)(void *,struct rq *),void *u) { (void)f;(void)u;assert(scheduler_live==1);scheduler_live--; }
static int register_trace_android_vh_jiffies_update(void (*f)(void *,void *),void *u) { (void)f;(void)u;if(hook_fail==2)return -EIO;jiffies_live++;return 0; }
static void unregister_trace_android_vh_jiffies_update(void (*f)(void *,void *),void *u) { (void)f;(void)u;assert(jiffies_live==1);jiffies_live--; }
static void tracepoint_synchronize_unregister(void) { trace_sync++; }
static int qcom_pmu_idle_register(struct qcom_pmu_notif_node *n) { (void)n;if(hook_fail==3)return -EIO;idle_live++;return 0; }
static int qcom_pmu_idle_unregister(struct qcom_pmu_notif_node *n) { (void)n;assert(idle_live==1);idle_live--;return 0; }
static void protocol_put(struct scmi_device *s,unsigned int id) { (void)s;assert(id==QCOM_SCMI_VENDOR_PROTOCOL);scmi_put++; }
static void put_device(struct device *d) { (void)d;device_put++; }
static int scmi_set(struct scmi_protocol_handle *p,void *msg,u64 algo,u32 id,size_t bytes) { (void)p;assert(algo==MEMLAT_ALGO_STR);fw_calls++;if(id==MEMLAT_SET_COMMON_EV_MAP || id==MEMLAT_SET_GRP_EV_MAP || id==MEMLAT_SET_MEM_GROUP) return fw_result; if(bytes==sizeof(struct scalar_param_msg)) last_fw_value=((struct scalar_param_msg *)msg)->val; else { assert(id==MEMLAT_SAMPLE_MS);last_fw_value=*(u32 *)msg; }return fw_result; }
static int scmi_stop(struct scmi_protocol_handle *p,void *msg,u64 algo,u32 id,size_t bytes) { (void)p;assert(!msg && !bytes && algo==MEMLAT_ALGO_STR && id==MEMLAT_STOP_TIMER);stop_calls++;return fw_result; }
static const struct qcom_scmi_vendor_ops ops={scmi_set,scmi_stop};
static int qcom_dcvs_update_votes(const char *name,struct dcvs_freq *v,u32 mask,enum dcvs_path_type path) { assert(name);vote_calls++;last_mask=mask;last_path=path;memset(last_votes,0,sizeof(last_votes));for(int i=0;i<MAX_MEMLAT_GRPS;i++)if(mask&BIT(i))last_votes[i]=v[i];return vote_result; }
static int qcom_dcvs_unregister_voter(const char *name,int hw,int path) { (void)name;(void)hw;(void)path;voter_unregister++;return 0; }
static int smp_processor_id(void) { return logical_cpu; }
static int qcom_pmu_read_all_local(struct qcom_pmu_data *p) { p->num_evs=pmu_result ? 0 : 1;p->event_ids[0]=1;p->ev_data[0]=42;return pmu_result; }
static struct device_node *of_parse_phandle(struct device_node *n,const char *name,int i) { assert(!strcmp(name,"qcom,cpulist"));if(i>=n->cpu_count)return NULL;n->cpus[i]->refs++;return n->cpus[i]; }
static int of_cpu_node_to_id(struct device_node *n) { return n->cpu; }
static void of_node_put(struct device_node *n) { assert(n && n->refs>0);n->refs--; }
static int cpu_logical_to_phys(int cpu) { (void)cpu;return physical_cpu; }
static void *of_find_property(struct device_node *n,const char *p,int *len) { (void)p;*len=n->len;return n->cells; }
static int of_property_read_u32_index(struct device_node *n,const char *p,int index,u32 *v) { (void)p;if(index<0 || index>=(n->len/4))return -EINVAL;*v=n->cells[index];return 0; }
/* INSERT_FUNCTIONS */
/* INSERT_TUNABLES */
store_attr(ipm_ceil,0U,50000U,MEMLAT_IPM_CEIL)
store_grp_attr(adaptive_low_freq,0U,U32_MAX,MEMLAT_ADAPTIVE_LOW_FREQ)
#define CHECK(e) do { assert(e); cases++; } while(0)
static void reset(void) { assert(!queue_live && !scheduler_live && !jiffies_live && !idle_live && !memlat_lock.held);free_allocations(); memset(&root,0,sizeof(root));memset(sampling_stats,0,sizeof(sampling_stats));fw_result=vote_result=vote_calls=hook_fail=allocation_fail=0;trace_sync=timer_cancel=work_cancel=scmi_put=device_put=stop_calls=0;static struct device dev;root.dev=&dev; }
static void numeric(void) {
 struct cpufreq_memfreq_map maps[]={{1000,100000},{2000,200000},{3000,300000}};
 struct memlat_mon mon={.freq_map=maps,.freq_map_len=3};
 CHECK(cpufreq_to_memfreq(&mon,0)==100000);CHECK(cpufreq_to_memfreq(&mon,1000)==100000);
 CHECK(cpufreq_to_memfreq(&mon,1001)==200000);CHECK(cpufreq_to_memfreq(&mon,UINT_MAX)==300000);
 mon.freq_map_len=0;CHECK(cpufreq_to_memfreq(&mon,2000)==0);
 CHECK(memlat_counter_delta(1,UINT64_MAX)==0);CHECK(memlat_counter_delta(50,40)==10);
 CHECK(memlat_percent(10,0)==0);CHECK(memlat_percent(UINT64_MAX,UINT64_MAX)==100);
 CHECK(memlat_percent(UINT64_MAX/2,UINT64_MAX)==49);CHECK(memlat_percent(200,100)==100);
 struct device dev={0};u32 rows=0;u32 values[]={1000000,100000,2000000,200000};struct device_node n={.cells=values,.len=sizeof(values)};
 CHECK(init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows) && rows==2);
 n.len=4;CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows));n.len=0;CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows));n.len=sizeof(values);
 values[2]=999999;CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows));values[2]=2000000;
 values[0]=0;CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows));values[0]=1000000;
 values[3]=50000;CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows));values[3]=200000;
 values[2]=UINT32_MAX;CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows));values[2]=2000000;
 values[3]=UINT32_MAX;CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows));values[3]=200000;
 n.len=15*8;CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows));n.len=sizeof(values);
 allocation_fail=1;CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows));allocation_fail=0;
 /* Exact native tuna DDRQOS gold, prime and prime-latfloor map rows. */
 u32 qos_maps[][4]={{2073600,0,3014400,1},{1478400,0,3206400,1},{2169600,0,3206400,1}};
 for(unsigned int q=0;q<3;q++) {
  n.cells=qos_maps[q];struct cpufreq_memfreq_map *qos=init_cpufreq_memfreq_map(&dev,&n,DCVS_DDRQOS,&rows);
  CHECK(qos && rows==2 && qos[0].memfreq_khz==0 && qos[1].memfreq_khz==1);
  struct memlat_mon qmon={.freq_map=qos,.freq_map_len=rows};
  CHECK(cpufreq_to_memfreq(&qmon,qos[0].cpufreq_mhz)==0 && cpufreq_to_memfreq(&qmon,qos[0].cpufreq_mhz+1)==1);
  CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_DDR,&rows));
  CHECK(!init_cpufreq_memfreq_map(&dev,&n,DCVS_LLCC,&rows));
 }
 n.cells=values;
 struct device_node cpu={.cpu=0};n.cpu_count=1;n.cpus[0]=&cpu;struct platform_device p={.dev={.of_node=&n}};cpumask_t mask;u32 mpidr;
 physical_cpu=31;CHECK(!get_mask_and_mpidr_from_pdev(&p,&mask,&mpidr) && mask==1 && mpidr==BIT(31) && !cpu.refs);
 physical_cpu=32;CHECK(get_mask_and_mpidr_from_pdev(&p,&mask,&mpidr)==-ERANGE && !cpu.refs);
 physical_cpu=-EIO;CHECK(get_mask_and_mpidr_from_pdev(&p,&mask,&mpidr)==-EIO);
 cpu.cpu=-1;CHECK(get_mask_and_mpidr_from_pdev(&p,&mask,&mpidr)==-EINVAL);cpu.cpu=NR_CPUS;CHECK(get_mask_and_mpidr_from_pdev(&p,&mask,&mpidr)==-EINVAL);
 n.cpu_count=0;CHECK(get_mask_and_mpidr_from_pdev(&p,&mask,&mpidr)==-ENODEV);
}
static void transactions(void) {
 reset();struct device dev={.name="grp"};struct memlat_group grp={.dev=&dev,.hw_type=1,.hw_min_freq=100,.hw_max_freq=1000};struct memlat_mon mon={.memlat_grp=&grp,.type=CPUCP_MON,.index=2,.min_freq=100,.max_freq=1000,.ipm_ceil=400};root.ops=&ops;root.groups[1]=&grp;grp.cpucp_enabled=true;
 fw_result=-EIO;CHECK(store_min_freq(&mon.kobj,NULL,"300",3)==-EIO && mon.min_freq==100 && last_fw_value==300);
 CHECK(store_max_freq(&mon.kobj,NULL,"700",3)==-EIO && mon.max_freq==1000);
 CHECK(store_ipm_ceil(&mon.kobj,NULL,"600",3)==-EIO && mon.ipm_ceil==400);
 CHECK(store_adaptive_low_freq(&grp.kobj,NULL,"700",3)==-EIO && !grp.adaptive_low_freq);
 root.sample_ms=8;root.cpucp_sample_ms=8;CHECK(store_sample_ms(NULL,NULL,"20",2)==-EIO && root.sample_ms==8 && root.cpucp_sample_ms==8);
 fw_result=0;CHECK(store_min_freq(&mon.kobj,NULL,"300",3)==3 && mon.min_freq==300);
 CHECK(store_max_freq(&mon.kobj,NULL,"700",3)==3 && mon.max_freq==700);
 CHECK(store_ipm_ceil(&mon.kobj,NULL,"90000",5)==5 && mon.ipm_ceil==50000 && last_fw_value==50000);
 CHECK(store_adaptive_low_freq(&grp.kobj,NULL,"700",3)==3 && grp.adaptive_low_freq==700);
 CHECK(store_sample_ms(NULL,NULL,"2",1)==1 && root.sample_ms==4 && root.cpucp_sample_ms==4);
 root.ops=NULL;CHECK(store_ipm_ceil(&mon.kobj,NULL,"1",1)==-ENODEV && mon.ipm_ceil==50000);
 mon.type=SAMPLING_MON;CHECK(store_min_freq(&mon.kobj,NULL,"0",1)==1 && mon.min_freq==100);
 CHECK(store_max_freq(&mon.kobj,NULL,"-1",2)==-EINVAL && mon.max_freq==700);
}
static void voting(void) {
 reset();struct device dev={.name="grp"};u32 fp[NUM_FP_VOTERS]={0},freqs[MAX_MEMLAT_GRPS]={0,300,0};struct memlat_group grp={.dev=&dev,.hw_type=1,.fp_voting_enabled=true,.fp_votes=fp,.sampling_path_type=DCVS_FAST_PATH};root.groups[1]=&grp;root.fp_enabled=true;
 update_memlat_fp_vote(-1,freqs);update_memlat_fp_vote(NUM_FP_VOTERS,freqs);CHECK(!vote_calls);
 vote_result=-EIO;update_memlat_fp_vote(0,freqs);CHECK(grp.fp_freq==0 && fp[0]==300 && last_votes[1].ib==300 && last_votes[1].ab==0 && last_mask==BIT(1));
 vote_result=0;update_memlat_fp_vote(0,freqs);CHECK(grp.fp_freq==300);
 freqs[1]=200;update_memlat_fp_vote(1,freqs);CHECK(grp.fp_freq==300 && vote_calls==2);
 freqs[1]=0;update_memlat_fp_vote(0,freqs);CHECK(grp.fp_freq==200);
 grp.sampling_registered=true;vote_result=-EIO;memlat_voter_cleanup(&grp);CHECK(!grp.sampling_registered && voter_unregister==1 && last_mask==BIT(1) && !last_votes[1].ib);
 memlat_voter_cleanup(&grp);CHECK(voter_unregister==1);
 grp.fp_voting_enabled=false;grp.sampling_path_type=DCVS_SLOW_PATH;grp.sampling_registered=true;grp.sampling_cur_freq=400;root.inited=true;
 vote_result=-EIO;memlat_update_work(&root.work);CHECK(grp.sampling_cur_freq==400 && last_path==DCVS_SLOW_PATH && last_votes[0].hw_type==1);
 vote_result=0;memlat_update_work(&root.work);CHECK(!grp.sampling_cur_freq);
}
static void cpucp_mapping(void) {
 reset();root.ops=&ops;root.common_ev_ids[INST_IDX]=1;root.common_ev_ids[CYC_IDX]=2;
 mapping_event=1;mapping_result=-EPROTO;fw_calls=0;CHECK(configure_cpucp_common_events()==-EPROTO && !fw_calls);
 mapping_result=MAX_CPUCP_EVT;CHECK(configure_cpucp_common_events()==-EOPNOTSUPP && !fw_calls);
 mapping_result=0;CHECK(!configure_cpucp_common_events() && fw_calls==1);
 root.common_ev_ids[FE_STALL_IDX]=4;mapping_event=4;mapping_result=-ENODEV;CHECK(configure_cpucp_common_events()==-ENODEV && fw_calls==1);
 struct device_node n={.name="grp"};struct device dev={.of_node=&n};struct memlat_group grp={.dev=&dev,.grp_ev_ids={3,0,0}};
 mapping_event=3;mapping_result=-EPROTO;fw_calls=0;CHECK(configure_cpucp_grp(&grp)==-EPROTO && fw_calls==1);
 mapping_result=MAX_CPUCP_EVT;fw_calls=0;CHECK(configure_cpucp_grp(&grp)==-EOPNOTSUPP && fw_calls==1);
 mapping_result=2;fw_calls=0;CHECK(!configure_cpucp_grp(&grp) && fw_calls==2);
}
static void lifecycle(void) {
 for(int failure=1;failure<=4;failure++) {reset();hook_fail=failure;CHECK(memlat_sampling_init()<0);CHECK(!root.scheduler_hook && !root.jiffies_hook && !root.idle_notifier && !root.memlat_wq && !queue_live && !scheduler_live && !jiffies_live && !idle_live);}
 reset();allocation_fail=1;CHECK(memlat_sampling_init()==-ENOMEM && !queue_live);allocation_fail=0;
 CHECK(!memlat_sampling_init() && queue_live==1 && scheduler_live && jiffies_live && idle_live);
 root.inited=true;root.sampling_enabled=true;root.cpucp_started=true;root.ops=&ops;struct scmi_handle handle={protocol_put};struct scmi_device scmi={.handle=&handle};root.scmi_dev=&scmi;fw_result=-EIO;
 memlat_stop();CHECK(root.stopping && !root.inited && !root.sampling_enabled && !root.memlat_wq && !root.scmi_dev && !root.ops);
 CHECK(!queue_live && !scheduler_live && !jiffies_live && !idle_live && timer_cancel==1 && work_cancel==1 && stop_calls==1 && scmi_put==1 && device_put==1);
 memlat_stop();CHECK(timer_cancel==1 && scmi_put==1);
 reset();CHECK(!memlat_sampling_init());root.inited=true;root.sample_ms=8;root.common_ev_ids[0]=1;root.common_ev_ids[1]=2;
 fake_now=10000000;logical_cpu=0;pmu_result=-EIO;memlat_sched_tick_cb(NULL,NULL);CHECK(!sampling_stats[0]->sample_ts);
 pmu_result=0;memlat_sched_tick_cb(NULL,NULL);CHECK(sampling_stats[0]->sample_ts==fake_now && sampling_stats[0]->curr.common_ctrs[0]==42);
 struct qcom_pmu_data raw={.num_evs=UINT_MAX};for(int i=0;i<8;i++){raw.event_ids[i]=1;raw.ev_data[i]=i;}
 memlat_pmu_idle_cb(&raw,0,0);CHECK(sampling_stats[0]->idle_sample && sampling_stats[0]->curr.common_ctrs[0]==7);
 memlat_pmu_idle_cb(&raw,NR_CPUS,0);memlat_pmu_idle_cb(NULL,0,0);CHECK(sampling_stats[0]->curr.common_ctrs[0]==7);
 sampling_stats[0]->prev.common_ctrs[1]=UINT64_MAX;sampling_stats[0]->curr.common_ctrs[1]=10;calculate_sampling_stats();CHECK(!sampling_stats[0]->freq_mhz);
 struct device dev={.name="grp"};struct memlat_group grp={.dev=&dev};root.groups[0]=&grp;grp.grp_ev_ids[0]=3;sampling_stats[0]->curr.common_ctrs[0]=UINT64_MAX;sampling_stats[0]->curr.grp_ctrs[0][0]=2;calculate_sampling_stats();CHECK(sampling_stats[0]->ipm[0]==U32_MAX);
 root.groups[0]=NULL;memlat_stop();CHECK(!queue_live);
}
int main(void) {numeric();transactions();voting();cpucp_mapping();lifecycle();reset();printf("%d\n",cases);}
