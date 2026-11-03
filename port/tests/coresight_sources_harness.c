#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int raw_spinlock_t;
enum cs_mode { CS_MODE_DISABLED, CS_MODE_SYSFS, CS_MODE_PERF };
struct mutex { int held; };
struct device_node { bool source, sink, atid, csr; int count; u32 values[8], num; int read_error; const char *name; u32 offset; };
struct device { struct device *parent; struct device_node *of_node; void *data, *platform_data; };
struct platform_device { struct device dev; };
struct fwnode_handle { bool qmi; };
struct coresight_device;
struct coresight_connection { struct coresight_device *dest_dev; struct fwnode_handle *dest_fwnode; };
struct coresight_platform_data { int nr_outconns; struct coresight_connection **out_conns; };
struct coresight_device { struct device dev; struct coresight_platform_data *pdata; int type; struct { int helper_subtype; } subtype; enum cs_mode mode; bool qmi; };
struct coresight_path { struct coresight_device *sink; int trace_id; };
struct perf_event { int unused; };
struct coresight_desc { const char *name; int type; struct { int source_subtype, sink_subtype; } subtype; void *ops, *groups; struct coresight_platform_data *pdata; struct device *dev; };
#define IS_VALID_CS_TRACE_ID(id) ((id) > 0 && (id) < 0x70)
#define GFP_KERNEL 0
#define IS_ENABLED(x) 1
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define EPROBE_DEFER 517
#define CORESIGHT_DEV_TYPE_SOURCE 1
#define CORESIGHT_DEV_TYPE_SINK 2
#define CORESIGHT_DEV_TYPE_HELPER 4
#define CORESIGHT_DEV_SUBTYPE_SOURCE_OTHERS 4
#define CORESIGHT_DEV_SUBTYPE_SOURCE_SOFTWARE 2
#define CORESIGHT_DEV_SUBTYPE_SINK_DUMMY 6
#define CORESIGHT_DEV_SUBTYPE_HELPER_QCOM_CSR 3
#define __iomem
#define BIT(n) (1U << (n))
#define raw_spin_lock_irqsave(lock, flags) do { (flags) = 0; assert(!*(lock)); *(lock) = 1; } while (0)
#define raw_spin_unlock_irqrestore(lock, flags) do { (void)(flags); assert(*(lock)); *(lock) = 0; } while (0)
#define CS_UNLOCK(base) ((void)(base))
#define CS_LOCK(base) ((void)(base))
struct coresight_atid_assign_req_msg_v01 { char name[25]; u8 atids[8]; u8 num_atids; };
/* INSERT_DECLARATIONS */
static unsigned int cases;
static u32 mmio[1024];
static bool used_ids[112];
static int claim_error, claimed, csr_claims, csr_disclaims;
static int qmi_assign_error, qmi_enable_error, qmi_disable_error, qmi_assigns, qmi_enables, qmi_disables;
static int alloc_count, allocation_attempts, fail_alloc, id_attempts, fail_id, register_error, pm_users, unregisters;
static void *allocations[32];
static struct coresight_atid_assign_req_msg_v01 sent_atids;
static struct csr_drvdata csr;
static struct coresight_device registered, qmi, sink, csr_cs;
static struct device qmi_dev, sink_dev, csr_dev;
static struct device_node node, sink_node;
static struct fwnode_handle qmi_node = {.qmi = true};
static struct coresight_connection source_qmi = {.dest_dev = &qmi, .dest_fwnode = &qmi_node};
static struct coresight_connection sink_csr = {.dest_dev = &csr_cs};
static struct coresight_connection *source_conns[] = {&source_qmi}, *sink_conns[] = {&sink_csr};
static struct coresight_platform_data source_pdata = {1, source_conns}, sink_pdata = {1, sink_conns};
static struct coresight_path path = {.sink = &sink, .trace_id = 36};
static struct platform_device pdev;
static int source_devs, sink_devs, remote_etm_devs, dummy_source_cs_ops, dummy_sink_cs_ops, remote_cs_ops;
static void *dummy_source_attr_grps, *remote_etm_attr_grps;
static int boot_enable;
#define CHECK(x) do { assert(x); cases++; } while (0)
#define dev_err(...) ((void)0)
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_dbg(...) ((void)0)
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static void *platform_get_drvdata(struct platform_device *pdev) { return pdev->dev.data; }
static void platform_set_drvdata(struct platform_device *pdev, void *data) { pdev->dev.data = data; }
static void mutex_init(struct mutex *mutex) { mutex->held = 0; }
static void mutex_lock(struct mutex *mutex) { assert(!mutex->held); mutex->held = 1; }
static void mutex_unlock(struct mutex *mutex) { assert(mutex->held); mutex->held = 0; }
static bool coresight_is_qmi_device(struct coresight_device *csdev) { return csdev->qmi; }
static int fwnode_property_match_string(struct fwnode_handle *f, const char *key, const char *value) { return f && f->qmi ? 0 : -EINVAL; }
static bool device_property_present(struct device *dev, const char *key) { return !strcmp(key, "atid") ? dev->of_node->atid : dev->of_node->csr; }
static int device_property_read_string(struct device *dev, const char *key, const char **value) { *value = dev->of_node->name; return *value ? 0 : -EINVAL; }
static int device_property_read_u32(struct device *dev, const char *key, u32 *value) {
    *value = !strcmp(key, "atid") ? dev->of_node->values[0] : !strcmp(key, "qcom,atid-num") ? dev->of_node->num : dev->of_node->offset;
    return dev->of_node->read_error;
}
static int of_property_count_u32_elems(struct device_node *node, const char *key) { return node->count; }
static int of_property_read_u32_array(struct device_node *node, const char *key, u32 *values, int count) {
    assert(count <= 8); memcpy(values, node->values, count * sizeof(*values)); return node->read_error;
}
static bool of_device_is_compatible(struct device_node *node, const char *compatible) {
    return strstr(compatible, "dummy-source") ? node->source : node->sink;
}
static int strscpy(char *dest, const char *source, size_t capacity) {
    if (strlen(source) >= capacity) return -E2BIG;
    strcpy(dest, source); return strlen(dest);
}
static int coresight_qmi_assign_atid(struct coresight_device *dev, struct coresight_atid_assign_req_msg_v01 *req) { qmi_assigns++; sent_atids = *req; return qmi_assign_error; }
static int coresight_qmi_remote_etm_enable(struct coresight_device *dev) { qmi_enables++; return qmi_enable_error; }
static int coresight_qmi_remote_etm_disable(struct coresight_device *dev) { qmi_disables++; return qmi_disable_error; }
static bool coresight_take_mode(struct coresight_device *dev, enum cs_mode mode) { if (dev->mode != CS_MODE_DISABLED) return false; dev->mode = mode; return true; }
static enum cs_mode coresight_get_mode(struct coresight_device *dev) { return dev->mode; }
static void coresight_set_mode(struct coresight_device *dev, enum cs_mode mode) { dev->mode = mode; }
static int coresight_trace_id_get_system_id(void) {
    if (++id_attempts == fail_id) return -ENOSPC;
    for (int id = 1; id < 112; id++) if (!used_ids[id]) { used_ids[id] = true; return id; }
    return -ENOSPC;
}
static int coresight_trace_id_get_static_system_id(int id) { if (used_ids[id]) return -EBUSY; used_ids[id] = true; return id; }
static void coresight_trace_id_put_system_id(int id) { assert(id > 0 && id < 112 && used_ids[id]); used_ids[id] = false; }
static u32 readl_relaxed(void *address) { assert(address >= (void *)mmio && address < (void *)(mmio + 1024) && (uintptr_t)address % 4 == 0); return *(u32 *)address; }
static void writel_relaxed(u32 value, void *address) { assert(address >= (void *)mmio && address < (void *)(mmio + 1024) && (uintptr_t)address % 4 == 0); *(u32 *)address = value; }
static int coresight_claim_device(struct coresight_device *dev) { csr_claims++; if (claim_error) return claim_error; assert(!claimed); claimed = 1; return 0; }
static void coresight_disclaim_device(struct coresight_device *dev) { csr_disclaims++; assert(claimed); claimed = 0; }
static struct coresight_device *coresight_get_sink(struct coresight_path *path) { return path->sink; }
static void *devm_kcalloc(struct device *dev, size_t n, size_t size, int flags) { if (++allocation_attempts == fail_alloc) return NULL; void *p = calloc(n, size); allocations[alloc_count++] = p; return p; }
static void *devm_kzalloc(struct device *dev, size_t size, int flags) { return devm_kcalloc(dev, 1, size, flags); }
static const char *coresight_alloc_device_name(int *list, struct device *dev) { return "fallback"; }
static struct coresight_platform_data *coresight_get_platform_data(struct device *dev) { return &source_pdata; }
static void pm_runtime_enable(struct device *dev) { pm_users++; }
static void pm_runtime_disable(struct device *dev) { pm_users--; }
static struct coresight_device *coresight_register(struct coresight_desc *desc) { registered.dev.parent = desc->dev; registered.pdata = desc->pdata; registered.type = desc->type; return register_error ? ERR_PTR(register_error) : &registered; }
static void coresight_unregister(struct coresight_device *dev) { unregisters++; }
static void coresight_disable(struct coresight_device *dev);
static int coresight_enable(struct coresight_device *dev) { return -EIO; }
/* INSERT_FUNCTIONS */
static void coresight_disable(struct coresight_device *dev) {
    if (node.source) dummy_source_disable(dev, NULL); else remote_etm_disable(dev, NULL);
}
static void reset(void) {
    for (int i = 0; i < alloc_count; i++) free(allocations[i]);
    alloc_count = 0; memset(used_ids, 0, sizeof(used_ids)); memset(mmio, 0, sizeof(mmio)); memset(&csr, 0, sizeof(csr));
    memset(&registered, 0, sizeof(registered)); memset(&pdev, 0, sizeof(pdev)); memset(&node, 0, sizeof(node));
    node.name = "native-source"; pdev.dev.of_node = &node;
    sink_node = (struct device_node){.csr = true, .offset = CSR_ATID0}; sink_dev.of_node = &sink_node;
    sink.dev.parent = &sink_dev; sink.pdata = &sink_pdata;
    csr_cs.dev.parent = &csr_dev; csr_cs.type = CORESIGHT_DEV_TYPE_HELPER; csr_cs.subtype.helper_subtype = CORESIGHT_DEV_SUBTYPE_HELPER_QCOM_CSR;
    csr_dev.data = &csr; csr.base = mmio; csr.size = sizeof(mmio); csr.csdev = &csr_cs;
    qmi.qmi = true; qmi.dev.parent = &qmi_dev; source_qmi.dest_dev = &qmi; sink_csr.dest_dev = &csr_cs;
    claim_error = claimed = csr_claims = csr_disclaims = qmi_assign_error = qmi_enable_error = qmi_disable_error = 0;
    qmi_assigns = qmi_enables = qmi_disables = allocation_attempts = fail_alloc = id_attempts = fail_id = register_error = pm_users = unregisters = 0;
    path.trace_id = 36; path.sink = &sink;
}
static struct remote_etm_drvdata *remote(bool fixed) {
    reset(); node.atid = fixed; node.count = 2; node.num = 2; node.values[0] = 36; node.values[1] = 37;
    CHECK(remote_etm_probe(&pdev) == 0); return pdev.dev.data;
}
int main(void) {
    reset(); u8 ids[] = {36, 37, 38};
    CHECK(qcom_csr_route_ids(&path, ids, 3, true) == 0);
    CHECK(csr.refs[0][36] == 0 && csr.refs[0][37] == 1 && csr.refs[0][38] == 1);
    CHECK(qcom_csr_route_ids(&path, ids, 3, true) == 0 && csr.refs[0][37] == 2);
    CHECK(qcom_csr_route_ids(&path, ids, 3, false) == 0 && csr.refs[0][37] == 1);
    CHECK(qcom_csr_route_ids(&path, ids, 3, false) == 0 && !csr.users && !claimed);
    CHECK(qcom_csr_route_ids(NULL, ids, 3, true) == -EINVAL);
    CHECK(qcom_csr_route_ids(&path, ids, 0, true) == -EINVAL);
    u8 bad[] = {36, 36}; CHECK(qcom_csr_route_ids(&path, bad, 2, true) == -EINVAL);
    bad[1] = 112; CHECK(qcom_csr_route_ids(&path, bad, 2, true) == -EINVAL);
    sink_csr.dest_dev = NULL; CHECK(qcom_csr_route_ids(&path, ids, 3, true) == -ENODEV);
    sink_node.csr = false; CHECK(qcom_csr_route_ids(&path, ids, 3, true) == 0);
    reset(); csr.users = 1; csr.refs[0][38] = UINT_MAX; claimed = 1;
    CHECK(qcom_csr_route_ids(&path, ids, 3, true) == -EOVERFLOW);
    CHECK(!csr.refs[0][37] && csr.users == 1 && csr.refs[0][38] == UINT_MAX);
    reset(); mmio[(CSR_ATID0 + 4) / 4] = BIT(6);
    CHECK(qcom_csr_route_ids(&path, ids, 3, true) == 0);
    CHECK(qcom_csr_route_ids(&path, ids, 3, false) == 0);
    CHECK(mmio[(CSR_ATID0 + 4) / 4] == BIT(6));
    for (int n = 0; n <= 9; n++) {
        reset(); node.atid = true; node.count = n; for (int j = 0; j < 8; j++) node.values[j] = j + 1;
        struct remote_etm_drvdata d = {.dev = &pdev.dev};
        CHECK(remote_etm_get_traceid(&d) == (n > 0 && n <= 8 ? 0 : -EINVAL));
    }
    for (int id = 0; id <= 255; id++) {
        reset(); node.atid = true; node.count = 1; node.values[0] = id;
        struct remote_etm_drvdata d = {.dev = &pdev.dev}; CHECK(remote_etm_get_traceid(&d) == (id > 0 && id < 112 ? 0 : -EINVAL));
    }
    reset(); node.atid = true; node.count = 2; node.values[0] = node.values[1] = 36;
    struct remote_etm_drvdata d = {.dev = &pdev.dev}; CHECK(remote_etm_get_traceid(&d) == -EINVAL);
    reset(); node.num = 2; fail_id = 2; d = (struct remote_etm_drvdata){.dev = &pdev.dev};
    CHECK(remote_etm_get_traceid(&d) == -ENOSPC && !d.reserved && !used_ids[1]);
    struct remote_etm_drvdata *r = remote(true);
    CHECK(remote_etm_trace_id(&registered, CS_MODE_SYSFS, &sink) == 36);
    CHECK(remote_etm_enable(&registered, NULL, CS_MODE_SYSFS, &path) == 0 && r->reserved == 2 && used_ids[36] && used_ids[37]);
    CHECK(remote_etm_enable(&registered, NULL, CS_MODE_SYSFS, &path) == -EBUSY);
    remote_etm_disable(&registered, NULL); CHECK(!r->reserved && !used_ids[36] && !used_ids[37] && !csr.users);
    r = remote(true); used_ids[37] = true;
    CHECK(remote_etm_enable(&registered, NULL, CS_MODE_SYSFS, &path) == -EBUSY && !used_ids[36] && used_ids[37] && !r->reserved && !qmi_enables);
    r = remote(true); qmi_enable_error = -EREMOTEIO;
    CHECK(remote_etm_enable(&registered, NULL, CS_MODE_SYSFS, &path) == -EREMOTEIO && !r->reserved && !csr.users);
    r = remote(true); qmi_enable_error = -ETIMEDOUT; qmi_disable_error = -EIO;
    CHECK(remote_etm_enable(&registered, NULL, CS_MODE_SYSFS, &path) == -ETIMEDOUT && r->reserved == 2 && r->stop_failed && !csr.users);
    CHECK(remote_etm_enable(&registered, NULL, CS_MODE_SYSFS, &path) == -EIO && r->reserved == 2);
    qmi_enable_error = qmi_disable_error = 0;
    CHECK(remote_etm_enable(&registered, NULL, CS_MODE_SYSFS, &path) == 0 && r->reserved == 2 && !r->stop_failed);
    qmi_disable_error = -EIO; remote_etm_disable(&registered, NULL);
    CHECK(r->reserved == 2 && r->stop_failed && !csr.users && registered.mode == CS_MODE_DISABLED);
    qmi_disable_error = 0; CHECK(remote_etm_enable(&registered, NULL, CS_MODE_SYSFS, &path) == 0);
    qmi_disable_error = -EIO; CHECK(remote_etm_suspend(&pdev.dev) == -EIO && !r->suspended && r->reserved == 2);
    qmi_disable_error = 0; CHECK(remote_etm_suspend(&pdev.dev) == 0 && r->suspended && r->reserved == 2 && csr.users == 1);
    qmi_enable_error = -EIO; CHECK(remote_etm_resume(&pdev.dev) == -EIO && r->suspended);
    qmi_enable_error = 0; CHECK(remote_etm_resume(&pdev.dev) == 0 && !r->suspended);
    remote_etm_remove(&pdev); CHECK(!used_ids[36] && !used_ids[37] && !csr.users && !pm_users && unregisters == 1);
    r = remote(false); path.trace_id = r->traceids[0];
    CHECK(remote_etm_enable(&registered, NULL, CS_MODE_SYSFS, &path) == 0 && qmi_assigns == 1 && sent_atids.num_atids == 2);
    CHECK(sent_atids.atids[0] == r->traceids[0] && sent_atids.atids[1] == r->traceids[1]);
    remote_etm_disable(&registered, NULL); CHECK(r->reserved == 2 && used_ids[r->traceids[0]]);
    remote_etm_remove(&pdev); CHECK(!used_ids[1] && !used_ids[2]);
    for (int failure = 1; failure <= 3; failure++) {
        reset(); node.num = 2;
        if (failure <= 2) fail_alloc = failure; else register_error = -EPROBE_DEFER;
        CHECK(remote_etm_probe(&pdev) == (failure <= 2 ? -ENOMEM : -EPROBE_DEFER));
        CHECK(!used_ids[1] && !used_ids[2] && !pm_users);
    }
    reset(); node.source = true; node.atid = true; node.values[0] = 50;
    CHECK(dummy_probe(&pdev) == 0 && used_ids[50]);
    struct dummy_drvdata *dummy = pdev.dev.data;
    CHECK(dummy_trace_id(&registered, CS_MODE_SYSFS, &sink) == 50);
    CHECK(dummy_source_enable(&registered, NULL, CS_MODE_SYSFS, &path) == 0 && dummy->enabled && !qmi_assigns);
    CHECK(dummy_source_enable(&registered, NULL, CS_MODE_SYSFS, &path) == -EBUSY);
    dummy_remove(&pdev); CHECK(!used_ids[50] && !pm_users && !dummy->id_owned);
    reset(); node.source = true;
    CHECK(dummy_probe(&pdev) == 0); dummy = pdev.dev.data;
    qmi_assign_error = -EREMOTEIO;
    CHECK(dummy_source_enable(&registered, NULL, CS_MODE_SYSFS, &path) == -EREMOTEIO && !dummy->enabled && registered.mode == CS_MODE_DISABLED);
    qmi_assign_error = 0; source_qmi.dest_dev = NULL;
    CHECK(dummy_source_enable(&registered, NULL, CS_MODE_SYSFS, &path) == -EPROBE_DEFER);
    source_qmi.dest_dev = &qmi; node.name = "a-name-too-long-for-the-native-QMI-field";
    CHECK(dummy_source_enable(&registered, NULL, CS_MODE_SYSFS, &path) == -E2BIG);
    node.name = "trace"; CHECK(dummy_source_enable(&registered, NULL, CS_MODE_SYSFS, &path) == 0);
    dummy_source_disable(&registered, NULL); CHECK(!dummy->enabled && used_ids[dummy->traceid]);
    dummy_remove(&pdev); CHECK(!used_ids[1]);
    reset(); node.source = true; register_error = -EPROBE_DEFER;
    CHECK(dummy_probe(&pdev) == -EPROBE_DEFER && !used_ids[1] && !pm_users);
    reset(); node.sink = true; CHECK(dummy_probe(&pdev) == 0 && !used_ids[1]);
    CHECK(dummy_sink_enable(&registered, CS_MODE_SYSFS, NULL) == 0);
    CHECK(dummy_sink_enable(&registered, CS_MODE_SYSFS, NULL) == 0);
    CHECK(dummy_sink_enable(&registered, CS_MODE_PERF, NULL) == -EBUSY);
    CHECK(dummy_sink_disable(&registered) == 0 && registered.mode == CS_MODE_SYSFS);
    CHECK(dummy_sink_disable(&registered) == 0 && registered.mode == CS_MODE_DISABLED);
    CHECK(dummy_sink_disable(&registered) == -EINVAL); dummy_remove(&pdev);
    reset(); printf("%u\n", cases);
    return 0;
}
