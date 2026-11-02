/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ADSPSLEEPMON_LPM_WAIT_TIME 5
#define ADSPSLEEPMON_LPI_WAIT_TIME 15
#define ADSPSLEEPMON_LPM_WAIT_TIME_OVERALL 60
#define ADSPSLEEPMON_MIN_REQUIRED_RESUMES 5
#define ADSPSLEEPMON_DEVICE_NAME_LOCAL "msm_adsp_sleepmon"
#define THIS_MODULE NULL
#define IS_ERR(p) ((uintptr_t)(p)>UINTPTR_MAX-4095)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define MKDEV(a,b) (a)
#define MAJOR(a) (a)
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
struct device { void *of_node; };
struct platform_device { struct device dev; };
struct cdev { unsigned int dev; void *owner; };
struct notifier_block { int (*notifier_call)(void); };
struct mutex { int unused; };
struct completion { bool initialized; };
struct timer_list { bool stopped; };
struct adspsleepmon {
 struct mutex lock, rpmsg_dev_lock;struct completion sem;int audio_clients;
 unsigned int lpm_wait_time,lpi_wait_time,lpm_wait_time_overall,min_required_resumes,adsp_rproc_phandle;
 bool b_config_panic_lpm,b_config_panic_lpi,b_config_adsp_panic_lpm,b_config_adsp_panic_lpi,b_config_adsp_panic_lpm_overall;
 bool b_config_adsp_ssr_enable,b_panic_lpm,b_panic_lpi,b_rpmsg_register,suspend_init_done;
 void *worker_task,*class;struct device *dev;struct cdev cdev;unsigned int devno;
 void *debugfs_dir,*debugfs_panic_file,*debugfs_read_panic_state,*debugfs_master_stats;
 void *debugfs_adsp_panic_file,*debugfs_read_adsp_panic_state;struct notifier_block nb;void *notifier;
};
static struct adspsleepmon g_adspsleepmon;
static struct timer_list adspsleep_timer;
static int adspsleepmon_worker, adspsleepmon_timer_cb, fops, panic_state_fops,read_panic_state_fops,master_stats_fops;
static int adsp_panic_state_fops,read_adsp_panic_state_fops,sleepmon_rpmsg_client,adsp_sleepmon_pm_nb;
static int adsp_sleepmon_ssr_notifier(void){return 0;}
static int fault,debugfs_failure,threads,regions,cdevs,classes,devices,channels,ssr,pm;
static struct device created_dev;
static void mutex_init(struct mutex *m){(void)m;}
static void init_completion(struct completion *c){c->initialized=true;}
static void complete_all(struct completion *c){assert(c->initialized);}
#define INIT_HLIST_HEAD(p) (*(p)=1)
static void timer_setup(struct timer_list *t,int cb,int flags){(void)cb;(void)flags;t->stopped=false;}
static void timer_shutdown_sync(struct timer_list *t){t->stopped=true;}
static void *kthread_run(int cb,void *data,const char *name){(void)cb;(void)data;(void)name;assert(g_adspsleepmon.sem.initialized && g_adspsleepmon.audio_clients);if(fault==1)return ERR_PTR(-EAGAIN);threads++;return &threads;}
static void kthread_stop(void *t){assert(t==&threads);threads--;}
static int alloc_chrdev_region(unsigned int *dev,int base,int count,const char *name){(void)base;(void)count;(void)name;if(fault==2)return -ENOMEM;*dev=1;regions++;return 0;}
static void unregister_chrdev_region(unsigned int dev,int count){(void)count;assert(dev==1 && regions==1);regions--;}
static void cdev_init(struct cdev *c,int *f){(void)c;(void)f;}
static int cdev_add(struct cdev *c,unsigned int dev,int count){(void)count;if(fault==3)return -EIO;c->dev=dev;cdevs++;return 0;}
static void cdev_del(struct cdev *c){assert(c->dev==1 && cdevs==1);cdevs--;}
static void *class_create(const char *name){(void)name;if(fault==4)return ERR_PTR(-ENOMEM);classes++;return &classes;}
static void class_destroy(void *c){assert(c==&classes && classes==1);classes--;}
static struct device *device_create(void *c,void *parent,unsigned int dev,void *data,const char *name){(void)parent;(void)data;(void)name;assert(c==&classes && dev==1);if(fault==5)return ERR_PTR(-ENOMEM);devices++;return &created_dev;}
static void device_destroy(void *c,unsigned int dev){assert(c==&classes && dev==1 && devices==1);devices--;}
static void *debugfs_create_dir(const char *name,void *p){(void)name;(void)p;return debugfs_failure?ERR_PTR(-ENODEV):&devices;}
static void *debugfs_create_file(const char *name,int mode,void *parent,void *data,int *ops){(void)name;(void)mode;(void)parent;(void)data;(void)ops;return ERR_PTR(-ENODEV);}
static void debugfs_remove_recursive(void *p){(void)p;}
static bool of_property_read_bool(void *node,const char *name){(void)node;(void)name;return false;}
static int of_property_read_u32(void *node,const char *name,unsigned int *p){(void)node;(void)name;(void)p;return -EINVAL;}
static int register_rpmsg_driver(int *c){(void)c;if(fault==6)return -EIO;channels++;pm++;g_adspsleepmon.suspend_init_done=true;return 0;}
static void unregister_rpmsg_driver(int *c){(void)c;assert(channels==1);channels--;}
static void *qcom_register_ssr_notifier(const char *name,struct notifier_block *n){(void)name;(void)n;if(fault==7)return ERR_PTR(-EAGAIN);ssr++;return &ssr;}
static void qcom_unregister_ssr_notifier(void *p,struct notifier_block *n){(void)n;assert(p==&ssr && ssr==1);ssr--;}
static void unregister_pm_notifier(int *n){(void)n;assert(pm==1);pm--;}
/* ACTUAL_FUNCTIONS */
int main(void){
 struct platform_device p={};int expected[]={0,-EAGAIN,-ENOMEM,-EIO,-ENOMEM,-ENOMEM,-EIO,-EAGAIN};
 for(fault=1;fault<=7;fault++){
  memset(&g_adspsleepmon,0,sizeof(g_adspsleepmon));assert(adspsleepmon_driver_probe(&p)==expected[fault]);
  assert(!threads && !regions && !cdevs && !classes && !devices && !channels && !ssr && !pm);
 }
 fault=0;
 for(debugfs_failure=0;debugfs_failure<=1;debugfs_failure++){
  memset(&g_adspsleepmon,0,sizeof(g_adspsleepmon));assert(adspsleepmon_driver_probe(&p)==0);
  assert(threads==1 && regions==1 && channels==1 && ssr==1 && pm==1);
  adspsleepmon_driver_remove(&p);assert(adspsleep_timer.stopped);
  assert(!threads && !regions && !cdevs && !classes && !devices && !channels && !ssr && !pm);
 }
 puts("{\"cases\":9,\"probe_failure_cleanup_checked\":true,\"timer_worker_shutdown_checked\":true}");
}
