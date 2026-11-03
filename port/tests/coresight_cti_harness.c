/* Run selected actual CTI functions against bounded MMIO and failing providers. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef size_t resource_size_t;
#define __iomem
#define BIT(n) (1U << (n))
#define GENMASK(h, l) ((UINT32_MAX >> (31 - (h))) & (UINT32_MAX << (l)))
#define BMVAL(v, l, h) (((v) & GENMASK(h, l)) >> (l))
#define CTI_DEVID_MAXTRIGS(v) BMVAL(v, 8, 15)
#define CTI_DEVID_CTMCHANNELS(v) BMVAL(v, 16, 19)
#define CORESIGHT_DEVID 0xfc8
#define GFP_KERNEL 0
#define CTI_DT_FILTER_OUT_SIGS "filters"
/* INSERT_CONSTANTS */
struct cti_config {
    int nr_ctm_channels, nr_trig_max, enable_req_count;
    bool hw_enabled, hw_powered, trig_filter_enable;
    unsigned long trig_in_use[2], trig_out_use[2], trig_out_filter[2];
    u8 xtrig_rchan_sel, ctiinout_sel;
    u32 ctiappset, ctiinen[128], ctiouten[128], ctigate, asicctl;
};
struct cti_drvdata;
struct device { struct device *parent; struct cti_drvdata *data; };
struct coresight_device { struct device dev; };
struct device_attribute { int unused; };
struct cti_pctrl { void *pctrl; int trig; };
struct cti_trig_con {
    struct coresight_device *con_dev;
    const char *con_dev_name, *con_node_name;
    struct cti_trig_con *next;
};
struct cti_device { struct cti_trig_con *trig_cons; int cpu; };
#define list_for_each_entry(item, head, node) for ((item) = *(head); (item); (item) = (item)->next)
struct cti_drvdata {
    void *base;
    struct coresight_device *csdev;
    int spinlock, gpio_mutex;
    struct cti_config config;
    struct cti_device ctidev;
    bool removing, extended_cti, atclk_enabled;
    resource_size_t regs_size;
    struct cti_pctrl *gpio_trigin, *gpio_trigout;
    void *atclk;
};
struct cti_trig_grp { int nr_sigs; unsigned long used_mask[2]; int sig_types[]; };
struct fwnode_handle { bool present; int count, read_error; u32 values[128]; };
enum cti_chan_op { CTI_CHAN_ATTACH, CTI_CHAN_DETACH };
enum cti_trig_dir { CTI_TRIG_IN, CTI_TRIG_OUT };
enum cti_chan_gate_op { CTI_GATE_CHAN_ENABLE, CTI_GATE_CHAN_DISABLE };
enum cti_chan_set_op { CTI_CHAN_SET, CTI_CHAN_CLR, CTI_CHAN_PULSE };
static u32 mmio[1024];
static int claims, disclaims, claim_error, clock_error, clock_enables, clock_disables;
static int gpio_error, gpio_enables, gpio_disables, register_error, assoc_published, force_error;
static bool link_success;
static bool cpu_is_online;
static int helpers, link_removes;
static const char *linked_name;
static unsigned int cases;
#define CHECK(x) do { assert(x); cases++; } while (0)
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
#define raw_spin_lock_irqsave(lock, flags) do { (flags) = 0; assert(!*(lock)); *(lock) = 1; } while (0)
#define raw_spin_unlock_irqrestore(lock, flags) do { (void)(flags); assert(*(lock)); *(lock) = 0; } while (0)
static void mutex_lock(int *lock) { assert(!*lock); *lock = 1; }
static void mutex_unlock(int *lock) { assert(*lock); *lock = 0; }
static u32 readl_relaxed(void *addr) {
    assert((uintptr_t)addr % 4 == 0 && addr >= (void *)mmio && addr < (void *)(mmio + 1024));
    return *(u32 *)addr;
}
static void writel_relaxed(u32 v, void *addr) {
    assert((uintptr_t)addr % 4 == 0 && addr >= (void *)mmio && addr < (void *)(mmio + 1024));
    *(u32 *)addr = v;
}
#define CS_UNLOCK(base) do { (void)(base); } while (0)
#define CS_LOCK(base) do { (void)(base); } while (0)
static int coresight_claim_device(struct coresight_device *dev) { claims++; return claim_error; }
static void coresight_disclaim_device_unlocked(struct coresight_device *dev) { disclaims++; }
static int clk_prepare_enable(void *clock) { clock_enables++; return clock_error; }
static void clk_disable_unprepare(void *clock) { clock_disables++; }
static int pm_runtime_force_suspend(struct device *dev) { return force_error; }
static int pm_runtime_force_resume(struct device *dev) { return force_error; }
static int cti_trigin_gpio_enable(struct cti_drvdata *d) { gpio_enables++; return gpio_error; }
static int cti_trigout_gpio_enable(struct cti_drvdata *d) { gpio_enables++; return gpio_error; }
static void cti_trigin_gpio_disable(struct cti_drvdata *d) { gpio_disables++; }
static void cti_trigout_gpio_disable(struct cti_drvdata *d) { gpio_disables++; }
static bool test_bit(u32 bit, const unsigned long *map) { assert(bit < 128); return map[bit / 64] & (1UL << (bit % 64)); }
static void set_bit(u32 bit, unsigned long *map) { assert(bit < 128); map[bit / 64] |= 1UL << (bit % 64); }
static void bitmap_zero(unsigned long *map, int bits) { memset(map, 0, (bits + 7) / 8); }
static void bitmap_or(unsigned long *dst, const unsigned long *a, const unsigned long *b, int bits) {
    for (int i = 0; i < (bits + 63) / 64; i++) dst[i] = a[i] | b[i];
}
static int kstrtoul(const char *str, int base, unsigned long *out) {
    char *end; errno = 0; unsigned long v = strtoul(str, &end, base);
    if (errno || end == str || (*end && *end != '\n')) return -EINVAL;
    *out = v; return 0;
}
static int kstrtou32(const char *str, int base, u32 *out) {
    unsigned long v; int ret = kstrtoul(str, base, &v);
    if (ret) return ret;
    if (v > UINT32_MAX) return -ERANGE;
    *out = v; return 0;
}
static bool fwnode_property_present(const struct fwnode_handle *f, const char *p) { return f->present; }
static int fwnode_property_count_u32(const struct fwnode_handle *f, const char *p) { return f->count; }
static int fwnode_property_read_u32_array(const struct fwnode_handle *f, const char *p, u32 *v, int n) {
    if (f->read_error) return f->read_error;
    memcpy(v, f->values, n * sizeof(*v)); return 0;
}
#define kcalloc(n, s, flags) calloc(n, s)
#define kzalloc(s, flags) calloc(1, s)
#define kfree(v) free(v)
static int cti_driver, cti_assoc_ops;
static void coresight_set_cti_ops(void *ops) { assoc_published = 1; }
static void coresight_remove_cti_ops(void) { assoc_published = 0; }
static int amba_driver_register(void *driver) { return register_error; }
static void cpus_read_lock(void) { }
static void cpus_read_unlock(void) { }
static bool cpu_online(int cpu) { return cpu_is_online; }
static bool cti_add_sysfs_link(struct cti_drvdata *drv, struct cti_trig_con *con) {
    linked_name = con->con_dev_name; return link_success;
}
static void coresight_add_helper(struct coresight_device *dev, struct coresight_device *helper) { helpers++; }
static void cti_remove_sysfs_link(struct cti_drvdata *drv, struct cti_trig_con *con) {
    assert(!strcmp(linked_name, con->con_dev_name)); link_removes++;
}
/* INSERT_FUNCTIONS */

static struct cti_drvdata d;
static struct coresight_device cs;
static struct device parent;
static struct cti_pctrl in, out;
static void reset(bool extended, int triggers, int channels) {
    memset(&d, 0, sizeof(d)); memset(mmio, 0, sizeof(mmio));
    in.trig = out.trig = -1;
    d.base = mmio; d.regs_size = sizeof(mmio); d.extended_cti = extended;
    d.ctidev.cpu = -1;
    d.csdev = &cs; d.gpio_trigin = &in; d.gpio_trigout = &out;
    parent.data = &d; cs.dev.parent = &parent;
    mmio[CORESIGHT_DEVID / 4] = (triggers << 8) | (channels << 16);
    claims = disclaims = claim_error = gpio_error = gpio_enables = gpio_disables = 0;
    clock_error = clock_enables = clock_disables = 0;
    force_error = 0;
}
static void initialized(bool ext, int triggers, int channels) {
    reset(ext, triggers, channels);
    CHECK(cti_set_default_config(&d) == 0);
    d.config.hw_powered = true;
    for (int i = 0; i < triggers; i++) {
        set_bit(i, d.config.trig_in_use); set_bit(i, d.config.trig_out_use);
    }
}
int main(void) {
    for (int ext = 0; ext < 2; ext++) {
        for (int trigs = 0; trigs <= 255; trigs++) {
            for (int chans = 0; chans < 16; chans++) {
                reset(ext, trigs, chans);
                bool valid = trigs && trigs <= (ext ? 128 : 32) && chans;
                CHECK((cti_set_default_config(&d) == 0) == valid);
            }
        }
        initialized(ext, ext ? 128 : 32, 4);
        d.regs_size = 4095; CHECK(cti_set_default_config(&d) == -EINVAL); d.regs_size = 4096;
        CHECK(cti_reg_valid(&d, 4092)); CHECK(!cti_reg_valid(&d, 4093)); CHECK(!cti_reg_valid(&d, 4096));
        int max = d.config.nr_trig_max;
        for (int trig = 0; trig <= max; trig++) {
            for (int chan = 0; chan <= 4; chan++) {
                int ret = cti_channel_trig_op(&cs.dev, CTI_CHAN_ATTACH, CTI_TRIG_IN, chan, trig);
                CHECK(ret == (trig < max && chan < 4 ? 0 : -EINVAL));
            }
        }
        CHECK(cti_channel_trig_op(&cs.dev, 9, CTI_TRIG_IN, 0, 0) == -EINVAL);
        CHECK(cti_channel_trig_op(&cs.dev, CTI_CHAN_ATTACH, 9, 0, 0) == -EINVAL);
        set_bit(max - 1, d.config.trig_out_filter);
        CHECK(cti_channel_trig_op(&cs.dev, CTI_CHAN_ATTACH, CTI_TRIG_OUT, 0, max - 1) == -EINVAL);
        char idx[20]; snprintf(idx, sizeof(idx), "%d", max - 1);
        CHECK(inout_sel_store(&cs.dev, NULL, idx, 2) == 2);
        snprintf(idx, sizeof(idx), "%d", max);
        CHECK(inout_sel_store(&cs.dev, NULL, idx, 2) == -EINVAL);
        CHECK(inen_store(&cs.dev, NULL, "16", 2) == -EINVAL);
        CHECK(outen_store(&cs.dev, NULL, "4294967296", 10) < 0);
        CHECK(inen_store(&cs.dev, NULL, "3", 1) == 1);
        CHECK(outen_store(&cs.dev, NULL, "5", 1) == 1);
        CHECK(gate_store(&cs.dev, NULL, "10", 2) == 2);
        CHECK(asicctl_store(&cs.dev, NULL, "19", 2) == 2);
        CHECK(appset_store(&cs.dev, NULL, "3", 1) == 1);
        CHECK(appset_store(&cs.dev, NULL, "4", 1) == 1);
        CHECK(d.config.ctiappset == 7);
        CHECK(cti_enable_hw(&d) == 0);
        CHECK(d.config.enable_req_count == 1 && d.config.hw_enabled);
        CHECK(mmio[(ext ? CTIINEN_EXTENDED(max - 1) : CTIINEN(max - 1)) / 4] == 3);
        CHECK(mmio[(ext ? CTIOUTEN_EXTENDED(max - 1) : CTIOUTEN(max - 1)) / 4] == 5);
        CHECK(mmio[(ext ? CTIGATE_EXTENDED : CTIGATE) / 4] == 10);
        CHECK(mmio[(ext ? ASICCTL_EXTENDED : ASICCTL) / 4] == 19);
        CHECK(appclear_store(&cs.dev, NULL, "2", 1) == 1);
        CHECK(mmio[(ext ? CTIAPPCLEAR_EXTENDED : CTIAPPCLEAR) / 4] == 2);
        CHECK(d.config.ctiappset == 5);
        CHECK(apppulse_store(&cs.dev, NULL, "1", 1) == 1);
        CHECK(d.config.ctiappset == 4);
        CHECK(cti_write_intack(&cs.dev, 1) == 0);
        CHECK(mmio[(ext ? CTIINTACK_EXTENDED(0) : CTIINTACK) / 4] == 1);
        CHECK(cti_enable_hw(&d) == 0 && d.config.enable_req_count == 2);
        CHECK(cti_suspend(&parent) == 0 && d.config.enable_req_count == 2 && !d.config.hw_enabled);
        CHECK(cti_resume(&parent) == 0 && d.config.enable_req_count == 2 && d.config.hw_enabled);
        CHECK(cti_disable_hw(&d) == 0 && d.config.hw_enabled);
        CHECK(cti_disable_hw(&d) == 0 && !d.config.hw_enabled);
        CHECK(cti_disable_hw(&d) == -EINVAL);
        CHECK(cti_write_intack(&cs.dev, 1) == -EHOSTDOWN);
        d.config.enable_req_count = INT_MAX;
        CHECK(cti_enable_hw(&d) == -EOVERFLOW);
        d.config.enable_req_count = 0; d.removing = true;
        CHECK(cti_enable_hw(&d) == -ENODEV);
        CHECK(cti_channel_trig_op(&cs.dev, CTI_CHAN_ATTACH, CTI_TRIG_IN, 0, 0) == -ENODEV);
        initialized(ext, ext ? 128 : 32, 4);
        claim_error = -EBUSY;
        CHECK(cti_enable_hw(&d) == (ext ? 0 : -EBUSY));
        CHECK(d.config.enable_req_count == (ext ? 1 : 0));
        initialized(ext, ext ? 128 : 32, 4);
        in.trig = 0; gpio_error = -EIO;
        CHECK(cti_channel_trig_op(&cs.dev, CTI_CHAN_ATTACH, CTI_TRIG_IN, 0, 0) == -EIO);
        CHECK(d.config.ctiinen[0] == 0);
        gpio_error = 0;
        CHECK(cti_channel_trig_op(&cs.dev, CTI_CHAN_ATTACH, CTI_TRIG_IN, 0, 0) == 0);
        CHECK(cti_channel_trig_op(&cs.dev, CTI_CHAN_ATTACH, CTI_TRIG_IN, 1, 0) == 0);
        CHECK(cti_channel_trig_op(&cs.dev, CTI_CHAN_DETACH, CTI_TRIG_IN, 0, 0) == 0 && !gpio_disables);
        CHECK(cti_channel_trig_op(&cs.dev, CTI_CHAN_DETACH, CTI_TRIG_IN, 1, 0) == 0 && gpio_disables == 1);
        CHECK(cti_channel_gate_op(&cs.dev, CTI_GATE_CHAN_DISABLE, 3) == 0);
        CHECK(cti_channel_gate_op(&cs.dev, 5, 0) == -EINVAL);
        CHECK(cti_channel_setop(&cs.dev, CTI_CHAN_SET, 3) == 0);
        CHECK(cti_channel_setop(&cs.dev, CTI_CHAN_CLR, 3) == 0);
        CHECK(cti_channel_setop(&cs.dev, CTI_CHAN_PULSE, 4) == -EINVAL);
    }
    for (int max = 1; max <= 128; max++) {
        struct cti_trig_grp group = { .nr_sigs = 1 };
        struct fwnode_handle f = { .present = true, .count = 1 };
        for (int bit = 0; bit <= 128; bit++) {
            memset(group.used_mask, 0, sizeof(group.used_mask)); f.values[0] = bit;
            CHECK(cti_plat_read_trig_group(&group, &f, "signals", max) == (bit < max ? 0 : -EINVAL));
        }
        initialized(true, max, 4); f.values[0] = max - 1;
        CHECK(cti_plat_process_filter_sigs(&d, &f) == 0);
        CHECK(test_bit(max - 1, d.config.trig_out_filter));
        f.count = -EIO; CHECK(cti_plat_process_filter_sigs(&d, &f) == -EINVAL);
        f.present = false; CHECK(cti_plat_count_sig_elements(&f, "signals") == 0);
        f.present = true; CHECK(cti_plat_count_sig_elements(&f, "signals") == -EIO);
    }
    struct cti_trig_grp group = { .nr_sigs = 2 };
    struct fwnode_handle f = { .present = true, .count = 2, .values = {127, 127} };
    CHECK(cti_plat_read_trig_group(&group, &f, "signals", 128) == -EINVAL);
    CHECK(!group.used_mask[0] && !group.used_mask[1]);
    f.read_error = -EIO; CHECK(cti_plat_read_trig_group(&group, &f, "signals", 128) == -EIO);
    initialized(true, 128, 4);
    clock_error = -EIO;
    CHECK(cti_runtime_resume(&parent) == -EIO && !d.atclk_enabled);
    clock_error = 0; CHECK(cti_runtime_resume(&parent) == 0 && d.atclk_enabled);
    CHECK(cti_runtime_suspend(&parent) == 0 && !d.atclk_enabled && clock_disables == 1);
    cti_disable_atclk(&d); CHECK(clock_disables == 1);
    CHECK(cti_enable_hw(&d) == 0);
    force_error = -EIO; CHECK(cti_suspend(&parent) == -EIO && d.config.hw_enabled && d.config.enable_req_count == 1);
    register_error = -EIO; CHECK(cti_init() == -EIO && !assoc_published);
    register_error = 0; CHECK(cti_init() == 0 && assoc_published);
    struct cti_trig_con connection = { .con_dev = &cs, .con_dev_name = "linked-device", .con_node_name = "firmware/node" };
    d.ctidev.trig_cons = &connection;
    link_success = true; cti_update_conn_xrefs(&d);
    CHECK(helpers == 1 && connection.con_dev == &cs);
    CHECK(!strcmp(connection.con_dev_name, "linked-device"));
    cti_remove_conn_xrefs(&d);
    CHECK(link_removes == 1 && !connection.con_dev);
    connection.con_dev = &cs; connection.con_dev_name = "linked-device";
    link_success = false; cti_update_conn_xrefs(&d);
    CHECK(helpers == 1 && !connection.con_dev);
    CHECK(!strcmp(connection.con_dev_name, connection.con_node_name));
    cti_remove_conn_xrefs(&d); CHECK(link_removes == 1);
    initialized(true, 128, 4);
    d.ctidev.cpu = 0; d.config.hw_powered = false; d.config.enable_req_count = 1;
    cpu_is_online = false; cti_restore_hw(&d);
    CHECK(!d.config.hw_powered && !d.config.hw_enabled && !mmio[CTICONTROL / 4]);
    cpu_is_online = true; cti_restore_hw(&d);
    CHECK(d.config.hw_powered && d.config.hw_enabled && d.config.enable_req_count == 1);
    printf("%u\n", cases);
    return 0;
}
