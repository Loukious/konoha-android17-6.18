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
typedef unsigned long irq_hw_number_t;
typedef int irqreturn_t;
#define __maybe_unused
#define GFP_KERNEL 0
#define BIT(n) (1UL << (n))
#define GENMASK(h,l) (((1UL << ((h)+1))-1)&~((1UL<<(l))-1))
#define BITS_PER_BYTE 8
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define READ_ONCE(n) (n)
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define pm_sleep_ptr(n) (n)
#define IRQ_TYPE_EDGE_RISING 1
#define IRQ_TYPE_EDGE_FALLING 2
#define IRQ_TYPE_EDGE_BOTH 3
#define IRQ_TYPE_LEVEL_HIGH 4
#define IRQ_TYPE_LEVEL_LOW 8
#define IRQ_TYPE_SENSE_MASK 15
#define IRQF_ONESHOT 1
#define IRQF_SHARED 2
#define IRQ_HANDLED 1
#define MODULE_DEVICE_TABLE(a,b)
#define MODULE_DESCRIPTION(a)
#define MODULE_ALIAS(a)
#define MODULE_LICENSE(a)
#define module_i2c_driver(a)
#define dev_err(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define pr_err(...) ((void)0)
static bool is_power_of_2(unsigned long n) { return n && !(n & (n-1)); }
static void usleep_range(unsigned a,unsigned b) { assert(a==5000 && b==5500); }
struct mutex { pthread_mutex_t lock; bool ready; };
static void mutex_init(struct mutex *m)
{
    pthread_mutexattr_t attr; assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_ERRORCHECK));
    assert(!pthread_mutex_init(&m->lock,&attr)); pthread_mutexattr_destroy(&attr); m->ready=true;
}
static void mutex_lock(struct mutex *m) { assert(m->ready && !pthread_mutex_lock(&m->lock)); }
static void mutex_unlock(struct mutex *m) { assert(m->ready && !pthread_mutex_unlock(&m->lock)); }
struct device_node { bool controller, toggle; int count; u32 values[257]; const char *pinctrl; };
struct device { struct device_node *of_node; void *drvdata; };
struct i2c_client { struct device dev; int irq; };
struct pinctrl { int unused; };
struct regmap { u8 regs[65536]; };
static struct regmap map;
static struct { unsigned reg, value; } writes[128];
static unsigned write_calls, fail_write, read_calls, fail_read, connect_errors;
static bool permanent_write_failure;
static int regmap_write(struct regmap *m,unsigned reg,unsigned val)
{
    assert(m==&map && reg<=65535 && val<=255);
    assert(write_calls<ARRAY_SIZE(writes)); writes[write_calls]=(typeof(writes[0])){reg,val};
    if (++write_calls==fail_write || permanent_write_failure) return -EIO;
    if ((reg&255)==0x15) m->regs[reg] |= val;
    else if ((reg&255)==0x16) m->regs[(reg&0xff00)|0x15] &= ~val;
    else if ((reg&255)==0x14) m->regs[(reg&0xff00)|0x18] &= ~val;
    else m->regs[reg]=val;
    return 0;
}
static int regmap_read(struct regmap *m,unsigned reg,unsigned *val)
{
    assert(m==&map && reg<=65535); if(++read_calls==fail_read) return -EIO; *val=m->regs[reg]; return 0;
}
static int regmap_bulk_read(struct regmap *m,unsigned reg,void *buf,size_t n)
{
    assert(m==&map && reg+n<=65536); ++read_calls;
    if(connect_errors) { --connect_errors; return -ENOTCONN; }
    if(read_calls==fail_read) return -EIO;
    memcpy(buf,m->regs+reg,n); return 0;
}
static void *allocated[8];
static unsigned alloc_count, alloc_calls, fail_alloc;
static void *devm_kcalloc(struct device *d,size_t n,size_t size,unsigned flags)
{
    if(++alloc_calls==fail_alloc) return NULL;
    assert(alloc_count<ARRAY_SIZE(allocated)); void *p=calloc(n,size); assert(p); allocated[alloc_count++]=p; return p;
}
#define devm_kzalloc(d,n,g) devm_kcalloc(d,1,n,g)
struct regmap_config { int reg_bits,val_bits; unsigned max_register; };
static int map_error,debug_error;
static bool debug_registered;
static struct regmap *devm_regmap_init_i2c(struct i2c_client *c,struct regmap_config *config)
{
    assert(config->reg_bits==16 && config->val_bits==8 && config->max_register==65535);
    return map_error?ERR_PTR(map_error):&map;
}
static int devm_regmap_qti_debugfs_register(struct device *d,struct regmap *m)
{
    assert(m==&map); if(debug_error) return debug_error; debug_registered=true; return 0;
}
static void i2c_set_clientdata(struct i2c_client *c,void *p) { c->dev.drvdata=p; }
static void *i2c_get_clientdata(struct i2c_client *c) { return c->dev.drvdata; }
static void *dev_get_drvdata(struct device *d) { return d->drvdata; }
static bool of_property_read_bool(struct device_node *n,const char *name)
{
    if(!n) return false;
    return !strcmp(name,"interrupt-controller")?n->controller:!strcmp(name,"qcom,enable-toggle-stat")?n->toggle:false;
}
static int of_property_count_u32_elems(struct device_node *n,const char *name) { return n->count; }
static int of_property_read_u32_index(struct device_node *n,const char *name,unsigned i,u32 *out)
{
    if(i>=(unsigned)n->count) return -EINVAL; *out=n->values[i]; return 0;
}
static int of_property_read_string(struct device_node *n,const char *name,const char **out)
{
    if(!n->pinctrl) return -ENOENT; *out=n->pinctrl; return 0;
}
struct irq_data { irq_hw_number_t hwirq; void *chip_data; };
static void *irq_data_get_irq_chip_data(struct irq_data *d) { return d->chip_data; }
struct irq_chip {
    const char *name;
    void (*irq_bus_lock)(struct irq_data *),(*irq_bus_sync_unlock)(struct irq_data *),(*irq_disable)(struct irq_data *),(*irq_enable)(struct irq_data *);
    int (*irq_set_type)(struct irq_data *,unsigned),(*irq_set_wake)(struct irq_data *,unsigned);
};
static void handle_level_irq(void) {}
static void irq_set_chip_data(unsigned irq,void *data) { assert(data); }
static void irq_set_chip_and_handler(unsigned irq,struct irq_chip *chip,void (*handler)(void)) {}
static void irq_set_nested_thread(unsigned irq,int nested) { assert(nested==1); }
static void irq_set_noprobe(unsigned irq) {}
struct irq_domain { void *host_data; bool live; };
struct irq_domain_ops {
    int (*map)(struct irq_domain *,unsigned,irq_hw_number_t);
    int (*xlate)(struct irq_domain *,struct device_node *,const u32 *,unsigned,unsigned long *,unsigned *);
};
static struct irq_domain domain;
static bool domain_fail, irq_live;
static unsigned children, nested_calls;
static struct irq_domain *irq_domain_add_tree(struct device_node *n,const struct irq_domain_ops *ops,void *data)
{
    if(domain_fail) return NULL; assert(!domain.live); domain=(struct irq_domain){data,true}; return &domain;
}
static void irq_domain_remove(struct irq_domain *d) { assert(d==&domain && d->live && !irq_live && !children); d->live=false; }
static bool map_irq;
static unsigned irq_find_mapping(struct irq_domain *d,unsigned hw) { assert(d&&d->live); return map_irq?hw+1:0; }
static void handle_nested_irq(unsigned irq) { ++nested_calls; }
static int pinctrl_error;
static struct pinctrl pinctrl;
static struct pinctrl *devm_pinctrl_get_select(struct device *d,const char *name) { return pinctrl_error?ERR_PTR(pinctrl_error):&pinctrl; }
static int request_error, wake_error, populate_error;
static unsigned irq_disable_depth, wake_depth;
static int devm_request_threaded_irq(struct device *d,int irq,void *primary,irqreturn_t (*thread)(int,void*),unsigned flags,const char *name,void *data)
{
    if(request_error) return request_error; assert(!irq_live && domain.live); irq_live=true; return 0;
}
static void devm_free_irq(struct device *d,int irq,void *data) { assert(irq_live && !irq_disable_depth && !wake_depth); irq_live=false; }
static int enable_irq_wake(int irq) { if(wake_error) return wake_error; ++wake_depth; return 0; }
static int disable_irq_wake(int irq) { assert(wake_depth); --wake_depth; return 0; }
static void disable_irq_nosync(int irq) { ++irq_disable_depth; }
static void enable_irq(int irq) { assert(irq_disable_depth); --irq_disable_depth; }
static int of_platform_populate(struct device_node *n,void *a,void *b,struct device *d) { children=1; return populate_error; }
static void of_platform_depopulate(struct device *d) { assert(!irq_live); children=0; }
struct dev_pm_ops { int (*suspend)(struct device *),(*suspend_noirq)(struct device *),(*resume)(struct device *); };
struct of_device_id { const char *compatible; };
struct i2c_device_id { const char *name; unsigned data; };
struct i2c_driver {
    struct { const char *name;const struct dev_pm_ops *pm;const struct of_device_id *of_match_table; } driver;
    int (*probe)(struct i2c_client *);void (*remove)(struct i2c_client *);const struct i2c_device_id *id_table;
};
/* DRIVER */
static void release(void)
{
    for(unsigned i=0;i<alloc_count;i++) free(allocated[i]); alloc_count=0; debug_registered=false;
}
static void reset(void)
{
    assert(!irq_live && !domain.live && !children && !alloc_count && !irq_disable_depth && !wake_depth);
    write_calls=read_calls=fail_write=fail_read=connect_errors=alloc_calls=fail_alloc=nested_calls=0;
    map_error=debug_error=pinctrl_error=request_error=wake_error=populate_error=0;
    domain_fail=map_irq=permanent_write_failure=false; memset(&map,0,sizeof(map));
}
static struct device_node node;
static struct i2c_client client;
static void node_init(unsigned n,bool controller)
{
    node=(struct device_node){.count=n,.controller=controller};
    for(unsigned i=0;i<n && i<ARRAY_SIZE(node.values);i++) node.values[i]=i;
    client=(struct i2c_client){.dev={.of_node=&node},.irq=21};
}
static struct i2c_pmic_periph periph;
static struct i2c_pmic chip;
static void init_local(void)
{
    periph=(struct i2c_pmic_periph){.addr=0x1200}; mutex_init(&periph.lock);
    chip=(struct i2c_pmic){.dev=&client.dev,.regmap=&map,.periph=&periph,.num_periphs=1,.summary_irq=21,.resume_completed=true};
    mutex_init(&chip.irq_complete); periph.data=&chip;
}
int main(void)
{
    unsigned cases=0; reset(); node_init(0,false);
    assert(!i2c_pmic_probe(&client) && !read_calls && !write_calls && debug_registered && !irq_live);
    assert(!i2c_pmic_suspend(&client.dev) && !i2c_pmic_resume(&client.dev) && !i2c_pmic_suspend_noirq(&client.dev));
    i2c_pmic_remove(&client);release(); ++cases;
    for(unsigned stage=0;stage<11;stage++) {
        reset();node_init(9,true);node.pinctrl="default";
        switch(stage) {
        case 0:fail_alloc=1;break;
        case 1:map_error=-EIO;break;
        case 2:debug_error=-ENOMEM;break;
        case 3:fail_alloc=2;break;
        case 4:domain_fail=true;break;
        case 5:fail_read=1;break;
        case 6:pinctrl_error=-EIO;break;
        case 7:request_error=-EINVAL;break;
        case 8:populate_error=-ENOMEM;break;
        case 9:node.values[8]=256;break;
        case 10:node.values[8]=0;break;
        }
        assert(i2c_pmic_probe(&client)<0 && !irq_live && !domain.live && !children && !wake_depth);
        release(); ++cases;
    }
    for(int n=-1;n<=257;n++) {
        reset();node_init(n>0?n:0,true);node.count=n;
        int rc=i2c_pmic_probe(&client);
        if(n<=0 || n>256) assert(rc<0);else {assert(!rc);i2c_pmic_remove(&client);}
        release();++cases;
    }
    reset();node_init(9,true);wake_error=-EOPNOTSUPP;
    assert(!i2c_pmic_probe(&client) && !wake_depth);i2c_pmic_remove(&client);release();++cases;
    reset();node_init(9,true);assert(!i2c_pmic_probe(&client));
    struct i2c_pmic *live=client.dev.drvdata;
    /* A summary byte's unused tail bits cannot index past the last peripheral. */
    map.regs[I2C_INTR_STATUS_BASE+1]=255;
    unsigned before_reads=read_calls;
    assert(i2c_pmic_irq_handler(21,live)==IRQ_HANDLED && read_calls-before_reads==3);
    for(unsigned i=0;i<9;i++) {
        live->periph[i].synced[IRQ_EN_SET]=5;live->periph[i].wake=2;
        map.regs[live->periph[i].addr|INT_EN_SET_OFFSET]=5;
    }
    write_calls=0;fail_write=1;
    assert(i2c_pmic_suspend(&client.dev)==-EIO && live->resume_completed);
    for(unsigned i=0;i<9;i++) assert(map.regs[live->periph[i].addr|INT_EN_SET_OFFSET]==5);
    fail_write=0;++cases;
    i2c_pmic_remove(&client);release();++cases;
    reset();init_local();struct irq_data irq={.hwirq=0x1201,.chip_data=&periph};
    for(unsigned bit=0;bit<8;bit++) for(unsigned initial=0;initial<256;initial++) for(unsigned type=0;type<5;type++) {
        unsigned mask=1U<<bit;irq.hwirq=0x1200|mask;
        periph.cached[IRQ_SET_TYPE]=periph.cached[IRQ_POL_HIGH]=periph.cached[IRQ_POL_LOW]=initial;
        unsigned edge=(unsigned[]){1,2,3,4,8}[type];
        i2c_pmic_irq_bus_lock(&irq);assert(!i2c_pmic_irq_set_type(&irq,edge));
        assert((periph.cached[IRQ_SET_TYPE]&mask)==(edge<4?mask:0));
        assert((periph.cached[IRQ_POL_HIGH]&mask)==((edge==1||edge==3||edge==4)?mask:0));
        assert((periph.cached[IRQ_POL_LOW]&mask)==((edge==2||edge==3||edge==8)?mask:0));
        assert((periph.cached[IRQ_SET_TYPE]&~mask)==(initial&~mask));
        /* Wake setter runs under the same IRQ core bus lock. */
        assert(!i2c_pmic_irq_set_wake(&irq,1) && (periph.wake&mask));
        assert(!i2c_pmic_irq_set_wake(&irq,0) && !(periph.wake&mask));
        mutex_unlock(&periph.lock);++cases;
    }
    for(unsigned old=0;old<256;old++) for(unsigned next=0;next<256;next++) for(unsigned fail=0;fail<3;fail++) {
        periph.synced[IRQ_EN_SET]=old;periph.cached[IRQ_EN_SET]=next;map.regs[0x1215]=old;
        write_calls=0;fail_write=fail;i2c_pmic_sync_enable(&chip,&periph);
        assert(periph.synced[IRQ_EN_SET]==map.regs[0x1215]);
        if(!fail) assert(map.regs[0x1215]==next);
        ++cases;
    }
    /* Partial polarity writes keep affected interrupts disabled and retry accurately. */
    for(unsigned fail=1;fail<5;fail++) {
        memset(periph.cached,0,sizeof(periph.cached));memset(periph.synced,0,sizeof(periph.synced));
        periph.synced[IRQ_EN_SET]=periph.cached[IRQ_EN_SET]=3;map.regs[0x1215]=3;
        periph.cached[IRQ_SET_TYPE]=periph.cached[IRQ_POL_HIGH]=periph.cached[IRQ_POL_LOW]=1;
        write_calls=0;fail_write=fail;irq.hwirq=0x1201;
        i2c_pmic_irq_bus_lock(&irq);i2c_pmic_irq_bus_sync_unlock(&irq);
        assert(periph.synced[IRQ_EN_SET]==map.regs[0x1215]);
        if(fail>1) assert(!(map.regs[0x1215]&1));
        write_calls=0;fail_write=0;i2c_pmic_irq_bus_lock(&irq);i2c_pmic_irq_bus_sync_unlock(&irq);
        assert(map.regs[0x1215]==3 && periph.synced[IRQ_SET_TYPE]==1);++cases;
    }
    write_calls=0;fail_write=0;periph.synced[IRQ_EN_SET]=3;periph.cached[IRQ_EN_SET]=1;
    map.regs[0x1215]=3;i2c_pmic_irq_disable_now(&chip,0x1202);assert(periph.synced[IRQ_EN_SET]==1);++cases;
    /* IRQ specifier and mapping validation. */
    domain=(struct irq_domain){.host_data=&chip,.live=true};unsigned long hw;unsigned type;
    for(unsigned p=0;p<258;p++) for(unsigned bit=0;bit<10;bit++) {
        u32 spec[3]={p,bit,1};int rc=i2c_pmic_domain_xlate(&domain,NULL,spec,3,&hw,&type);
        assert((rc==0)==(p<=255 && bit<8)); if(!rc) assert(hw==(p<<8|BIT(bit)) && type==1);++cases;
    }
    assert(i2c_pmic_domain_map(&domain,1,0x1201)==0);
    assert(i2c_pmic_domain_map(&domain,1,0x1203)==-ENODEV);
    assert(i2c_pmic_domain_map(&domain,1,0x11201)==-ENODEV);
    domain.live=false;cases+=3;
    /* Retry budget and no hardware register writes during plain reads. */
    for(unsigned n=0;n<6;n++) {
        read_calls=0;connect_errors=n;u8 buf[5];int rc=i2c_pmic_read(&map,0x1211,buf,5);
        assert(read_calls==(n<4?n+1:4));assert((rc==0)==(n<4));++cases;
    }
    /* Suspend failures restore normal masks rather than silently hiding an earlier error. */
    periph.wake=2;periph.synced[IRQ_EN_SET]=5;client.dev.drvdata=&chip;
    for(unsigned fail=1;fail<3;fail++) {
        write_calls=0;fail_write=fail;chip.resume_completed=true;map.regs[0x1215]=5;
        assert(i2c_pmic_suspend(&client.dev)==-EIO && chip.resume_completed && map.regs[0x1215]==5);++cases;
    }
    write_calls=0;fail_write=0;assert(!i2c_pmic_suspend(&client.dev) && !chip.resume_completed && map.regs[0x1215]==2);
    domain=(struct irq_domain){.host_data=&chip,.live=true};chip.domain=&domain;
    assert(i2c_pmic_irq_handler(21,&chip)==IRQ_HANDLED && irq_disable_depth==1 && chip.irq_waiting);
    assert(i2c_pmic_irq_handler(21,&chip)==IRQ_HANDLED && irq_disable_depth==1);
    assert(i2c_pmic_suspend_noirq(&client.dev)==-EBUSY);
    write_calls=0;assert(!i2c_pmic_resume(&client.dev) && !irq_disable_depth && chip.resume_completed && !chip.irq_waiting && map.regs[0x1215]==5);
    domain.live=false;chip.domain=NULL;++cases;
    /* Native optional IRQ toggle self-test restores settings on every failed write. */
    chip.toggle_stat=true;
    for(unsigned fail=0;fail<=20;fail++) {
        write_calls=read_calls=0;fail_write=fail;fail_read=0;
        map.regs[0x12e0]=3;map.regs[0x12e1]=4;map.regs[0x1215]=periph.synced[IRQ_EN_SET]=2;
        int rc=i2c_pmic_toggle_stat(&chip);
        if(fail<18) assert((rc==0)==(fail==0));
        if(fail<18) assert(map.regs[0x12e0]==3 && map.regs[0x12e1]==4 && map.regs[0x1215]==2);
        ++cases;
    }
    printf("%u\n",cases);
}
