#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef size_t resource_size_t;
enum cs_mode { CS_MODE_SYSFS };
#define __iomem
#define U32_MAX UINT32_MAX
#define CORESIGHT_LAR 0xfb0
#define CORESIGHT_UNLOCK 0xc5acce55
#define GFP_KERNEL 0
#define EPROBE_DEFER 517
struct mutex { int held; };
struct device { struct device *parent; struct tgu_drvdata *data; void *of_node, *platform_data; int kobj; };
struct device_attribute { int unused; };
struct coresight_device { struct device dev; };
struct resource { size_t size; };
struct amba_device { struct device dev; struct resource res; };
struct amba_id { int unused; };
struct coresight_platform_data { int unused; };
struct coresight_desc { const char *name; int type; struct { int helper_subtype; } subtype; void *ops, *access, *groups; struct device *dev; void *pdata; };
#define CSDEV_ACCESS_IOMEM(base) (base)
#define CORESIGHT_DEV_TYPE_HELPER 4
#define CORESIGHT_DEV_SUBTYPE_HELPER_QCOM_TGU 5
static u32 mmio[1024];
static int pm_votes, pm_gets, pm_puts, pm_error, claimed, claims, disclaims, claim_error, writes;
static unsigned int cases;
#define CHECK(x) do { assert(x); cases++; } while (0)
static void __raw_writel(u32 val, void *addr) {
    assert(addr >= (void *)mmio && addr < (void *)(mmio + 1024) && (uintptr_t)addr % 4 == 0);
    assert(pm_votes > 0 && (claimed || addr == (void *)((char *)mmio + CORESIGHT_LAR))); writes++; *(u32 *)addr = val;
}
#define mb() ((void)0)
/* INSERT_DECLARATIONS */
static struct amba_device adev;
static struct coresight_device cs;
static struct coresight_platform_data pdata;
static int tgu_devs, tgu_ops;
static void *tgu_attr_grps;
static int alloc_attempt, fail_alloc, map_error, property_error, register_error, unregisters, groups_removed;
static int steps = 3, conditions = 4, regs = 9, timers = 8;
static void *allocations[16];
static int allocation_count;
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static void dev_set_drvdata(struct device *dev, struct tgu_drvdata *d) { dev->data = d; }
static void mutex_init(struct mutex *m) { m->held = 0; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = 1; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = 0; }
static int pm_runtime_resume_and_get(struct device *dev) { pm_gets++; if (pm_error) return pm_error; pm_votes++; return 0; }
static void pm_runtime_put(struct device *dev) { pm_puts++; assert(pm_votes > 0); pm_votes--; }
static int coresight_claim_device(struct coresight_device *dev) { claims++; if (claim_error) return claim_error; assert(!claimed); claimed = 1; return 0; }
static void coresight_disclaim_device_unlocked(struct coresight_device *dev) { assert(claimed); claimed = 0; disclaims++; }
static int kstrtoul(const char *str, int base, unsigned long *out) { char *end; errno = 0; *out = strtoul(str, &end, base); return errno || end == str || (*end && *end != '\n') ? -EINVAL : 0; }
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define dev_dbg(...) ((void)0)
static const char *coresight_alloc_device_name(int *list, struct device *dev) { return "tgu0"; }
static int device_property_read_string(struct device *dev, const char *name, const char **out) { *out = "native-tgu"; return 0; }
static struct coresight_platform_data *coresight_get_platform_data(struct device *dev) { return &pdata; }
static void *devm_kzalloc(struct device *dev, size_t size, int flags) {
    if (++alloc_attempt == fail_alloc) return NULL;
    void *p = calloc(1, size); allocations[allocation_count++] = p; return p;
}
static void *devm_ioremap_resource(struct device *dev, struct resource *res) { return map_error ? (void *)(intptr_t)map_error : mmio; }
static size_t resource_size(struct resource *res) { return res->size; }
static int of_property_read_u32(void *node, const char *name, int *out) {
    *out = !strcmp(name, "tgu-steps") ? steps : !strcmp(name, "tgu-conditions") ? conditions : !strcmp(name, "tgu-regs") ? regs : timers;
    return property_error;
}
static struct coresight_device *coresight_register(struct coresight_desc *desc) { cs.dev.parent = desc->dev; return register_error ? (void *)(intptr_t)register_error : &cs; }
static void coresight_unregister(struct coresight_device *dev) { unregisters++; }
static void sysfs_remove_groups(int *kobj, void *groups) { groups_removed++; }
/* INSERT_FUNCTIONS */
static void reset(void) {
    for (int i = 0; i < allocation_count; i++) free(allocations[i]);
    allocation_count = 0;
    memset(&adev, 0, sizeof(adev)); memset(mmio, 0, sizeof(mmio));
    pm_votes = 1; pm_gets = pm_puts = pm_error = claimed = claims = disclaims = claim_error = writes = 0;
    alloc_attempt = fail_alloc = map_error = property_error = register_error = unregisters = groups_removed = 0;
    steps = 3; conditions = 4; regs = 9; timers = 8; adev.res.size = sizeof(mmio);
}
static void initialized(void) { reset(); CHECK(tgu_probe(&adev, NULL) == 0); CHECK(pm_votes == 0); }
int main(void) {
    CHECK(tgu_path_enable(&cs, CS_MODE_SYSFS, NULL) == -EOPNOTSUPP);
    CHECK(tgu_path_disable(&cs, NULL) == -EOPNOTSUPP);
    for (int fail = 1; fail <= 6; fail++) {
        reset(); fail_alloc = fail; CHECK(tgu_probe(&adev, NULL) == -ENOMEM); CHECK(!unregisters);
    }
    reset(); map_error = -EIO; CHECK(tgu_probe(&adev, NULL) == -EIO);
    reset(); property_error = -EINVAL; CHECK(tgu_probe(&adev, NULL) == -EINVAL);
    reset(); register_error = -EPROBE_DEFER; CHECK(tgu_probe(&adev, NULL) == -EPROBE_DEFER);
    for (int step = 0; step <= 9; step++) {
        for (int cond = 0; cond <= 5; cond++) {
            for (int reg = 0; reg <= 25; reg++) {
                struct tgu_drvdata d = {.size = 4096, .max_steps = step, .max_conditions = cond, .max_regs = reg, .max_timer_counter = 8};
                bool valid = step > 0 && step <= 8 && cond > 0 && cond <= 4 && reg > 0 && reg <= 24;
                if (valid) valid = GROUP_REG_STEP(3, reg - 1, step - 1) < 0xf00;
                CHECK((tgu_validate_layout(&d) == 0) == valid);
            }
        }
    }
    initialized(); struct tgu_drvdata *d = adev.dev.data;
    CHECK(set_group_store(&cs.dev, NULL, "-1 0 0 1", 8) == -EINVAL);
    CHECK(set_group_store(&cs.dev, NULL, "4 0 0 1", 7) == -EINVAL);
    CHECK(set_group_store(&cs.dev, NULL, "0 9 0 1", 7) == -EINVAL);
    CHECK(set_group_store(&cs.dev, NULL, "0 0 3 1", 7) == -EINVAL);
    CHECK(set_group_store(&cs.dev, NULL, "0 0 0 100000000", 15) == -EINVAL);
    for (int i = 0; i < MAX_GROUP_SETS; i++) CHECK(set_group_store(&cs.dev, NULL, "3 8 2 55", 8) == 8);
    CHECK(set_group_store(&cs.dev, NULL, "0 0 0 1", 7) == -ENOSPC);
    for (int i = 0; i < MAX_CONDITION_SETS; i++) {
        CHECK(set_condition_store(&cs.dev, NULL, "3 2 aa", 6) == 6);
        CHECK(set_select_store(&cs.dev, NULL, "3 2 bb", 6) == 6);
    }
    CHECK(set_condition_store(&cs.dev, NULL, "3 2 aa", 6) == -ENOSPC);
    CHECK(set_select_store(&cs.dev, NULL, "3 2 bb", 6) == -ENOSPC);
    CHECK(set_condition_store(&cs.dev, NULL, "-1 0 1", 6) == -EINVAL);
    CHECK(set_condition_store(&cs.dev, NULL, "4 0 1", 5) == -EINVAL);
    CHECK(set_select_store(&cs.dev, NULL, "0 -1 1", 6) == -EINVAL);
    for (int i = 0; i < MAX_TIMER_COUNTER_SETS; i++) {
        CHECK(set_timer_store(&cs.dev, NULL, "7 cc", 4) == 4);
        CHECK(set_counter_store(&cs.dev, NULL, "7 dd", 4) == 4);
    }
    CHECK(set_timer_store(&cs.dev, NULL, "7 cc", 4) == -ENOSPC);
    CHECK(set_counter_store(&cs.dev, NULL, "7 dd", 4) == -ENOSPC);
    CHECK(set_timer_store(&cs.dev, NULL, "8 1", 3) == -EINVAL);
    CHECK(set_counter_store(&cs.dev, NULL, "-1 1", 4) == -EINVAL);
    pm_error = -EIO; CHECK(enable_tgu_store(&cs.dev, NULL, "1", 1) == -EIO && !d->enable && !pm_votes && !writes);
    pm_error = 0; claim_error = -EBUSY; CHECK(enable_tgu_store(&cs.dev, NULL, "1", 1) == -EBUSY && !pm_votes && !writes);
    claim_error = 0; CHECK(enable_tgu_store(&cs.dev, NULL, "1", 1) == 1 && d->enable && pm_votes == 1);
    CHECK(mmio[GROUP_REG_STEP(3, 8, 2) / 4] == 0x55);
    CHECK(mmio[CONDITION_DECODE_STEP(3, 2) / 4] == 0xaa);
    CHECK(mmio[CONDITION_SELECT_STEP(3, 2) / 4] == 0xbb);
    CHECK(mmio[TIMER0_COMPARE_STEP(7) / 4] == 0xcc);
    CHECK(mmio[COUNTER0_COMPARE_STEP(7) / 4] == 0xdd);
    int gets = pm_gets; CHECK(enable_tgu_store(&cs.dev, NULL, "1", 1) == 1 && pm_gets == gets && pm_votes == 1);
    CHECK(enable_tgu_store(&cs.dev, NULL, "0", 1) == 1 && !d->enable && !pm_votes && !claimed);
    CHECK(enable_tgu_store(&cs.dev, NULL, "0", 1) == 1 && !pm_votes);
    CHECK(enable_tgu_store(&cs.dev, NULL, "2", 1) == -EINVAL);
    CHECK(reset_tgu_store(&cs.dev, NULL, "0", 1) == -EINVAL);
    CHECK(reset_tgu_store(&cs.dev, NULL, "1", 1) == 1 && !d->grp_refcnt && !d->cond_refcnt && !pm_votes);
    CHECK(!d->grp_data[0].value && !d->counter_data[7].value);
    CHECK(enable_tgu_store(&cs.dev, NULL, "1", 1) == 1 && pm_votes == 1);
    CHECK(reset_tgu_store(&cs.dev, NULL, "1", 1) == 1 && !d->enable && !pm_votes);
    CHECK(enable_tgu_store(&cs.dev, NULL, "1", 1) == 1); tgu_remove(&adev);
    CHECK(!pm_votes && !claimed && d->removing && unregisters == 1 && groups_removed == 1);
    CHECK(set_group_store(&cs.dev, NULL, "0 0 0 1", 7) == -ENODEV);
    CHECK(enable_tgu_store(&cs.dev, NULL, "1", 1) == -ENODEV);
    CHECK(reset_tgu_store(&cs.dev, NULL, "1", 1) == -ENODEV);
    reset(); for (int i = 0; i < allocation_count; i++) free(allocations[i]);
    printf("%u\n", cases);
    return 0;
}
