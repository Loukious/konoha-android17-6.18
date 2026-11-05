#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <time.h>
typedef uint8_t u8;
#define __init
#define __exit
#define __user
#define unlikely(x) (x)
#define READ_ONCE(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p,v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define late_initcall(x)
#define module_exit(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
#define HZ 100UL
static unsigned long jiffies = 1000;
#define time_is_before_jiffies(x) ((long)((x)-jiffies)<0)
#define TASK_UNINTERRUPTIBLE 2
#define TASK_STOPPED 4
#define TASK_TRACED 8
#define PIDTYPE_PID 0
typedef struct { atomic_int value; } atomic_t;
#define atomic_read(p) atomic_load(&(p)->value)
#define atomic_set(p,v) atomic_store(&(p)->value,v)
#define atomic_xchg(p,v) atomic_exchange(&(p)->value,v)
static int atomic_cmpxchg(atomic_t *p,int old,int value) {atomic_compare_exchange_strong(&p->value,&old,value);return old;}
static int atomic_inc_return(atomic_t *p) {return atomic_fetch_add(&p->value,1)+1;}
struct mutex {pthread_mutex_t lock;};
#define DEFINE_MUTEX(n) struct mutex n={PTHREAD_MUTEX_INITIALIZER}
static void mutex_lock(struct mutex *m) {assert(!pthread_mutex_lock(&m->lock));}
static void mutex_unlock(struct mutex *m) {assert(!pthread_mutex_unlock(&m->lock));}
struct walt_task_struct {u8 hung_detect_status;};
struct task_struct {
    bool in_iowait;
    unsigned long __state,last_switch_time;
    void *mm;
    struct task_struct *group_leader;
    struct walt_task_struct storage;
    uint64_t android_vendor_data1[8]; /* Obsolete storage is deliberately separate. */
    atomic_int refs;
};
static struct task_struct task;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static bool block_callback,callback_entered,resume_callback,drain_entered;
static atomic_int active_callbacks,exit_returned;
static bool walt_disabled;
static int task_storage_ready;
/* WALT_AVAILABILITY */
static void publish_walt_ready(void) {
    /* WALT_PUBLICATION */
}
static struct walt_task_struct *task_walt(struct task_struct *t) {
    assert(t);
    assert(walt_task_data_available() && task_storage_ready);
    pthread_mutex_lock(&gate);
    if(block_callback){callback_entered=true;pthread_cond_broadcast(&gate_cond);while(!resume_callback)pthread_cond_wait(&gate_cond,&gate);}
    pthread_mutex_unlock(&gate);
    return &t->storage;
}
struct pid {int number;atomic_int refs;bool alive;struct task_struct *task;};
static struct pid test_pid;
static struct pid *find_get_pid(int n){if(n!=test_pid.number||!READ_ONCE(test_pid.alive))return NULL;atomic_fetch_add(&test_pid.refs,1);return &test_pid;}
static struct pid *find_vpid(int n){assert(!"PID lookup must hold a reference");return NULL;}
static struct task_struct *get_pid_task(struct pid *p,int type){if(!p)return NULL;assert(atomic_load(&p->refs)>0);if(!READ_ONCE(p->alive))return NULL;atomic_fetch_add(&p->task->refs,1);return p->task;}
static void put_pid(struct pid *p){if(p)assert(atomic_fetch_sub(&p->refs,1)>0);}
static void put_task_struct(struct task_struct *t){assert(atomic_fetch_sub(&t->refs,1)>0);}
struct ctl_table;
typedef int proc_handler(const struct ctl_table *,int,void *,size_t *,loff_t *);
struct ctl_table {const char *procname;void *data;int maxlen,mode;proc_handler *proc_handler;const int *extra1,*extra2;};
static const int zero=0,one=1,max_value=INT_MAX;
#define SYSCTL_ZERO (&zero)
#define SYSCTL_ONE (&one)
#define SYSCTL_INT_MAX (&max_value)
struct io {int n,v[2],error;};
static int proc_dointvec(const struct ctl_table *table,int write,void *buffer,size_t *len,loff_t *pos){
    struct io *io=buffer;if(io->error)return io->error;
    assert(table->data&&table->maxlen>0&&table->maxlen<=8);
    if(write){assert(io->n<=table->maxlen/(int)sizeof(int));memcpy(table->data,io->v,io->n*sizeof(int));}
    else{io->n=table->maxlen/sizeof(int);memcpy(io->v,table->data,io->n*sizeof(int));}
    return 0;
}
static int proc_dointvec_minmax(const struct ctl_table *table,int write,void *buffer,size_t *len,loff_t *pos){
    struct io *io=buffer;
    if(write&&!io->error&&((table->extra1&&io->v[0]<*table->extra1)||(table->extra2&&io->v[0]>*table->extra2)))return -EINVAL;
    return proc_dointvec(table,write,buffer,len,pos);
}
struct ctl_table_header {bool live;};
static struct ctl_table_header header;
static int hook1_error,hook2_error,sysctl_error,drains,panic_calls;
static bool hook1_live,hook2_live;
static struct ctl_table_header *register_sysctl_sz(const char *path,const struct ctl_table *table,size_t n){
    assert(!strcmp(path,"hung_task_enh")&&n==5);
    for(unsigned i=0;i<n;i++)assert(table[i].procname&&table[i].proc_handler&&table[i].mode==0644);
    if(sysctl_error)return NULL;assert(!header.live);header.live=true;return &header;
}
#define register_sysctl(path,table) register_sysctl_sz(path,table,sizeof(table)/sizeof(*table))
static void unregister_sysctl_table(struct ctl_table_header *p){assert(p==&header&&header.live);header.live=false;}
static int register_trace_android_vh_check_uninterruptible_tasks(void (*cb)(void*,struct task_struct*,unsigned long,bool*),void *priv){if(hook1_error)return hook1_error;assert(!hook1_live);hook1_live=true;return 0;}
static int register_trace_android_vh_check_uninterruptible_tasks_dn(void (*cb)(void*,void*),void *priv){if(hook2_error)return hook2_error;assert(!hook2_live);hook2_live=true;return 0;}
static void unregister_trace_android_vh_check_uninterruptible_tasks(void (*cb)(void*,struct task_struct*,unsigned long,bool*),void *priv){assert(hook1_live);hook1_live=false;}
static void unregister_trace_android_vh_check_uninterruptible_tasks_dn(void (*cb)(void*,void*),void *priv){assert(hook2_live);hook2_live=false;}
static void tracepoint_synchronize_unregister(void){
    drains++;pthread_mutex_lock(&gate);drain_entered=true;pthread_cond_broadcast(&gate_cond);
    while(atomic_load(&active_callbacks))pthread_cond_wait(&gate_cond,&gate);
    pthread_mutex_unlock(&gate);
}
static void panic(const char *message){assert(strstr(message,"IO wait"));panic_calls++;}
/* DRIVER */
static void reset(void){
    assert(!hook1_live&&!hook2_live&&!header.live&&!atomic_load(&active_callbacks));
    assert(!atomic_load(&test_pid.refs)&&!atomic_load(&task.refs));
    memset(&hung_task_enh,0,sizeof(hung_task_enh));memset(&task,0,sizeof(task));
    task.mm=(void*)1;task.group_leader=&task;task.in_iowait=true;task.__state=TASK_UNINTERRUPTIBLE;task.last_switch_time=899;
    test_pid.number=123;test_pid.task=&task;test_pid.alive=true;
    walt_disabled=false;task_storage_ready=1;hook1_error=hook2_error=sysctl_error=drains=panic_calls=0;
    block_callback=callback_entered=resume_callback=drain_entered=false;atomic_store(&exit_returned,0);
}
static int ctl(unsigned index,bool write,struct io *io){size_t len=8;loff_t pos=0;return hung_task_table[index].proc_handler(&hung_task_table[index],write,io,&len,&pos);}
static void *increment_many(void *counter){for(unsigned i=0;i<10000;i++)hung_task_saturating_inc(counter);return NULL;}
static void *run_callback(void *unused){
    atomic_fetch_add(&active_callbacks,1);bool need=false;qcom_before_check_tasks(NULL,&task,1,&need);assert(need);
    pthread_mutex_lock(&gate);atomic_fetch_sub(&active_callbacks,1);pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate);return NULL;
}
static void *run_exit(void *unused){hung_task_enh_exit();atomic_store(&exit_returned,1);return NULL;}
static atomic_bool readiness_probe;
static void *wait_for_task_data(void *unused){
    bool need=true;
    qcom_before_check_tasks(NULL,&task,1,&need);
    assert(need); /* Original scanner decision survives unavailable storage. */
    atomic_store_explicit(&readiness_probe,true,memory_order_relaxed);
    do {
        need=true;
        qcom_before_check_tasks(NULL,&task,1,&need);
        if(need)sched_yield();
    } while(need);
    assert(task_storage_ready && task.storage.hung_detect_status==TASK_IN_BLOCKLIST);
    return NULL;
}
int main(void){unsigned cases=0;
    /* Init errors must leave no hook/sysctl and drain registered callbacks. */
    for(int ready=0;ready<2;ready++)for(int fault=1;fault<4;fault++){
        reset();walt_disabled=!ready;task_storage_ready=ready;if(fault==1)hook1_error=-EIO;if(fault==2)hook2_error=-EIO;if(fault==3)sysctl_error=-ENOMEM;
        assert(hung_task_enh_init()==(fault==3?-ENOMEM:-EIO));
        assert(!hook1_live&&!hook2_live&&!header.live&&drains==(fault>=2));cases++;
    }
    /* Delayed or failed WALT init must not fail module load or touch storage. */
    for(int becomes_ready=0;becomes_ready<2;becomes_ready++){
        reset();walt_disabled=true;task_storage_ready=0;
        assert(!hung_task_enh_init()&&hook1_live&&hook2_live&&header.live);
        for(int initial=0;initial<2;initial++){
            bool need=initial;qcom_before_check_tasks(NULL,&task,1,&need);
            assert(need==initial&&!atomic_read(&hung_task_enh.curr_iowait_task_cnt));cases++;
        }
        struct io io={.n=1,.v={123}};
        assert(!ctl(4,true,&io)); /* Configuration independent of task data works. */
        io.n=2;io.v[0]=123;io.v[1]=1;
        assert(ctl(1,true,&io)==-EAGAIN&&ctl(1,false,&io)==-EAGAIN);
        assert(!task.storage.hung_detect_status&&!atomic_load(&test_pid.refs)&&!atomic_load(&task.refs));
        hung_task_enh.max_iowait_timeout_cnt=1;hung_task_enh.max_iowait_task_cnt=1;
        qcom_check_tasks_done(NULL,NULL);assert(!panic_calls);
        if(becomes_ready){
            task_storage_ready=1;publish_walt_ready();
            assert(!ctl(1,true,&io)&&task.storage.hung_detect_status==TASK_IN_ALLOWLIST);
            io.n=0;assert(!ctl(1,false,&io)&&io.n==2&&io.v[0]==123&&io.v[1]==TASK_IN_ALLOWLIST);
            bool need=false;qcom_before_check_tasks(NULL,&task,1,&need);assert(need);
            qcom_check_tasks_done(NULL,NULL);assert(panic_calls==1);
        }
        hung_task_enh_exit();assert(drains==1&&!hook1_live&&!hook2_live&&!header.live);cases+=8;
    }
    /* A concurrent consumer sees all task initialization after publication. */
    reset();walt_disabled=true;task_storage_ready=0;assert(!hung_task_enh_init());
    pthread_t readiness_reader;atomic_store(&readiness_probe,false);
    assert(!pthread_create(&readiness_reader,NULL,wait_for_task_data,NULL));
    while(!atomic_load_explicit(&readiness_probe,memory_order_relaxed))sched_yield();
    task.storage.hung_detect_status=TASK_IN_BLOCKLIST;task_storage_ready=1;publish_walt_ready();
    assert(!pthread_join(readiness_reader,NULL));hung_task_enh_exit();cases++;
    reset();assert(!hung_task_enh_init());assert(!hung_task_enh.global_detect_mode&&hung_task_enh.max_iowait_task_cnt==5&&!hung_task_enh.max_iowait_timeout_cnt&&!hung_task_enh.read_pid);cases++;
    /* Native allowlist/blocklist selection, including each task status. */
    for(int mode=0;mode<2;mode++)for(int status=0;status<3;status++){
        hung_task_enh.global_detect_mode=mode;task.storage.hung_detect_status=status;bool need=false;
        qcom_before_check_tasks(NULL,&task,1,&need);assert(need==(mode?status!=2:status==1));atomic_set(&hung_task_enh.curr_iowait_task_cnt,0);cases++;
    }
    /* All I/O-wait eligibility dimensions and timeout equality boundary. */
    hung_task_enh.global_detect_mode=1;task.storage.hung_detect_status=0;
    for(int io=0;io<2;io++)for(int state=0;state<16;state++)for(int mm=0;mm<2;mm++)for(int leader=0;leader<2;leader++)for(unsigned last=0;last<902;last++){
        task.in_iowait=io;task.__state=state;task.mm=mm?(void*)1:NULL;task.group_leader=leader?&task:NULL;task.last_switch_time=last;
        atomic_set(&hung_task_enh.curr_iowait_task_cnt,0);bool need=false;qcom_before_check_tasks(NULL,&task,1,&need);assert(need);
        int expected=io&&(state==2||state==4||state==8)&&mm&&leader&&last&&last<900;
        assert(atomic_read(&hung_task_enh.curr_iowait_task_cnt)==expected);cases++;
    }
    /* Native thresholds, resets and disabled panic. Panic is only a mock. */
    for(int limit=0;limit<=5;limit++)for(int count=0;count<=7;count++)for(int previous=0;previous<=7;previous++){
        hung_task_enh.max_iowait_task_cnt=5;hung_task_enh.max_iowait_timeout_cnt=limit;atomic_set(&hung_task_enh.curr_iowait_timeout_cnt,previous);atomic_set(&hung_task_enh.curr_iowait_task_cnt,count);panic_calls=0;
        qcom_check_tasks_done(NULL,NULL);int consecutive=count>=5?previous+1:0;
        assert(!atomic_read(&hung_task_enh.curr_iowait_task_cnt));assert(atomic_read(&hung_task_enh.curr_iowait_timeout_cnt)==consecutive);assert(panic_calls==(limit&&consecutive>=limit));cases++;
    }
    hung_task_enh.max_iowait_timeout_cnt=0;atomic_set(&hung_task_enh.curr_iowait_timeout_cnt,INT_MAX);atomic_set(&hung_task_enh.curr_iowait_task_cnt,5);panic_calls=0;qcom_check_tasks_done(NULL,NULL);assert(atomic_read(&hung_task_enh.curr_iowait_timeout_cnt)==INT_MAX&&!panic_calls);
    atomic_t counter={ATOMIC_VAR_INIT(INT_MAX-1)};assert(hung_task_saturating_inc(&counter)==INT_MAX&&hung_task_saturating_inc(&counter)==INT_MAX);cases+=3;
    atomic_set(&counter,0);pthread_t workers[4];for(int i=0;i<4;i++)assert(!pthread_create(&workers[i],NULL,increment_many,&counter));for(int i=0;i<4;i++)assert(!pthread_join(workers[i],NULL));assert(atomic_read(&counter)==40000);cases+=40000;
    /* Sysctl bounds, stable read PID, task references and actual task storage. */
    int values[]={INT_MIN,-1,0,1,2,5,INT_MAX};
    for(unsigned index=0;index<5;index++)if(index!=1)for(unsigned v=0;v<sizeof(values)/sizeof(*values);v++){
        struct io io={.n=1,.v={values[v]}};int min=*hung_task_table[index].extra1,max=*hung_task_table[index].extra2;int rc=ctl(index,true,&io);assert(rc==((values[v]<min||values[v]>max)?-EINVAL:0));cases++;
    }
    for(int mode=0;mode<2;mode++)for(int value=-1;value<=2;value++)for(int pid=-1;pid<=124;pid++){
        hung_task_enh.global_detect_mode=mode;task.storage.hung_detect_status=0;struct io io={.n=2,.v={pid,value}};int rc=ctl(1,true,&io);bool valid=pid==123&&value>=0&&value<=1;assert(rc==(valid?0:-ENOENT));
        assert(task.storage.hung_detect_status==(valid&&value?(mode?2:1):0));assert(!atomic_load(&test_pid.refs)&&!atomic_load(&task.refs));cases++;
    }
    for(int status=0;status<3;status++){task.storage.hung_detect_status=status;hung_task_enh.read_pid=123;struct io io={0};assert(!ctl(1,false,&io)&&io.n==2&&io.v[0]==123&&io.v[1]==status);assert(!atomic_load(&test_pid.refs)&&!atomic_load(&task.refs));cases++;}
    test_pid.alive=false;struct io io={.n=2,.v={123,1}};assert(ctl(1,true,&io)==-ENOENT&&ctl(1,false,&io)==-ENOENT);test_pid.alive=true;io.error=-EFAULT;assert(ctl(1,true,&io)==-EFAULT&&ctl(1,false,&io)==-EFAULT);assert(!atomic_load(&test_pid.refs)&&!atomic_load(&task.refs));cases+=4;
    hung_task_enh_exit();assert(drains==1&&!hook1_live&&!hook2_live&&!header.live);
    /* Module exit must wait for a callback preempted inside native task access. */
    reset();assert(!hung_task_enh_init());task.storage.hung_detect_status=1;block_callback=true;pthread_t callback,exit_thread;
    assert(!pthread_create(&callback,NULL,run_callback,NULL));pthread_mutex_lock(&gate);while(!callback_entered)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);
    assert(!pthread_create(&exit_thread,NULL,run_exit,NULL));pthread_mutex_lock(&gate);while(!drain_entered)pthread_cond_wait(&gate_cond,&gate);assert(!atomic_load(&exit_returned));resume_callback=true;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate);
    assert(!pthread_join(callback,NULL)&&!pthread_join(exit_thread,NULL));assert(atomic_load(&exit_returned)&&!hook1_live&&!hook2_live&&!header.live);cases++;
    reset();printf("%u\n",cases);return 0;
}
