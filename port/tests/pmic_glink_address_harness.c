#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
#define GENMASK(h,l) (((1UL<<((h)+1))-1)&~((1UL<<(l))-1))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define I2C_M_RD 1
#define I2C_FUNC_I2C 1
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define EPROBE_DEFER 517
struct mutex { pthread_mutex_t lock; };
static void mutex_init(struct mutex *m) { assert(!pthread_mutex_init(&m->lock,NULL)); }
static void mutex_lock(struct mutex *m) { assert(!pthread_mutex_lock(&m->lock)); }
static void mutex_unlock(struct mutex *m) { assert(!pthread_mutex_unlock(&m->lock)); }
typedef pthread_mutex_t spinlock_t;
#define spin_lock_init(p) assert(!pthread_mutex_init(p,NULL))
#define spin_lock_irqsave(p,f) do {(f)=0;assert(!pthread_mutex_lock(p));}while(0)
#define spin_unlock_irqrestore(p,f) assert(!pthread_mutex_unlock(p))
struct completion { bool done; };
static void init_completion(struct completion *c) { c->done=false; }
static void reinit_completion(struct completion *c) { c->done=false; }
static void complete(struct completion *c) { c->done=true; }
static unsigned msecs_to_jiffies(unsigned ms) { return ms; }
static unsigned wait_for_completion_timeout(struct completion *c,unsigned timeout) { return c->done; }
struct device { int unused; };
struct pmic_glink_client { int unused; };
struct pmic_glink_hdr { u32 owner,type,opcode; };
struct i2c_adapter { void *data; };
struct i2c_msg { u16 addr,flags,len;u8 *buf; };
struct i2c_algorithm { int (*master_xfer)(struct i2c_adapter*,struct i2c_msg*,int);u32 (*functionality)(struct i2c_adapter*); };
static void *i2c_get_adapdata(struct i2c_adapter *a) { return a->data; }
struct spmi_controller { void *data; };
static void *spmi_controller_get_drvdata(struct spmi_controller *s) { return s->data; }
static u16 get_unaligned_be16(const void *data) { const u8 *b=data;return ((u16)b[0]<<8)|b[1]; }
static int pmic_glink_write(struct pmic_glink_client*,void*,size_t);
/* DRIVER */
static struct pmic_glink_debug_dev gd;
static struct pmic_glink_client client;
static unsigned sends,reads,writes;
static bool no_reply,wrong_sid,short_reply;
static int send_error;
static u32 last_sid,last_addr,last_bytes;
static int pmic_glink_write(struct pmic_glink_client *c,void *data,size_t len)
{
    assert(c==&client);struct reg_dump_read_req_msg *q=data;
    assert(q->hdr.owner==MSG_OWNER_REG_DUMP&&q->hdr.type==MSG_TYPE_REQ_RESP);
    ++sends;last_sid=q->pmic_sid;last_addr=q->address;
    if(send_error)return send_error;
    if(q->hdr.opcode==REG_DUMP_REG_READ_REQ) {
        ++reads;assert(len==sizeof(*q)&&q->byte_count<=256);
        last_bytes=q->byte_count;
        if(no_reply)return 0;
        struct reg_dump_read_resp_msg response={.hdr=q->hdr,.bus_id=q->bus_id,.pmic_sid=q->pmic_sid,.address=q->address,.byte_count=q->byte_count};
        if(wrong_sid)++response.pmic_sid;
        for(unsigned i=0;i<q->byte_count;i++)response.data[i]=q->pmic_sid+q->address+i;
        pmic_glink_debug_callback(&gd,&response,short_reply?sizeof(response)-1:sizeof(response));
    } else {
        ++writes;assert(q->hdr.opcode==REG_DUMP_REG_WRITE_REQ && len==sizeof(struct reg_dump_write_req_msg));
        if(no_reply)return 0;
        struct reg_dump_write_resp_msg response={.hdr=q->hdr};
        assert(!pmic_glink_debug_callback(&gd,&response,sizeof(response)));
    }
    return 0;
}
static void reset(void)
{
    gd=(struct pmic_glink_debug_dev){.client=&client};mutex_init(&gd.lock);spin_lock_init(&gd.response_lock);init_completion(&gd.ack);
    sends=reads=writes=0;no_reply=wrong_sid=short_reply=false;send_error=0;
}
static void destroy(void) { pthread_mutex_destroy(&gd.lock.lock);pthread_mutex_destroy(&gd.response_lock); }
int main(void)
{
    unsigned cases=0;u8 output[512],address[2]={1,4},value[3]={1,4,7};
    struct i2c_glink_ctrl i2c={.gd=&gd,.bus_id=2};i2c.i2c.data=&i2c;
    struct spmi_glink_ctrl spmi={.gd=&gd,.bus_id=0};struct spmi_controller ctrl={.data=&spmi};
    /* Real regmap two-message reads for every seven-bit I2C address, including Onyx 0x68/0x69. */
    for(unsigned sid=0;sid<256;sid++)for(unsigned bytes=1;bytes<=4;bytes++) {
        reset();struct i2c_msg msgs[2]={{sid,0,2,address},{sid,I2C_M_RD,bytes,output}};
        int rc=i2c_glink_xfer(&i2c.i2c,msgs,2);
        if(sid<=0x7f) {assert(rc==2&&sends==1&&last_sid==sid&&last_addr==0x104&&last_bytes==bytes);for(unsigned j=0;j<bytes;j++)assert(output[j]==(u8)(sid+0x104+j));}
        else assert(rc==-EINVAL&&!sends);
        destroy();++cases;
    }
    /* I2C register writes have the same address namespace; mock only. */
    /* Reject wide message addresses before narrowing them into the wire u8. */
    for(unsigned sid=256;sid<65536;sid++) {
        reset();struct i2c_msg msgs[2]={{sid,0,2,address},{sid,I2C_M_RD,1,output}};
        assert(i2c_glink_xfer(&i2c.i2c,msgs,2)==-EINVAL&&!sends);destroy();++cases;
    }
    for(unsigned sid=0;sid<256;sid++) {
        reset();struct i2c_msg msg={sid,0,3,value};int rc=i2c_glink_xfer(&i2c.i2c,&msg,1);
        assert((rc==1)==(sid<=127));assert(sends==(sid<=127));destroy();++cases;
    }
    /* SPMI retains a four-bit SID and rejects requests that cross the last SID. */
    for(unsigned sid=0;sid<256;sid++) {
        reset();int rc=spmi_glink_read_cmd(&ctrl,0,sid,0x104,output,1);
        assert((rc==0)==(sid<=15));assert(sends==(sid<=15));
        unsigned before=sends;rc=spmi_glink_write_cmd(&ctrl,0,sid,0x104,value,1);
        assert((rc==0)==(sid<=15));assert(sends-before==(sid<=15));destroy();++cases;
    }
    for(unsigned sid=0;sid<128;sid++)for(unsigned len=1;len<260;len++) {
        reset();int rc=pmic_glink_debug_read_regs(&gd,2,sid,0xfffe,output,len);
        if(sid==127 && len>2)assert(rc==-EINVAL&&!sends);
        else {assert(!rc&&sends>=1&&sends<=3);assert(last_sid==sid+(len>2));}
        destroy();++cases;
    }
    reset();assert(spmi_glink_read_cmd(&ctrl,0,15,0xffff,output,2)==-EINVAL&&!sends);
    assert(spmi_glink_write_cmd(&ctrl,0,15,0xffff,value,2)==-EINVAL&&!sends);
    assert(pmic_glink_debug_read_regs(&gd,2,128,0,output,1)==-EINVAL&&!sends);
    assert(pmic_glink_debug_write_regs(&gd,2,128,0,value,1)==-EINVAL&&!sends);destroy();cases+=4;
    /* Reject malformed vectors without issuing any hardware command. */
    for(unsigned flags=1;flags<8;flags++) {
        reset();struct i2c_msg msgs[2]={{0x68,flags,2,address},{0x68,I2C_M_RD,1,output}};
        assert(i2c_glink_xfer(&i2c.i2c,msgs,2)==-EINVAL&&!sends);destroy();++cases;
    }
    reset();struct i2c_msg msgs[2]={{0x68,0,2,address},{0x69,I2C_M_RD,1,output}};
    assert(i2c_glink_xfer(&i2c.i2c,msgs,2)==-EINVAL&&!sends);
    msgs[1].addr=0x68;msgs[1].buf=NULL;assert(i2c_glink_xfer(&i2c.i2c,msgs,2)==-EINVAL&&!sends);destroy();cases+=2;
    /* Existing reply fences must remain effective for seven-bit addresses. */
    for(unsigned fault=0;fault<3;fault++) {
        reset();no_reply=fault==0;wrong_sid=fault==1;short_reply=fault==2;
        int rc=pmic_glink_debug_read_regs(&gd,2,0x68,0x104,output,1);
        assert(rc==(fault==2?-EPROTO:-ETIMEDOUT)&&gd.poisoned&&sends==1);
        assert(pmic_glink_debug_read_regs(&gd,2,0x69,0x104,output,1)==-EIO&&sends==1);destroy();++cases;
    }
    reset();send_error=-ENOTCONN;assert(pmic_glink_debug_read_regs(&gd,2,0x68,0x104,output,1)==-ENOTCONN&&!gd.poisoned);
    send_error=0;assert(!pmic_glink_debug_read_regs(&gd,2,0x68,0x104,output,1));destroy();++cases;
    printf("%u\n",cases);
}
