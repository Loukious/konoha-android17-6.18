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
struct mutex { int held; };
struct qmi_handle { int initialized; };
struct qmi_service { unsigned int node, port; };
struct qmi_txn { void *response; };
struct sockaddr_qrtr { unsigned int sq_family, sq_node, sq_port; };
struct qmi_response_type_v01 { uint16_t result, error; };
struct qmi_elem_info { int data_type, elem_len, elem_size, array_type, tlv_type; size_t offset; const struct qmi_elem_info *ei_array; };
static struct qmi_elem_info qmi_response_type_v01_ei[1];
enum { QMI_UNSIGNED_1_BYTE, QMI_UNSIGNED_4_BYTE, QMI_UNSIGNED_8_BYTE, QMI_SIGNED_4_BYTE_ENUM, QMI_STRING, QMI_STRUCT, QMI_EOTI, NO_ARRAY, STATIC_ARRAY };
struct device_driver { const char *name; };
struct device_node { int unused; };
struct qmi_drvdata;
struct device { struct device *parent; struct device_driver *driver; struct device_node *of_node; void *platform_data; struct qmi_drvdata *data; };
struct platform_device { struct device dev; };
struct coresight_platform_data { int unused; };
struct coresight_device { struct device dev; int type; };
struct coresight_desc { const char *name; int type; struct { int helper_subtype; } subtype; struct coresight_platform_data *pdata; struct device *dev; void *ops; };
#define CORESIGHT_QMI_DEFINE_TABLES
#define IS_ENABLED(x) 1
#define U64_MAX UINT64_MAX
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define CORESIGHT_DEV_TYPE_HELPER 4
#define CORESIGHT_DEV_SUBTYPE_HELPER_QCOM_QMI 4
#define QMI_RESULT_SUCCESS_V01 0
#define AF_QIPCRTR 42
#define GFP_KERNEL 0
#define EPROBE_DEFER 517
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
/* INSERT_HEADER */
static struct qmi_drvdata fixture;
static struct coresight_device cs;
static struct coresight_platform_data pdata;
static struct sockaddr_qrtr sent_to;
static unsigned int sent_id;
static size_t sent_max;
static unsigned char sent_request[256];
static int init_error, lookup_error, register_error, instance_error, alloc_error, txn_error, send_error, wait_error;
static int txn_inits, sends, waits, cancels, releases, unregisters, callback_while_wait;
static uint16_t remote_result;
static unsigned int cases;
static void *allocation;
#define CHECK(x) do { assert(x); cases++; } while (0)
#define raw_spin_lock_init(lock) (*(lock) = 0)
#define raw_spin_lock_irqsave(lock, flags) do { (flags) = 0; assert(!*(lock)); *(lock) = 1; } while (0)
#define raw_spin_unlock_irqrestore(lock, flags) do { (void)(flags); assert(*(lock)); *(lock) = 0; } while (0)
static void mutex_init(struct mutex *lock) { lock->held = 0; }
static void mutex_lock(struct mutex *lock) { assert(!lock->held); lock->held = 1; }
static void mutex_unlock(struct mutex *lock) { assert(lock->held); lock->held = 0; }
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static void *platform_get_drvdata(struct platform_device *pdev) { return pdev->dev.data; }
static void platform_set_drvdata(struct platform_device *pdev, struct qmi_drvdata *d) { pdev->dev.data = d; }
static int qmi_txn_init(struct qmi_handle *handle, struct qmi_txn *txn, const struct qmi_elem_info *ei, void *response) {
    txn_inits++; txn->response = response; return txn_error;
}
static int qmi_send_request(struct qmi_handle *handle, struct sockaddr_qrtr *addr, struct qmi_txn *txn,
                            unsigned int id, size_t maximum, const struct qmi_elem_info *ei, const void *req) {
    sends++; sent_to = *addr; sent_id = id; sent_max = maximum;
    size_t bytes = id == CORESIGHT_QMI_ETR_ASSIGN_REQ_V01 ? sizeof(struct coresight_etr_assign_req_msg_v01) :
        id == CORESIGHT_QMI_ATID_ASSIGN_V01 ? sizeof(struct coresight_atid_assign_req_msg_v01) : sizeof(struct coresight_set_etm_req_msg_v01);
    memcpy(sent_request, req, bytes); return send_error;
}
static void qmi_txn_cancel(struct qmi_txn *txn) { cancels++; }
static int qmi_txn_wait(struct qmi_txn *txn, int timeout);
#define msecs_to_jiffies(n) (n)
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
static int qmi_devs, qmi_ops, server_ops;
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
static const char *coresight_alloc_device_name(int *list, struct device *dev) { return "qmi0"; }
static int device_property_read_string(struct device *dev, const char *property, const char **out) { *out = "native-qmi"; return 0; }
static struct coresight_platform_data *coresight_get_platform_data(struct device *dev) { return &pdata; }
static void *devm_kzalloc(struct device *dev, size_t size, int flags) {
    if (alloc_error) return NULL;
    allocation = calloc(1, size); return allocation;
}
static int of_property_read_u32(struct device_node *node, const char *property, uint32_t *out) { *out = 13; return instance_error; }
static int qmi_handle_init(struct qmi_handle *handle, size_t max_len, void *ops, void *handlers) { handle->initialized = !init_error; return init_error; }
static int qmi_add_lookup(struct qmi_handle *handle, int service, int version, int instance) { return lookup_error; }
static struct coresight_device *coresight_register(struct coresight_desc *desc) {
    cs.dev.parent = desc->dev; cs.type = desc->type; return register_error ? (void *)(intptr_t)register_error : &cs;
}
static void coresight_unregister(struct coresight_device *dev) { unregisters++; }
static void qmi_handle_release(struct qmi_handle *handle) { releases++; handle->initialized = 0; }
/* INSERT_FUNCTIONS */
static int qmi_txn_wait(struct qmi_txn *txn, int timeout) {
    waits++; CHECK(timeout == TIMEOUT_MS);
    ((struct qmi_response_type_v01 *)txn->response)->result = remote_result;
    if (callback_while_wait) {
        struct qmi_service server = { .node = 8, .port = 9 };
        service_coresight_qmi_del_server(&fixture.handle, &server);
        service_coresight_qmi_new_server(&fixture.handle, &server);
    }
    return wait_error;
}
static struct platform_device pdev;
static void reset(void) {
    free(allocation); allocation = NULL;
    memset(&fixture, 0, sizeof(fixture)); memset(&pdev, 0, sizeof(pdev));
    fixture.dev = &pdev.dev; pdev.dev.data = &fixture; cs.dev.parent = &pdev.dev;
    init_error = lookup_error = register_error = instance_error = alloc_error = txn_error = send_error = wait_error = 0;
    txn_inits = sends = waits = cancels = releases = unregisters = callback_while_wait = 0;
    remote_result = 0;
}
static int call(int op) {
    struct coresight_etr_assign_req_msg_v01 etr = { .etr_id = 1, .subsys_id = 2, .buffer_base = 0x100000, .buffer_size = 0x1000 };
    struct coresight_atid_assign_req_msg_v01 atid = { .name = "remote", .atids = {38, 39}, .num_atids = 2 };
    if (op == 0) return coresight_qmi_remote_etm_enable(&cs);
    if (op == 1) return coresight_qmi_remote_etm_disable(&cs);
    if (op == 2) return coresight_qmi_etr_assign(&cs, &etr);
    return coresight_qmi_assign_atid(&cs, &atid);
}
int main(void) {
    for (int op = 0; op < 4; op++) {
        for (int failure = 0; failure < 7; failure++) {
            reset(); fixture.service_connected = true;
            fixture.s_addr = (struct sockaddr_qrtr){AF_QIPCRTR, 8, 9};
            if (failure == 1) fixture.service_connected = false;
            if (failure == 2) fixture.removing = true;
            if (failure == 3) txn_error = -ENOMEM;
            if (failure == 4) send_error = -EIO;
            if (failure == 5) wait_error = -ETIMEDOUT;
            if (failure == 6) remote_result = 1;
            int expected[] = {0, -ENOTCONN, -ENODEV, -ENOMEM, -EIO, -ETIMEDOUT, -EREMOTEIO};
            CHECK(call(op) == expected[failure]);
            CHECK(cancels == (failure == 4));
            CHECK(!fixture.mutex.held && !fixture.server_lock);
            if (!failure) {
                CHECK(sent_to.sq_node == 8 && sent_to.sq_port == 9);
                CHECK(sent_id == (op < 2 ? 0x2c : op == 2 ? 0x42 : 0x44));
                CHECK(sent_max == (op < 2 ? 7 : op == 2 ? 36 : 34));
                if (op < 2) CHECK(((struct coresight_set_etm_req_msg_v01 *)sent_request)->state == (op == 0));
            }
        }
    }
    reset();
    struct qmi_service first = {.node = 8, .port = 9}, second = {.node = 10, .port = 11};
    service_coresight_qmi_new_server(&fixture.handle, &first);
    service_coresight_qmi_new_server(&fixture.handle, &second);
    service_coresight_qmi_del_server(&fixture.handle, &first);
    CHECK(fixture.service_connected && fixture.s_addr.sq_port == 11);
    service_coresight_qmi_del_server(&fixture.handle, &second); CHECK(!fixture.service_connected);
    fixture.removing = true; service_coresight_qmi_new_server(&fixture.handle, &first); CHECK(!fixture.service_connected);
    reset(); fixture.service_connected = true; callback_while_wait = 1;
    CHECK(call(0) == 0 && fixture.service_connected);
    struct coresight_atid_assign_req_msg_v01 req = {.name = "test"};
    for (int n = 0; n < 256; n++) {
        req.num_atids = n; for (int j = 0; j < 8; j++) req.atids[j] = j + 1;
        CHECK(coresight_qmi_assign_atid(&cs, &req) == (n && n <= 8 ? 0 : -EINVAL));
    }
    req.num_atids = 1;
    for (int id = 0; id < 256; id++) {
        req.atids[0] = id; CHECK(coresight_qmi_assign_atid(&cs, &req) == (id > 0 && id < 0x70 ? 0 : -EINVAL));
    }
    req.num_atids = 2; req.atids[0] = req.atids[1] = 1; CHECK(coresight_qmi_assign_atid(&cs, &req) == -EINVAL);
    memset(req.name, 'x', sizeof(req.name)); CHECK(coresight_qmi_assign_atid(&cs, &req) == -EINVAL);
    struct coresight_etr_assign_req_msg_v01 etr = {.buffer_base = UINT64_MAX, .buffer_size = 1};
    CHECK(coresight_qmi_etr_assign(&cs, &etr) == -EINVAL);
    etr.buffer_base = 0; etr.buffer_size = 0; CHECK(coresight_qmi_etr_assign(&cs, &etr) == -EINVAL);
    struct device_driver other = {.name = "coresight-cti"}; pdev.dev.driver = &other; cs.type = CORESIGHT_DEV_TYPE_HELPER;
    CHECK(!coresight_is_qmi_device(&cs)); other.name = "coresight-qmi"; CHECK(coresight_is_qmi_device(&cs));
    for (int failure = 0; failure < 6; failure++) {
        reset();
        if (failure == 1) alloc_error = 1;
        if (failure == 2) instance_error = -EINVAL;
        if (failure == 3) init_error = -ENOMEM;
        if (failure == 4) lookup_error = -EIO;
        if (failure == 5) register_error = -EPROBE_DEFER;
        int expected[] = {0, -ENOMEM, -EINVAL, -ENOMEM, -EIO, -EPROBE_DEFER};
        CHECK(coresight_qmi_probe(&pdev) == expected[failure]);
        CHECK(releases == (failure >= 4));
        if (!failure) {
            coresight_qmi_remove(&pdev);
            CHECK(pdev.dev.data->removing && !pdev.dev.data->service_connected);
            CHECK(releases == 1 && unregisters == 1);
        }
    }
    free(allocation);
    printf("%u\n", cases);
    return 0;
}
