#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>
typedef uint32_t u32;
struct rpmsg_device {};
struct rpmsg_endpoint { struct rpmsg_device *rpdev; void *priv; };
struct glink_channel { pthread_mutex_t recv_lock;struct rpmsg_endpoint ept;u32 remote_signals;int (*signals_cb)(struct rpmsg_device *,void *,u32,u32); };
struct qcom_glink { void *dev; };
struct glink_msg { uint16_t cmd,param1; uint32_t param2; };
static struct glink_channel ch;
#define to_glink_channel(p) ((struct glink_channel *)((char *)(p)-offsetof(struct glink_channel,ept)))
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!pthread_mutex_lock(p));}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);assert(!pthread_mutex_unlock(p));}while(0)
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define NATIVE_DTR_SIG (1U<<31)
#define NATIVE_CTS_SIG (1U<<30)
#define NATIVE_CD_SIG (1U<<29)
#define NATIVE_RI_SIG (1U<<28)
#define CH_INFO(...) ((void)0)
#define dev_err(...) ((void)0)
static void qcom_glink_rx_advance(struct qcom_glink *g,unsigned n){(void)g;assert(n==8);}
static struct glink_channel *qcom_glink_channel_ref_get(struct qcom_glink *g,bool remote,unsigned cid){(void)g;(void)remote;return cid==1?&ch:NULL;}
static void qcom_glink_channel_ref_put(struct glink_channel *c){assert(c==&ch);}
/* INSERT_SIGNALS */
static atomic_int entered,release_cb,detach_started,detach_done,calls;
static int callback(struct rpmsg_device *d,void *p,u32 old,u32 new){(void)d;(void)p;assert(old==0&&new==(TIOCM_DSR|TIOCM_CTS));atomic_fetch_add(&calls,1);atomic_store(&entered,1);while(!atomic_load(&release_cb))sched_yield();return 0;}
static void *dispatch(void *p){(void)p;struct qcom_glink g={0};assert(!qcom_glink_handle_signals(&g,1,NATIVE_DTR_SIG|NATIVE_CTS_SIG));return NULL;}
static void *detach(void *p){(void)p;atomic_store(&detach_started,1);assert(!qcom_glink_register_signals_cb(&ch.ept,NULL));atomic_store(&detach_done,1);return NULL;}
int main(void){assert(!pthread_mutex_init(&ch.recv_lock,NULL));assert(qcom_glink_register_signals_cb(NULL,callback)==-EINVAL);assert(!qcom_glink_register_signals_cb(&ch.ept,callback));pthread_t a,b;assert(!pthread_create(&a,NULL,dispatch,NULL));while(!atomic_load(&entered))sched_yield();assert(!pthread_create(&b,NULL,detach,NULL));while(!atomic_load(&detach_started))sched_yield();usleep(10000);assert(!atomic_load(&detach_done));atomic_store(&release_cb,1);assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));assert(atomic_load(&detach_done));struct qcom_glink g={0};for(int i=0;i<10000;i++)assert(!qcom_glink_handle_signals(&g,1,NATIVE_CD_SIG));assert(atomic_load(&calls)==1);assert(ch.remote_signals==TIOCM_CD);assert(qcom_glink_handle_signals(&g,2,0)==-EINVAL);assert(!pthread_mutex_destroy(&ch.recv_lock));puts("synchronous-detach PASS");}
