/* Full adapted packet, ioctl, mapping and idle/open functions are inserted below. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#define __user
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef unsigned int vm_fault_t;
#define U64_MAX UINT64_MAX
#define PAGE_SIZE 4096UL
#define PAGE_ALIGN(v) (((v) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGNED(v) (!((v) & (PAGE_SIZE - 1)))
#define offset_in_page(v) ((unsigned long)(v) & (PAGE_SIZE - 1))
#define GFP_KERNEL 0
#define GFP_ATOMIC 1
#define ERESTARTSYS 512
#define ENOIOCTLCMD 515
#define RPMSG_DEFER 1
#define RPMSG_ADDR_ANY UINT32_MAX
#define GLINK_PKT_INFO(...) ((void)0)
#define GLINK_PKT_ERR(...) ((void)0)
#define container_of(p,t,m) ((t *)((char *)(p) - offsetof(t,m)))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define READ_ONCE(x) (x)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define WARN_ON_ONCE(x) assert(!(x))
#define VM_WRITE 1UL
#define VM_EXEC 2UL
#define VM_MAYWRITE 4UL
#define VM_MAYEXEC 8UL
#define VM_IO 16UL
#define VM_PFNMAP 32UL
#define VM_DONTCOPY 64UL
#define VM_DONTEXPAND 128UL
#define VM_DONTDUMP 256UL
#define VM_FAULT_NOPAGE 0U
#define VM_FAULT_OOM 1U
#define VM_FAULT_SIGBUS 2U
#define VM_FAULT_ERROR 3U
struct mutex { int held, rank; };
typedef struct mutex spinlock_t;
static int mm_held, gp_held, spin_held, cases;
#define CHECK(x) do { assert(x); cases++; } while (0)
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=1; if(m->rank==2)gp_held++; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=0; if(m->rank==2)gp_held--; }
static int lock_error;
static int mutex_lock_interruptible(struct mutex *m) { if(lock_error){int e=lock_error;lock_error=0;return e;}mutex_lock(m);return 0; }
static void spin_lock_irq(spinlock_t *m) { mutex_lock(m);spin_held++; }
static void spin_unlock_irq(spinlock_t *m) { spin_held--;mutex_unlock(m); }
#define spin_lock_irqsave(m,f) do{(f)=0;spin_lock_irq(m);}while(0)
#define spin_unlock_irqrestore(m,f) do{(void)(f);spin_unlock_irq(m);}while(0)
struct completion { int done; };
static int wait_result=1;
static void (*wait_hook)(void);
static void complete_all(struct completion *c){c->done=1;}
static void reinit_completion(struct completion *c){c->done=0;}
static bool completion_done(struct completion *c){return c->done;}
static int wait_for_completion_interruptible_timeout(struct completion *c,unsigned long t){(void)c;(void)t;return wait_result;}
static int wait_for_completion_interruptible(struct completion *c){if(wait_hook)wait_hook();assert(c->done);return 0;}
static unsigned long msecs_to_jiffies(unsigned int ms){return ms;}
struct list_head { struct list_head *next,*prev; };
struct work_struct { void (*fn)(struct work_struct *); int pending; };
static bool schedule_work(struct work_struct *w){if(w->pending)return false;w->pending=1;return true;}
static bool cancel_work_sync(struct work_struct *w){bool p=w->pending;w->pending=0;return p;}
static void drain_work(struct work_struct *w){if(w->pending){w->pending=0;w->fn(w);}}
struct device { void *data; int refs; };
struct cdev {};
struct inode { struct cdev *i_cdev; };
struct file { void *private_data; int f_flags; };
struct rpmsg_endpoint {};
struct rpmsg_device { struct device dev; struct rpmsg_endpoint *ept; };
struct rpmsg_driver {};
typedef struct { int value; } refcount_t;
static int refcount_read(refcount_t *r){return r->value;}
static void refcount_inc(refcount_t *r){r->value++;}
static void refcount_dec(refcount_t *r){assert(r->value>1);r->value--;}
static void get_device(struct device *d){assert(d->refs>0);d->refs++;}
static void put_device(struct device *d){assert(d->refs>1);d->refs--;}
static void *dev_get_drvdata(struct device *d){return d->data;}
static void dev_set_drvdata(struct device *d,void *data){d->data=data;}
typedef struct { int wakes; } wait_queue_head_t;
static void wake_up_interruptible(wait_queue_head_t *q){q->wakes++;}
static int wait_event_result=-ERESTARTSYS;
static void (*event_hook)(void);
#define wait_event_interruptible_lock_irq(q,cond,lock) ({ \
 (void)(q);spin_unlock_irq(&(lock));if(event_hook)event_hook();spin_lock_irq(&(lock)); \
 (cond)?0:wait_event_result; })
struct sk_buff { struct sk_buff *next,*prev;unsigned char *head,*data;unsigned int len;_Alignas(8) unsigned char cb[48]; };
struct sk_buff_head { struct sk_buff *next,*prev; };
static int skb_live,alloc_fail;
static bool fail_alloc(void){if(alloc_fail>0&&!--alloc_fail)return true;return false;}
static struct sk_buff *alloc_skb(size_t n,int flags){(void)flags;if(fail_alloc())return NULL;struct sk_buff *s=calloc(1,sizeof(*s));assert(s);s->data=s->head=malloc(n?n:1);assert(s->head);memset(s->cb,0xa5,sizeof(s->cb));skb_live++;return s;}
static void skb_put_data(struct sk_buff *s,void *p,size_t n){if(n)memcpy(s->data,p,n);s->len+=n;}
static void kfree_skb(struct sk_buff *s){assert(s->head==s->data);free(s->head);free(s);skb_live--;}
static void skb_queue_head_init(struct sk_buff_head *q){q->next=q->prev=(void *)q;}
static bool skb_queue_empty(struct sk_buff_head *q){return q->next==(void *)q;}
static struct sk_buff *skb_peek(struct sk_buff_head *q){return skb_queue_empty(q)?NULL:q->next;}
static void skb_unlink(struct sk_buff *s,struct sk_buff_head *q){(void)q;s->next->prev=s->prev;s->prev->next=s->next;s->next=s->prev=NULL;}
static void skb_queue_tail(struct sk_buff_head *q,struct sk_buff *s){s->next=(void *)q;s->prev=q->prev;q->prev->next=s;q->prev=s;}
static void skb_queue_head(struct sk_buff_head *q,struct sk_buff *s){s->prev=(void *)q;s->next=q->next;q->next->prev=s;q->next=s;}
static struct sk_buff *skb_dequeue(struct sk_buff_head *q){struct sk_buff *s=skb_peek(q);if(s)skb_unlink(s,q);return s;}
#define skb_queue_walk(q,s) for((s)=(q)->next;(s)!=(void *)(q);(s)=(s)->next)
#define skb_queue_walk_safe(q,s,n) for((s)=(q)->next,(n)=(s)->next;(s)!=(void *)(q);(s)=(n),(n)=(s)->next)
struct mm_struct;
struct vm_area_struct;
struct vm_operations_struct { void (*close)(struct vm_area_struct *);int (*mremap)(struct vm_area_struct *); };
struct vm_area_struct { unsigned long vm_start,vm_end,vm_flags;struct mm_struct *vm_mm;void *vm_private_data;const struct vm_operations_struct *vm_ops; };
struct pte_stub { unsigned long address;unsigned char *page;struct vm_area_struct *vma; };
struct mm_struct { struct vm_area_struct *vmas[16];int nr,refs,users;struct pte_stub ptes[64];int count; };
struct { struct mm_struct *mm; } task;
#define current (&task)
struct page {};
static struct mm_struct *mms[4];
static int nr_mms,map_fail,map_calls,boundary_live;
static void mmap_read_lock(struct mm_struct *m){assert(!gp_held);assert(m->refs>0);assert(!mm_held);mm_held=1;}
static void mmap_read_unlock(struct mm_struct *m){(void)m;assert(mm_held==1);mm_held=0;}
static void mmap_write_lock(struct mm_struct *m){assert(!gp_held);assert(m->refs>0);assert(!mm_held);mm_held=2;}
static void mmap_write_unlock(struct mm_struct *m){(void)m;assert(mm_held==2);mm_held=0;}
static void mmgrab(struct mm_struct *m){m->refs++;}
static void mmdrop(struct mm_struct *m){assert(m->refs>1);m->refs--;}
static struct vm_area_struct *find_vma(struct mm_struct *m,unsigned long a){assert(mm_held);for(int i=0;i<m->nr;i++)if(m->vmas[i]->vm_end>a)return m->vmas[i];return NULL;}
static struct vm_area_struct *vma_lookup(struct mm_struct *m,unsigned long a){struct vm_area_struct *v=find_vma(m,a);return v&&v->vm_start<=a?v:NULL;}
static void vm_flags_clear(struct vm_area_struct *v,unsigned long f){v->vm_flags&=~f;}
static void vm_flags_set(struct vm_area_struct *v,unsigned long f){v->vm_flags|=f;}
static void zap_page_range_single(struct vm_area_struct *v,unsigned long a,unsigned long n,void *d){(void)d;assert(mm_held);assert(a>=v->vm_start&&a+n<=v->vm_end);struct mm_struct *m=v->vm_mm;for(int i=0;i<m->count;)if(m->ptes[i].address>=a&&m->ptes[i].address<a+n)m->ptes[i]=m->ptes[--m->count];else i++;}
static vm_fault_t vmf_insert_pfn(struct vm_area_struct *v,unsigned long a,unsigned long pfn){assert(mm_held);assert((v->vm_flags&VM_PFNMAP)&&!(v->vm_flags&VM_MAYWRITE));map_calls++;if(map_fail&&map_calls==map_fail)return VM_FAULT_OOM;struct mm_struct *m=v->vm_mm;assert(m->count<64);m->ptes[m->count++]=(struct pte_stub){a,(void *)(pfn*PAGE_SIZE),v};return VM_FAULT_NOPAGE;}
static bool is_vmalloc_addr(void *p){(void)p;return false;}
static struct page *host_virt_to_page(void *p){return (void *)((unsigned long)p&~(PAGE_SIZE-1));}
#define virt_to_page(p) host_virt_to_page((void *)(p))
static struct page *vmalloc_to_page(void *p){return virt_to_page(p);}
static unsigned long page_to_pfn(struct page *p){return (unsigned long)p/PAGE_SIZE;}
static unsigned long get_zeroed_page(int flags){(void)flags;if(fail_alloc())return 0;void *p;assert(!posix_memalign(&p,PAGE_SIZE,PAGE_SIZE));memset(p,0,PAGE_SIZE);boundary_live++;return (unsigned long)p;}
static void free_page(unsigned long p){for(int n=0;n<nr_mms;n++)for(int i=0;i<mms[n]->count;i++)assert(mms[n]->ptes[i].page!=(void *)p);free((void *)p);boundary_live--;}
static int acks;
static int qcom_glink_rx_done(struct rpmsg_endpoint *e,void *data){assert(e);assert(!spin_held);for(int n=0;n<nr_mms;n++)for(int i=0;i<mms[n]->count;i++)assert(mms[n]->ptes[i].page!=(void *)((unsigned long)data&~(PAGE_SIZE-1)));acks++;return 0;}
static int reg_error,registers,unregisters;
static int register_rpmsg_driver(struct rpmsg_driver *d){(void)d;registers++;return reg_error;}
static void unregister_rpmsg_driver(struct rpmsg_driver *d){(void)d;assert(!mm_held);unregisters++;}
static int copy_in_fault,copy_out_fault;
static void (*copy_hook)(void);
static int copy_to_user(void *d,const void *s,size_t n){assert(!gp_held&&!mm_held);if(copy_hook)copy_hook();if(copy_out_fault)return 1;if(n)memcpy(d,s,n);return 0;}
static int copy_from_user(void *d,const void *s,size_t n){assert(!gp_held&&!mm_held);if(copy_in_fault)return 1;memcpy(d,s,n);return 0;}
static int send_calls,trysend_calls,send_error;
static size_t sent_length;
static unsigned char sent_bytes[32];
static void *vmemdup_user(const void *p,size_t n){if(fail_alloc())return ERR_PTR(-ENOMEM);void *d=malloc(n?n:1);assert(d);if(n&&copy_from_user(d,p,n)){free(d);return ERR_PTR(-EFAULT);}return d;}
#define kvfree free
static int rpmsg_send(struct rpmsg_endpoint *e,const void *p,int n){assert(e&&gp_held&&!spin_held&&n>=0);send_calls++;sent_length=n;assert(n<=32);if(n)memcpy(sent_bytes,p,n);return send_error;}
static int rpmsg_trysend(struct rpmsg_endpoint *e,const void *p,int n){trysend_calls++;return rpmsg_send(e,p,n);}
#define get_user(v,p) (copy_from_user(&(v),(p),sizeof(v))?-EFAULT:0)
#define put_user(v,p) ({int x=(v);copy_to_user((p),&x,sizeof(x))?-EFAULT:0;})
static void *kmemdup(const void *p,size_t n,int f){(void)f;if(fail_alloc())return NULL;void *d=malloc(n?n:1);assert(d);if(n)memcpy(d,p,n);return d;}
#define kfree free
static int qcom_glink_set_signals(struct rpmsg_endpoint *e,u32 set,u32 clear){(void)e;(void)set;(void)clear;return 0;}
static int qcom_glink_get_signals(struct rpmsg_endpoint *e){(void)e;return TIOCM_RTS;}
/* INSERT_DRIVER */
struct fixture { struct glink_pkt_device g;struct rpmsg_device rp;struct rpmsg_endpoint ept;struct mm_struct mm,other;struct vm_area_struct v,split;struct file f;struct inode inode;unsigned char *allocation,*data;size_t length,offset; };
static struct fixture *active;
static void init(struct fixture *x,size_t offset,size_t len){memset(x,0,sizeof(*x));active=x;x->g.lock.rank=2;x->g.dev.refs=2;x->g.refcount.value=2;x->g.rpdev=&x->rp;x->rp.ept=&x->ept;x->rp.dev.data=&x->g;x->g.rx_done=true;x->g.edge="adsp";x->g.ch_name="bt_cp_ctrl";x->g.open_tout=120;x->g.ch_open.done=1;x->g.drv_idle.done=1;x->g.drv_registered=true;x->g.idle_work.fn=glink_pkt_idle_work;skb_queue_head_init(&x->g.queue);skb_queue_head_init(&x->g.pending);x->f.private_data=&x->g;x->inode.i_cdev=&x->g.cdev;x->mm.refs=x->other.refs=1;x->mm.users=x->other.users=1;x->v=(struct vm_area_struct){.vm_start=0x100000,.vm_end=0x110000,.vm_mm=&x->mm};x->mm.vmas[0]=&x->v;x->mm.nr=1;x->other.nr=0;mms[0]=&x->mm;mms[1]=&x->other;nr_mms=2;current->mm=&x->mm;CHECK(!glink_pkt_mmap(&x->f,&x->v));x->offset=offset;x->length=len;assert(!posix_memalign((void **)&x->allocation,PAGE_SIZE,offset+len+1));x->data=x->allocation+offset;for(size_t i=0;i<len;i++)x->data[i]=(i*31+7)&255;alloc_fail=map_fail=map_calls=copy_in_fault=copy_out_fault=reg_error=lock_error=0;wait_result=1;copy_hook=event_hook=wait_hook=NULL;acks=registers=unregisters=0;}
static void enqueue(struct fixture *x){CHECK(glink_pkt_rpdev_no_copy_cb(&x->rp,x->data,x->length,NULL,0)==RPMSG_DEFER);}
static void cleanup(struct fixture *x){current->mm=&x->mm;glink_pkt_revoke_pending(&x->g);mutex_lock(&x->g.lock);glink_pkt_clear_queues(&x->g);mutex_unlock(&x->g.lock);glink_pkt_cancel_idle(&x->g);CHECK(!skb_live&&!boundary_live&&!x->mm.count&&x->mm.refs==1);CHECK(!gp_held&&!mm_held&&!spin_held);free(x->allocation);}
static unsigned char mapped_byte(struct fixture *x,unsigned long a){for(int i=0;i<x->mm.count;i++)if(x->mm.ptes[i].address==(a&~(PAGE_SIZE-1)))return x->mm.ptes[i].page[a&(PAGE_SIZE-1)];assert(false);return 0;}
static void extent_cases(void){struct fixture x;for(size_t off=0;off<PAGE_SIZE;off++){size_t lengths[]={1,2,4095,4096,4097,8192,8193};for(unsigned n=0;n<sizeof(lengths)/sizeof(*lengths);n++){init(&x,off,lengths[n]);enqueue(&x);struct glink_pkt_zerocopy_receive z={.address=x.v.vm_start};u64 cookie;CHECK(!glink_pkt_zerocopy_receive(&x.g,&z,&cookie));CHECK(z.length==x.length&&z.offset==off&&cookie);CHECK(acks==0&&skb_queue_empty(&x.g.queue));for(size_t i=0;i<PAGE_ALIGN(off+x.length);i++)CHECK(mapped_byte(&x,z.address+i)==(i>=off&&i<off+x.length?x.data[i-off]:0));/* A complete interior page is the original firmware page. */for(int p=0;p<x.mm.count;p++){size_t cursor=x.mm.ptes[p].address-z.address;if(cursor>=off&&cursor+PAGE_SIZE<=off+x.length)CHECK(x.mm.ptes[p].page==x.allocation+cursor);}CHECK(!glink_pkt_zerocopy_done(&x.g,&z));CHECK(acks==1&&x.mm.count==0);cleanup(&x);}}}
static void mapping_faults(void){struct fixture x;for(int map=1;map<=4;map++){init(&x,1,3*PAGE_SIZE);enqueue(&x);map_fail=map;struct glink_pkt_zerocopy_receive z={.address=x.v.vm_start};u64 cookie=0;CHECK(glink_pkt_zerocopy_receive(&x.g,&z,&cookie)==-ENOMEM);CHECK(!cookie&&!acks&&!x.mm.count&&!boundary_live&&skb_peek(&x.g.queue));cleanup(&x);}for(int alloc=1;alloc<=2;alloc++){init(&x,1,3*PAGE_SIZE);enqueue(&x);alloc_fail=alloc;struct glink_pkt_zerocopy_receive z={.address=x.v.vm_start};u64 cookie;CHECK(glink_pkt_zerocopy_receive(&x.g,&z,&cookie)==-ENOMEM);CHECK(!acks&&!x.mm.count&&!boundary_live&&skb_peek(&x.g.queue));cleanup(&x);}}
static void ownership_cases(void){struct fixture x;init(&x,1,3*PAGE_SIZE);enqueue(&x);struct glink_pkt_zerocopy_receive z={.address=x.v.vm_start};u64 cookie;CHECK(!glink_pkt_zerocopy_receive(&x.g,&z,&cookie));current->mm=&x.other;CHECK(glink_pkt_zerocopy_done(&x.g,&z)==-EINVAL&&acks==0);current->mm=&x.mm;enqueue(&x);u64 c2;struct glink_pkt_zerocopy_receive overlap={.address=z.address+PAGE_SIZE};CHECK(glink_pkt_zerocopy_receive(&x.g,&overlap,&c2)==-EBUSY);glink_pkt_zerocopy_rollback(&x.g,&z,cookie+1);CHECK(!acks&&x.mm.count==4);CHECK(!glink_pkt_zerocopy_done(&x.g,&z));CHECK(!glink_pkt_zerocopy_receive(&x.g,&z,&c2)&&c2!=cookie);glink_pkt_zerocopy_rollback(&x.g,&z,cookie);CHECK(x.mm.count==4&&acks==1);CHECK(!glink_pkt_zerocopy_done(&x.g,&z));cleanup(&x);
init(&x,1,10);enqueue(&x);x.v.vm_private_data=NULL;CHECK(glink_pkt_zerocopy_receive(&x.g,&z,&cookie)==-EINVAL);x.v.vm_private_data=&x.g;z.address++;CHECK(glink_pkt_zerocopy_receive(&x.g,&z,&cookie)==-EINVAL);cleanup(&x);
init(&x,1,2*PAGE_SIZE);enqueue(&x);z.address=x.v.vm_start;x.v.vm_end=x.v.vm_start+PAGE_SIZE;CHECK(glink_pkt_zerocopy_receive(&x.g,&z,&cookie)==-ENOSPC);cleanup(&x);}
static void reset_and_split(void){struct fixture x;init(&x,1,3*PAGE_SIZE);enqueue(&x);struct glink_pkt_zerocopy_receive z={.address=x.v.vm_start};u64 cookie;CHECK(!glink_pkt_zerocopy_receive(&x.g,&z,&cookie));x.mm.users=0;glink_pkt_revoke_pending(&x.g);CHECK(acks==1&&!x.mm.count);cleanup(&x);
init(&x,1,3*PAGE_SIZE);enqueue(&x);z.address=x.v.vm_start;CHECK(!glink_pkt_zerocopy_receive(&x.g,&z,&cookie));struct vm_area_struct removed=x.v;removed.vm_start+=PAGE_SIZE;removed.vm_end=removed.vm_start+PAGE_SIZE;mmap_write_lock(&x.mm);zap_page_range_single(&x.v,removed.vm_start,PAGE_SIZE,NULL);x.split=x.v;x.split.vm_start=removed.vm_end;x.v.vm_end=removed.vm_start;x.mm.vmas[1]=&x.split;x.mm.nr=2;glink_pkt_vma_close(&removed);mmap_write_unlock(&x.mm);CHECK(acks==1&&!x.mm.count);CHECK(glink_pkt_vma_mremap(&x.v)==-EINVAL);cleanup(&x);}
static void ioctl_faults(void){struct fixture x;init(&x,1,3*PAGE_SIZE);enqueue(&x);struct glink_pkt_zerocopy_receive z={.address=x.v.vm_start};copy_out_fault=1;CHECK(glink_pkt_ioctl(&x.f,GLINK_PKT_IOCTL_ZC_RECV,(unsigned long)&z)==-EFAULT);CHECK(!acks&&!x.mm.count&&!boundary_live&&skb_peek(&x.g.queue));copy_out_fault=0;CHECK(!glink_pkt_ioctl(&x.f,GLINK_PKT_IOCTL_ZC_RECV,(unsigned long)&z));CHECK(!glink_pkt_ioctl(&x.f,GLINK_PKT_IOCTL_ZC_DONE,(unsigned long)&z));CHECK(acks==1);copy_in_fault=1;CHECK(glink_pkt_ioctl(&x.f,GLINK_PKT_IOCTL_ZC_RECV,(unsigned long)&z)==-EFAULT);copy_in_fault=0;int signals;CHECK(!glink_pkt_ioctl(&x.f,TIOCMGET,(unsigned long)&signals)&&signals==TIOCM_RTS);signals=TIOCM_DTR;CHECK(!glink_pkt_ioctl(&x.f,TIOCMSET,(unsigned long)&signals));x.g.disconnecting=true;CHECK(glink_pkt_ioctl(&x.f,GLINK_PKT_IOCTL_QUEUE_RX_INTENT,0)==-ENETRESET);CHECK(glink_pkt_ioctl(&x.f,GLINK_PKT_IOCTL_ZC_RECV,(unsigned long)&z)==-ENETRESET);x.g.disconnecting=false;cleanup(&x);}
static void reset_copy_hook(void){mutex_lock(&active->g.lock);glink_pkt_clear_queues(&active->g);mutex_unlock(&active->g.lock);copy_hook=NULL;}
static void enqueue_event_hook(void){enqueue(active);event_hook=NULL;}
static void read_cases(void){struct fixture x;unsigned char out[32];init(&x,3,8);x.f.f_flags=O_NONBLOCK;CHECK(glink_pkt_read(&x.f,(void *)out,1,NULL)==-EAGAIN);enqueue(&x);x.g.fragmented_read=true;copy_out_fault=1;CHECK(glink_pkt_read(&x.f,(void *)out,3,NULL)==-EFAULT&&acks==0&&x.g.rdata_len==8);copy_out_fault=0;CHECK(glink_pkt_read(&x.f,(void *)out,3,NULL)==3&&!memcmp(out,x.data,3));CHECK(glink_pkt_read(&x.f,(void *)out,20,NULL)==5&&!memcmp(out,x.data+3,5)&&acks==1);enqueue(&x);alloc_fail=1;CHECK(glink_pkt_read(&x.f,(void *)out,3,NULL)==-ENOMEM&&acks==1);copy_hook=reset_copy_hook;CHECK(glink_pkt_read(&x.f,(void *)out,3,NULL)==-ENETRESET&&acks==2);x.f.f_flags=0;event_hook=enqueue_event_hook;CHECK(glink_pkt_read(&x.f,(void *)out,20,NULL)==8&&acks==3);CHECK(!glink_pkt_read(&x.f,(void *)out,0,NULL));cleanup(&x);}
static void open_idle_cases(void){struct fixture x;init(&x,0,4);x.g.enable_ch_close=true;struct file fresh={0};x.g.refcount.value=1;x.g.dev.refs=1;x.g.drv_registered=false;reg_error=-ENOMEM;CHECK(glink_pkt_open(&x.inode,&fresh)==-ENOMEM);CHECK(x.g.refcount.value==1&&x.g.idle_work.pending);drain_work(&x.g.idle_work);reg_error=0;wait_result=-ERESTARTSYS;CHECK(glink_pkt_open(&x.inode,&fresh)==-ERESTARTSYS);drain_work(&x.g.idle_work);wait_result=0;CHECK(glink_pkt_open(&x.inode,&fresh)==-ETIMEDOUT);drain_work(&x.g.idle_work);wait_result=1;CHECK(!glink_pkt_open(&x.inode,&fresh));mmap_write_lock(&x.mm);int prior=unregisters;CHECK(!glink_pkt_release(&x.inode,&fresh));CHECK(unregisters==prior&&x.g.idle_work.pending);mmap_write_unlock(&x.mm);/* A new admitted open invalidates the stale last-close check. */CHECK(!glink_pkt_open(&x.inode,&fresh));drain_work(&x.g.idle_work);CHECK(unregisters==prior&&x.g.drv_registered);CHECK(!glink_pkt_release(&x.inode,&fresh));drain_work(&x.g.idle_work);CHECK(unregisters==prior+1&&!x.g.drv_registered);x.g.removing=true;CHECK(glink_pkt_open(&x.inode,&fresh)==-ENODEV);x.g.dev.refs=2;cleanup(&x);}
static void write_cases(void){struct fixture x;init(&x,0,4);send_calls=trysend_calls=send_error=0;CHECK(!glink_pkt_write(&x.f,NULL,0,NULL));CHECK(send_calls==1&&sent_length==0);CHECK(glink_pkt_write(&x.f,(void *)x.data,4,NULL)==4&&!memcmp(sent_bytes,x.data,4)&&send_calls==2);x.f.f_flags=O_NONBLOCK;send_error=-EAGAIN;CHECK(glink_pkt_write(&x.f,(void *)x.data,4,NULL)==-EAGAIN&&trysend_calls==1);CHECK(glink_pkt_write(&x.f,NULL,(size_t)INT_MAX+1,NULL)==-EMSGSIZE&&send_calls==3);alloc_fail=1;CHECK(glink_pkt_write(&x.f,(void *)x.data,4,NULL)==-ENOMEM&&send_calls==3);copy_in_fault=1;CHECK(glink_pkt_write(&x.f,(void *)x.data,4,NULL)==-EFAULT&&send_calls==3);copy_in_fault=0;lock_error=-ERESTARTSYS;CHECK(glink_pkt_write(&x.f,(void *)x.data,4,NULL)==-ERESTARTSYS&&send_calls==3);x.g.disconnecting=true;CHECK(glink_pkt_write(&x.f,NULL,0,NULL)==-ENETRESET&&send_calls==3);x.g.disconnecting=false;cleanup(&x);}
static void callback_cases(void){struct fixture x;init(&x,0,4);x.rp.dev.data=NULL;CHECK(glink_pkt_rpdev_cb(&x.rp,x.data,4,NULL,0)==-ENETRESET);x.rp.dev.data=&x.g;CHECK(glink_pkt_rpdev_cb(&x.rp,x.data,-1,NULL,0)==-EINVAL);CHECK(glink_pkt_rpdev_cb(&x.rp,NULL,4,NULL,0)==-EINVAL);alloc_fail=1;CHECK(glink_pkt_rpdev_cb(&x.rp,x.data,4,NULL,0)==-ENOMEM);x.g.disconnecting=true;CHECK(glink_pkt_rpdev_cb(&x.rp,x.data,4,NULL,0)==-ENETRESET&&!skb_live&&!acks);x.g.disconnecting=false;x.g.rx_done=false;CHECK(!glink_pkt_rpdev_cb(&x.rp,x.data,4,NULL,0));unsigned char out[8];CHECK(glink_pkt_read(&x.f,(void *)out,8,NULL)==4&&!memcmp(out,x.data,4)&&!acks);cleanup(&x);}
int main(void){extent_cases();mapping_faults();ownership_cases();reset_and_split();ioctl_faults();read_cases();open_idle_cases();write_cases();callback_cases();printf("%d\n",cases);}
