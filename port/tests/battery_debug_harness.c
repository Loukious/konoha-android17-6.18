#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
typedef int64_t s64;
#define GFP_KERNEL 0
#define SZ_4K 4096
#define PAGE_SIZE 4096
#define U32_MAX UINT32_MAX
#define READ_ONCE(x) (x)
#define CONFIG_DEBUG_FS 1
#define CONFIG_QTI_PMIC_GLINK_CLIENT_DEBUG 1
#define IS_ENABLED(x) (x)
#define __user
#define EPROBE_DEFER 517
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define EXPORT_SYMBOL(n)
#define EXPORT_SYMBOL_GPL(n)
#define DEFINE_SHOW_ATTRIBUTE(n) static int n##_fops
#define DEFINE_DEBUGFS_ATTRIBUTE(n,g,s,f) static int n
#define DEVICE_ATTR_RW(n) struct device_attribute dev_attr_##n
#define DEVICE_ATTR_RO(n) struct device_attribute dev_attr_##n
#define pr_debug(...) ((void)0)
#define pr_err(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
struct mutex { pthread_mutex_t lock; pthread_t owner; bool held; };
static void mutex_init(struct mutex *m) { pthread_mutexattr_t a; assert(!pthread_mutexattr_init(&a)); assert(!pthread_mutexattr_settype(&a,PTHREAD_MUTEX_ERRORCHECK)); assert(!pthread_mutex_init(&m->lock,&a)); pthread_mutexattr_destroy(&a);m->held=false; }
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->lock));m->owner=pthread_self();m->held=true; }
static void mutex_unlock(struct mutex *m) { assert(m->held&&pthread_equal(m->owner,pthread_self()));m->held=false;assert(!pthread_mutex_unlock(&m->lock)); }
typedef pthread_mutex_t spinlock_t;
#define spin_lock_init(p) assert(!pthread_mutex_init(p,NULL))
#define spin_lock_irqsave(p,f) do { (f)=0;assert(!pthread_mutex_lock(p)); } while(0)
#define spin_unlock_irqrestore(p,f) assert(!pthread_mutex_unlock(p))
struct completion { pthread_mutex_t lock;pthread_cond_t cond;bool done; };
static int wait_mode;
static void init_completion(struct completion *c) { assert(!pthread_mutex_init(&c->lock,NULL));assert(!pthread_cond_init(&c->cond,NULL));c->done=false; }
static void reinit_completion(struct completion *c) { pthread_mutex_lock(&c->lock);c->done=false;pthread_mutex_unlock(&c->lock); }
static void complete(struct completion *c) { pthread_mutex_lock(&c->lock);c->done=true;pthread_cond_broadcast(&c->cond);pthread_mutex_unlock(&c->lock); }
static unsigned wait_for_completion_timeout(struct completion *c,unsigned t) { pthread_mutex_lock(&c->lock);if(wait_mode){struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=5;while(!c->done){int r=pthread_cond_timedwait(&c->cond,&c->lock,&until);assert(!r);}}bool done=c->done;pthread_mutex_unlock(&c->lock);return done; }
#define msecs_to_jiffies(n) (n)
struct list_head {struct list_head *next,*prev;};
#define INIT_LIST_HEAD(p) ((p)->next=(p),(p)->prev=(p))
static void list_add_tail(struct list_head*n,struct list_head*h) {n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
static void list_del(struct list_head*n) {n->prev->next=n->next;n->next->prev=n->prev;}
#define list_for_each_entry(p,h,m) for(p=container_of((h)->next,__typeof__(*p),m);&p->m!=(h);p=container_of(p->m.next,__typeof__(*p),m))
#define list_for_each_entry_safe(p,t,h,m) for(p=container_of((h)->next,__typeof__(*p),m),t=container_of(p->m.next,__typeof__(*p),m);&p->m!=(h);p=t,t=container_of(t->m.next,__typeof__(*t),m))
struct device;
struct kobject {struct device*dev;};
struct attribute {const char*name;unsigned mode;};
struct device_attribute {struct attribute attr;};
struct file {int unused;};
struct bin_attribute {struct attribute attr;ssize_t(*read)(struct file*,struct kobject*,const struct bin_attribute*,char*,loff_t,size_t);ssize_t(*write)(struct file*,struct kobject*,const struct bin_attribute*,char*,loff_t,size_t);};
struct attribute_group {struct attribute **attrs;const struct bin_attribute **bin_attrs;};
struct device {void*data;struct kobject kobj;void*alloc[8];unsigned allocations;};
struct platform_device {struct device dev;};
static void*dev_get_drvdata(struct device*d){return d->data;}
static struct device*kobj_to_dev(struct kobject*k){return k->dev;}
static void platform_set_drvdata(struct platform_device*p,void*d){p->dev.data=d;p->dev.kobj.dev=&p->dev;}
static void*platform_get_drvdata(struct platform_device*p){return p->dev.data;}
static int alloc_calls,alloc_fail_at,live_alloc,sysfs_error,register_error,debug_calls,debug_fail_at,debug_null_at;
static void*kzalloc(size_t n,int flags){if(++alloc_calls==alloc_fail_at)return NULL;void*p=calloc(1,n?:1);assert(p);live_alloc++;return p;}
static void kfree(void*p){if(p){assert(live_alloc);live_alloc--;free(p);}}
static void*devm_kzalloc(struct device*d,size_t n,int flags){void*p=kzalloc(n,flags);if(p){assert(d->allocations<8);d->alloc[d->allocations++]=p;}return p;}
static void*devm_kcalloc(struct device*d,size_t n,size_t z,int flags){assert(n<=SIZE_MAX/z);return devm_kzalloc(d,n*z,flags);}
struct dentry {bool present,protected,isfile;struct dentry*parent;const char*name;};
static struct dentry entries[128];static unsigned nentries;static bool sysfs_present;
static struct dentry*debugfs_create_dir(const char*name,struct dentry*parent){debug_calls++;if(debug_calls==debug_fail_at)return ERR_PTR(-ENOMEM);if(debug_calls==debug_null_at)return NULL;assert(nentries<128);struct dentry*d=&entries[nentries++];*d=(struct dentry){.present=true,.parent=parent,.name=name};return d;}
static struct dentry*debugfs_create_file(const char*name,unsigned mode,struct dentry*parent,void*data,void*fops){struct dentry*d=debugfs_create_dir(name,parent);if(!IS_ERR_OR_NULL(d))d->protected=d->isfile=true;return d;}
static struct dentry*debugfs_create_file_unsafe(const char*name,unsigned mode,struct dentry*parent,void*data,void*fops){struct dentry*d=debugfs_create_dir(name,parent);if(!IS_ERR_OR_NULL(d))d->isfile=true;return d;}
static void debugfs_remove_recursive(struct dentry*d){if(IS_ERR_OR_NULL(d))return;for(unsigned i=0;i<nentries;i++){for(struct dentry*p=&entries[i];p;p=p->parent)if(p==d){entries[i].present=false;break;}}}
static int sysfs_create_group(struct kobject*k,const struct attribute_group*g){if(sysfs_error)return sysfs_error;sysfs_present=true;return 0;}
static void sysfs_remove_group(struct kobject*k,const struct attribute_group*g){sysfs_present=false;}
struct debugfs_blob_wrapper {void*data;size_t size;};
struct seq_file {void*private;char buf[8192];size_t len;};
static void assert_seq_locked(struct seq_file*);
static void seq_printf(struct seq_file*s,const char*f,...){assert_seq_locked(s);va_list a;va_start(a,f);int n=vsnprintf(s->buf+s->len,sizeof(s->buf)-s->len,f,a);va_end(a);assert(n>=0&&(unsigned)n<sizeof(s->buf)-s->len);s->len+=n;}
static void seq_puts(struct seq_file*s,const char*t){seq_printf(s,"%s",t);}
static int sysfs_emit(char*b,const char*f,...){va_list a;va_start(a,f);int n=vsnprintf(b,PAGE_SIZE,f,a);va_end(a);assert(n>=0&&n<PAGE_SIZE);return n;}
static ssize_t memory_read_from_buffer(void*dest,size_t count,loff_t*pos,const void*src,size_t size){if(*pos<0)return -EINVAL;if((uint64_t)*pos>=size)return 0;size_t n=count<size-*pos?count:size-*pos;memcpy(dest,(const char*)src+*pos,n);*pos+=n;return n;}
static int kstrtou32(const char*s,unsigned base,u32*out){char*end;errno=0;unsigned long long v=strtoull(s,&end,base);if(errno||end==s||v>UINT32_MAX)return -EINVAL;while(*end=='\n'||*end==' ')end++;if(*end)return -EINVAL;*out=v;return 0;}
static bool test_bit(unsigned n,const unsigned long*p){return (*p>>n)&1;}
#define for_each_set_bit(i,p,n) for((i)=0;(i)<(n);(i)++)if(test_bit(i,p))
struct pmic_glink_hdr {u32 owner,type,opcode;};
enum pmic_glink_state {PMIC_GLINK_STATE_DOWN,PMIC_GLINK_STATE_UP};
struct pmic_glink_client;
struct idr {struct pmic_glink_client*client;};
struct rw_semaphore {pthread_rwlock_t lock;};
#define down_read(p) assert(!pthread_rwlock_rdlock(&(p)->lock))
#define up_read(p) assert(!pthread_rwlock_unlock(&(p)->lock))
typedef struct {atomic_int value;} atomic_t;
#define atomic_read(p) atomic_load(&(p)->value)
struct pmic_glink_dev {int client_srcu;struct list_head client_dev_list;struct list_head reply_fences;struct mutex client_lock;struct rw_semaphore rpdev_sem;void*rpdev;bool removing;atomic_t state;struct idr client_idr;};
struct pmic_glink_client {struct pmic_glink_dev*pgdev;u32 id;void*priv;int(*msg_cb)(void*,void*,size_t);void(*state_cb)(void*,enum pmic_glink_state);struct list_head node;char*name;};
struct pmic_glink_client_data {u32 id;const char*name;void*priv;int(*msg_cb)(void*,void*,size_t);void(*state_cb)(void*,enum pmic_glink_state);};
static void*idr_find(struct idr*i,u32 owner){return i->client&&i->client->id==owner?i->client:NULL;}
static struct pmic_glink_client*pmic_glink_register_client(struct device*,const struct pmic_glink_client_data*);
int pmic_glink_unregister_client(struct pmic_glink_client*);
static int pmic_glink_write(struct pmic_glink_client*,void*,size_t);
static void idr_remove(struct idr*i,u32 owner){assert(i->client&&i->client->id==owner);i->client=NULL;}
#define list_del_rcu(n) list_del(n)
static void synchronize_srcu(int*);
/* PARENT */
/* UNREGISTER */
/* CHILD */
static void assert_seq_locked(struct seq_file*s){struct votable*v=s->private;assert(v&&v->bd->lock.held&&pthread_equal(v->bd->lock.owner,pthread_self()));}
static struct pmic_glink_dev parent;
static int sends,send_error,reply_mode,fw_votables=2,fw_voters=3,fw_eff=0,fw_mask=1;
static int qbg_size=2;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static int send_entered,remove_entered,block_formatter,formatter_entered,resume_formatter;
static atomic_int rx_active,removed;
static int unregister_draining,block_rx,rx_entered,resume_rx;
static unsigned fences(void){unsigned n=0;struct pmic_glink_reply_fence*f;list_for_each_entry(f,&parent.reply_fences,node)n++;return n;}
static int deliver(void*data,size_t len){struct pmic_glink_hdr h;memcpy(&h,data,sizeof(h));mutex_lock(&parent.client_lock);bool drop=pmic_glink_filter_reply(&parent,data,len);struct pmic_glink_client*c=drop?NULL:idr_find(&parent.client_idr,h.owner);if(c)atomic_fetch_add(&rx_active,1);mutex_unlock(&parent.client_lock);if(!c)return drop?1:0;pthread_mutex_lock(&gate);if(block_rx){block_rx=0;rx_entered=1;pthread_cond_broadcast(&gate_cond);while(!resume_rx)pthread_cond_wait(&gate_cond,&gate);}pthread_mutex_unlock(&gate);int rc=c->msg_cb(c->priv,data,len);pthread_mutex_lock(&gate);atomic_fetch_sub(&rx_active,1);pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate);return rc;}
static struct pmic_glink_client*pmic_glink_register_client(struct device*d,const struct pmic_glink_client_data*x){if(register_error)return ERR_PTR(register_error);assert(!parent.client_idr.client);struct battery_dbg_dev*bd=x->priv;/* Initialization must precede an early callback, even malformed. */mutex_lock(&bd->lock);mutex_unlock(&bd->lock);struct pmic_glink_hdr early={x->id,1,BD_GET_VOTABLE_REQ};assert(!x->msg_cb(x->priv,&early,sizeof(early)));struct pmic_glink_client*c=kzalloc(sizeof(*c),0);assert(c);*c=(struct pmic_glink_client){&parent,x->id,x->priv,x->msg_cb,x->state_cb};INIT_LIST_HEAD(&c->node);if(c->state_cb)list_add_tail(&c->node,&parent.client_dev_list);parent.client_idr.client=c;return c;}
static void synchronize_srcu(int*s){pthread_mutex_lock(&gate);unregister_draining=1;pthread_cond_broadcast(&gate_cond);while(atomic_load(&rx_active))pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);}
static int pmic_glink_write(struct pmic_glink_client*c,void*data,size_t len){assert(c&&c==parent.client_idr.client);struct pmic_glink_hdr h;memcpy(&h,data,sizeof(h));
 /* Independent literal frame recovered from the signed CN306 stock binary. */
 if(h.opcode==BD_GET_AGGREGATOR_INFO_REQ){static const unsigned char stock[]={0x0d,0x80,0,0,1,0,0,0,0x15,0,0,0,0,0,0,0};assert(len==sizeof(stock));assert(!memcmp(data,stock,sizeof(stock)));}
 sends++;pthread_mutex_lock(&gate);send_entered=1;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate);if(send_error)return send_error;if(!reply_mode)return 0;
 if(h.opcode==BD_GET_AGGREGATOR_INFO_REQ){struct votables_list_resp_msg r={.hdr=h};r.all_data.num_votables=fw_votables;r.all_data.num_voters=fw_voters;for(unsigned i=0;i<MAX_NUM_VOTABLES;i++)snprintf(r.all_data.votables[i],MAX_NAME_LEN,"vote%u",i);for(unsigned i=0;i<MAX_NUM_VOTERS;i++)snprintf(r.all_data.voters[i],MAX_NAME_LEN,"voter%u",i);strcpy(r.all_data.voters[0],"glink");deliver(&r,reply_mode==3?sizeof(h):sizeof(r));return 0;}
 if(h.opcode==BD_GET_VOTABLE_REQ){struct votable_resp_msg r={.hdr=h};r.v_data.votable_id=((struct votable_req_msg*)data)->votable_id;r.v_data.active_voter_mask=fw_mask;r.v_data.eff_voter=fw_eff;r.v_data.eff_val=123;for(int i=0;i<MAX_NUM_VOTERS;i++)r.v_data.votes[i]=i+100;if(reply_mode==2)r.v_data.votable_id=(r.v_data.votable_id+1)%fw_votables;int rc=deliver(&r,sizeof(r));return rc<0?0:rc;}
 if(h.opcode==BD_QBG_DUMP_REQ){struct qbg_context_resp_msg r={.hdr=h,.length=qbg_size};memset(r.buf,'Q',sizeof(r.buf));int rc=deliver(&r,offsetof(struct qbg_context_resp_msg,buf)+qbg_size);return rc<0?0:rc;}
 return deliver(&h,sizeof(h));
}
static void reset(void){assert(!parent.client_idr.client&&!sysfs_present&&!atomic_load(&rx_active));pmic_glink_clear_requests(&parent);assert(!live_alloc);memset(entries,0,sizeof(entries));nentries=0;alloc_calls=alloc_fail_at=debug_calls=debug_fail_at=debug_null_at=sysfs_error=register_error=sends=send_error=wait_mode=send_entered=remove_entered=unregister_draining=block_rx=rx_entered=resume_rx=0;fw_votables=2;fw_voters=3;fw_eff=0;fw_mask=1;qbg_size=2;reply_mode=1;atomic_store(&removed,0);}
static void destroy(struct platform_device*p){assert(!sysfs_present&&!parent.client_idr.client);for(unsigned i=0;i<nentries;i++)assert(!entries[i].present);if(p->dev.data){struct battery_dbg_dev*bd=p->dev.data;pthread_mutex_destroy(&bd->lock.lock);pthread_mutex_destroy(&bd->response_lock);pthread_mutex_destroy(&bd->ack.lock);pthread_cond_destroy(&bd->ack.cond);}for(unsigned i=0;i<p->dev.allocations;i++)kfree(p->dev.alloc[i]);memset(p,0,sizeof(*p));}
static int read_vote(struct battery_dbg_dev*bd,u32 id){mutex_lock(&bd->lock);int rc=battery_dbg_request_read_votable(bd,id);mutex_unlock(&bd->lock);return rc;}
static void*run_request(void*p){struct battery_dbg_dev*bd=p;int*r=malloc(sizeof(*r));*r=read_vote(bd,0);return r;}
static void*run_remove(void*p){pthread_mutex_lock(&gate);remove_entered=1;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate);battery_dbg_remove(p);atomic_store(&removed,1);return NULL;}
static void*run_unregister(void*c){assert(!pmic_glink_unregister_client(c));atomic_store(&removed,1);return NULL;}
static void*run_delivery(void*r){int*answer=malloc(sizeof(*answer));*answer=deliver(r,sizeof(struct votable_resp_msg));return answer;}
int main(void){INIT_LIST_HEAD(&parent.client_dev_list);INIT_LIST_HEAD(&parent.reply_fences);mutex_init(&parent.client_lock);assert(!pthread_rwlock_init(&parent.rpdev_sem.lock,NULL));parent.rpdev=(void*)1;atomic_store(&parent.state.value,1);unsigned cases=0;struct platform_device p={0};
 /* Every native debugfs publication failure and NULL result must unwind. */
 for(int null=0;null<2;null++)for(int stage=1;stage<=18;stage++){reset();if(null)debug_null_at=stage;else debug_fail_at=stage;assert(battery_dbg_probe(&p)<0);destroy(&p);assert(!live_alloc&&!fences());cases++;}
 for(int fault=0;fault<5;fault++){reset();if(fault==0)alloc_fail_at=1;if(fault==1)alloc_fail_at=3;if(fault==2)register_error=-EPROTO;if(fault==3)sysfs_error=-ENOMEM;if(fault==4)alloc_fail_at=4;assert(battery_dbg_probe(&p)<0);destroy(&p);assert(!live_alloc&&!fences());cases++;}
 /* Full native boundary counts, including zero and the documented maximum. */
 for(int nv=0;nv<=MAX_NUM_VOTABLES;nv++)for(int nr=0;nr<=MAX_NUM_VOTERS;nr++){reset();fw_votables=nv;fw_voters=nr;assert(!battery_dbg_probe(&p));struct battery_dbg_dev*bd=p.dev.data;assert(bd->all_data.num_votables==(u32)nv&&bd->all_data.num_voters==(u32)nr);assert((nv>0)==(bd->votable!=NULL));for(unsigned j=0;j<nentries;j++)if(entries[j].isfile)assert(entries[j].protected);battery_dbg_remove(&p);destroy(&p);cases++;}
 reset();assert(!battery_dbg_probe(&p));struct battery_dbg_dev*bd=p.dev.data;
 /* Actual callback parsing for exact heap extents, with forged lengths. */
 u32 ops[]={BD_GET_AGGREGATOR_INFO_REQ,BD_GET_VOTABLE_REQ,BD_QBG_DUMP_REQ};size_t sizes[]={sizeof(struct votables_list_resp_msg),sizeof(struct votable_resp_msg),sizeof(struct qbg_context_resp_msg)};
 for(unsigned op=0;op<3;op++)for(size_t len=0;len<=sizes[op]+1;len++){unsigned char*raw=calloc(1,len?:1);assert(raw);if(len>=sizeof(struct pmic_glink_hdr)){struct pmic_glink_hdr h={MSG_OWNER_BD,1,ops[op]};memcpy(raw,&h,sizeof(h));}int rc=battery_dbg_callback(bd,raw,len);if(len<sizeof(struct pmic_glink_hdr))assert(rc==-EINVAL);else if(op<2)assert((len==sizes[op])?(rc==0):(rc==-EPROTO));else assert((len>=offsetof(struct qbg_context_resp_msg,buf)&&len<=sizes[op])?rc==0:rc==-EPROTO);free(raw);cases++;}
 for(unsigned length=0;length<=MAX_BUF_LEN+1;length++){struct qbg_context_resp_msg *r=calloc(1,sizeof(*r));assert(r);r->hdr=(struct pmic_glink_hdr){MSG_OWNER_BD,1,BD_QBG_DUMP_REQ};r->length=length;assert(battery_dbg_callback(bd,r,sizeof(*r))==(length<=MAX_BUF_LEN?0:-EPROTO));if(length&&length<=MAX_BUF_LEN)assert(battery_dbg_callback(bd,r,offsetof(struct qbg_context_resp_msg,buf)+length-1)==-EPROTO);free(r);cases++;}
 /* Every voter-mask boundary and each effective voter index. */
 for(unsigned nr=0;nr<=MAX_NUM_VOTERS;nr++)for(unsigned eff=0;eff<=MAX_NUM_VOTERS;eff++){bd->all_data.num_voters=nr;struct votable_resp_msg r={.hdr={MSG_OWNER_BD,1,BD_GET_VOTABLE_REQ},.v_data={.votable_id=0,.active_voter_mask=1,.eff_voter=eff}};int rc=battery_dbg_callback(bd,&r,sizeof(r));assert(rc==((nr&&eff<nr)?0:-EPROTO));cases++;}bd->all_data.num_voters=3;
 /* All formatters must retain their request mutex through response copying. */
 struct seq_file seq={.private=&bd->votable[0]};assert(!active_show(&seq,NULL)&&!strcmp(seq.buf,"0x1\n"));seq.len=0;assert(!winvote_show(&seq,NULL)&&!strcmp(seq.buf,"123\n"));seq.len=0;assert(!winner_show(&seq,NULL)&&!strcmp(seq.buf,"glink\n"));seq.len=0;assert(!voters_show(&seq,NULL)&&!strcmp(seq.buf,"glink \n"));seq.len=0;assert(!votes_show(&seq,NULL)&&!strcmp(seq.buf,"100 \n"));seq.len=0;assert(!status_show(&seq,NULL)&&strstr(seq.buf,"EFFECTIVE:"));fw_mask=0;seq.len=0;assert(!status_show(&seq,NULL)&&!strcmp(seq.buf,"\n"));fw_mask=1;u64 val=99;assert(!override_get(&bd->votable[0],&val)&&val==100);assert(override_set(&bd->votable[0],(u64)U32_MAX+1)==-ERANGE);assert(!override_set(&bd->votable[0],99));bd->override_voter_id=MAX_NUM_VOTERS;assert(override_set(&bd->votable[0],99)==-EOPNOTSUPP);assert(!override_get(&bd->votable[0],&val)&&!val);cases+=12;
 /* Read-only QBG data consumption and battery-cell selection do not overflow. */
 char out[PAGE_SIZE];assert(battery_cell_id_store(&p.dev,NULL,"4294967295",10)==10);assert(battery_cell_id_show(&p.dev,NULL,out)==11&&!strcmp(out,"4294967295\n"));assert(battery_cell_id_store(&p.dev,NULL,"4294967296",10)==-EINVAL);assert(!get_qbg_context_write(bd,0));assert(qbg_blob_read(NULL,&p.dev.kobj,NULL,out,0,10)==2&&!memcmp(out,"QQ",2));assert(qbg_blob_read(NULL,&p.dev.kobj,NULL,out,-1,10)==-EINVAL);cases+=6;
 /* Rejected same-envelope/wrong-ID reply must preserve the fence across reload. */
 reply_mode=2;assert(read_vote(bd,0)==-EPROTO&&fences()==1);unsigned before=sends;assert(read_vote(bd,0)==-EAGAIN&&sends==before);battery_dbg_remove(&p);destroy(&p);assert(fences()==1);reply_mode=1;assert(!battery_dbg_probe(&p));bd=p.dev.data;assert(read_vote(bd,0)==-EAGAIN);struct votable_resp_msg late={.hdr={MSG_OWNER_BD,1,BD_GET_VOTABLE_REQ},.v_data={.votable_id=0,.active_voter_mask=1,.eff_voter=0}};late.v_data.votable_id=1;assert(deliver(&late,sizeof(late))==1&&fences()==1);late.v_data.votable_id=0;assert(deliver(&late,sizeof(late))==1&&!fences());assert(!read_vote(bd,0));cases+=5;
 /* Wrong ID after timeout while still bound cannot release the record. */
 reply_mode=0;assert(read_vote(bd,0)==-ETIMEDOUT&&fences()==1);late.v_data.votable_id=1;assert(deliver(&late,sizeof(late))==-EPROTO&&fences()==1);late.v_data.votable_id=0;assert(!deliver(&late,sizeof(late))&&!fences());reply_mode=1;cases+=3;
 /* A failed send did not submit: even ENOTCONN must release its empty record. */
 send_error=-ENOTCONN;assert(read_vote(bd,0)==-ENOTCONN&&!fences());send_error=0;assert(!read_vote(bd,0));cases+=2;
 /* Native PDR down/up does not prove channel drain; old replies still fence. */
 reply_mode=0;assert(read_vote(bd,0)==-ETIMEDOUT);battery_dbg_state_cb(bd,PMIC_GLINK_STATE_DOWN);battery_dbg_state_cb(bd,PMIC_GLINK_STATE_UP);assert(read_vote(bd,0)==-EAGAIN);assert(!deliver(&late,sizeof(late))&&!fences());reply_mode=1;assert(!read_vote(bd,0));cases+=4;
 battery_dbg_remove(&p);destroy(&p);reset();
 /* A removal or PDR callback aborts a live waiting request before teardown. */
 for(int kind=0;kind<2;kind++){reset();assert(!battery_dbg_probe(&p));bd=p.dev.data;reply_mode=0;wait_mode=1;pthread_t tx,rm;assert(!pthread_create(&tx,NULL,run_request,bd));pthread_mutex_lock(&gate);while(!send_entered)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);if(!kind)assert(!pthread_create(&rm,NULL,run_remove,&p));else battery_dbg_state_cb(bd,PMIC_GLINK_STATE_DOWN);void*answer;assert(!pthread_join(tx,&answer));assert(*(int*)answer==(!kind?-ESHUTDOWN:-ENOTCONN));free(answer);if(!kind)assert(!pthread_join(rm,NULL)&&atomic_load(&removed));else battery_dbg_remove(&p);destroy(&p);pmic_glink_clear_requests(&parent);assert(!live_alloc);cases++;}
 /* Actual unregister must drain rejection before classifying an envelope.
  * A new same-owner client must not receive the detached client's replies. */
 reset();assert(!battery_dbg_probe(&p));bd=p.dev.data;reply_mode=0;assert(read_vote(bd,0)==-ETIMEDOUT);late.v_data.active_voter_mask=1u<<20;block_rx=1;pthread_t rx,unreg;assert(!pthread_create(&rx,NULL,run_delivery,&late));pthread_mutex_lock(&gate);while(!rx_entered)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);assert(!pthread_create(&unreg,NULL,run_unregister,bd->client));pthread_mutex_lock(&gate);while(!unregister_draining)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);assert(!atomic_load(&removed));
 struct pmic_glink_client replacement={.pgdev=&parent,.id=MSG_OWNER_BD,.priv=bd,.msg_cb=battery_dbg_callback};mutex_lock(&parent.client_lock);parent.client_idr.client=&replacement;mutex_unlock(&parent.client_lock);late.v_data.active_voter_mask=1;assert(deliver(&late,sizeof(late))==1&&fences()==1);late.v_data.active_voter_mask=1u<<20;
 pthread_mutex_lock(&gate);resume_rx=1;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate);void*answer;assert(!pthread_join(rx,&answer));assert(*(int*)answer==-EPROTO);free(answer);assert(!pthread_join(unreg,NULL)&&atomic_load(&removed));assert(fences()==1);mutex_lock(&parent.client_lock);parent.client_idr.client=NULL;mutex_unlock(&parent.client_lock);sysfs_remove_group(&p.dev.kobj,&battery_dbg_group);debugfs_remove_recursive(bd->debugfs_dir);destroy(&p);late.v_data.active_voter_mask=1;assert(deliver(&late,sizeof(late))==1&&!fences());cases+=6;
 /* A native malformed aggregator reply completes the wait as EPROTO while
  * preserving the guard, including after probe has destroyed the client. */
 reset();reply_mode=3;wait_mode=1;
 assert(!battery_dbg_probe(&p)&&fences()==1&&sends==1&&sysfs_present);
 struct battery_dbg_dev *failed=p.dev.data;
 assert(failed->votables_error==-EPROTO&&failed->ack.done&&!failed->pending&&failed->all_data.num_votables==0);
 for(unsigned j=0;j<nentries;j++)assert(!entries[j].present);
 reply_mode=1;assert(!get_qbg_context_write(failed,0)&&failed->qbg_blob.size==2);
 battery_dbg_remove(&p);destroy(&p);wait_mode=0;
 assert(!battery_dbg_probe(&p)&&sends==2&&fences()==1&&sysfs_present);
 failed=p.dev.data;assert(failed->votables_error==-EAGAIN);
 battery_dbg_remove(&p);destroy(&p);
 struct votables_list_resp_msg agg={.hdr={MSG_OWNER_BD,1,BD_GET_AGGREGATOR_INFO_REQ}};
 assert(deliver(&agg,sizeof(agg)-1)==1&&fences()==1);
 assert(deliver(&agg,sizeof(agg))==1&&!fences());
 assert(!battery_dbg_probe(&p));bd=p.dev.data;cases+=6;
 /* A malformed matching message must wake a genuinely blocked waiter.
  * Wrong opcodes and types cannot wake it or publish state. */
 reply_mode=0;wait_mode=1;send_entered=0;pthread_t waiting;
 assert(!pthread_create(&waiting,NULL,run_request,bd));
 pthread_mutex_lock(&gate);while(!send_entered)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);
 struct pmic_glink_hdr malformed={MSG_OWNER_BD,1,BD_QBG_DUMP_REQ};
 assert(deliver(&malformed,sizeof(malformed))==-EPROTO&&bd->pending);
 malformed=(struct pmic_glink_hdr){MSG_OWNER_BD,2,BD_GET_VOTABLE_REQ};
 assert(deliver(&malformed,sizeof(malformed))==-EINVAL&&bd->pending);
 malformed.type=1;
 assert(deliver(&malformed,sizeof(malformed))==-EPROTO);
 assert(!pthread_join(waiting,&answer)&&*(int*)answer==-EPROTO);free(answer);
 assert(fences()==1&&!bd->pending&&bd->votable[0].data.eff_val==0);
 late.v_data.active_voter_mask=1;assert(!deliver(&late,sizeof(late))&&!fences());
 send_entered=0;assert(!pthread_create(&waiting,NULL,run_request,bd));
 pthread_mutex_lock(&gate);while(!send_entered)pthread_cond_wait(&gate_cond,&gate);pthread_mutex_unlock(&gate);
 late.v_data.active_voter_mask=1u<<20;late.v_data.eff_val=999;
 assert(deliver(&late,sizeof(late))==-EPROTO);
 assert(!pthread_join(waiting,&answer)&&*(int*)answer==-EPROTO);free(answer);
 assert(fences()==1&&!bd->pending&&bd->votable[0].data.eff_val==0);
 late.v_data.active_voter_mask=1;assert(!deliver(&late,sizeof(late))&&!fences());
 battery_dbg_remove(&p);destroy(&p);cases+=7;
 /* Aggregation silence cannot remove the independently working native QBG
  * files or publish invented voters. No retry bypasses the old record. */
 reset();reply_mode=0;assert(!battery_dbg_probe(&p));bd=p.dev.data;
 assert(bd->votables_error==-ETIMEDOUT&&fences()==1&&sends==1&&sysfs_present);
 assert(!bd->votable&&!bd->debugfs_dir&&bd->all_data.num_votables==0);
 for(unsigned j=0;j<nentries;j++)assert(!entries[j].present);
 assert(votables_status_show(&p.dev,NULL,out)==19&&!strcmp(out,"unavailable (-110)\n"));
 reply_mode=1;assert(!get_qbg_context_write(bd,0)&&sends==2&&fences()==1);
 assert(qbg_blob_read(NULL,&p.dev.kobj,NULL,out,0,sizeof(out))==2&&!memcmp(out,"QQ",2));
 assert(battery_dbg_get_votables_list(bd)==-EAGAIN&&sends==2);
 battery_dbg_remove(&p);destroy(&p);cases+=7;
 reset();pthread_mutex_destroy(&parent.client_lock.lock);pthread_rwlock_destroy(&parent.rpdev_sem.lock);printf("%u\n",cases);
}
