/* verify_qcom_sysfs.py inserts the complete ported driver into this fixture. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#define PAGE_SIZE 4096
#define GFP_KERNEL 0
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x) >= (uintptr_t)-4095)
#define PTR_ERR(x) ((long)(intptr_t)(x))
#define mca_log_info(...) ((void)0)
#define mca_log_err(...) ((void)0)
#define ADAPTER_PROTOCOL_BC12 0
#define ADAPTER_PROTOCOL_PD 1
#define TYPEC_PORT_0 0
#define MAIN_BUCK_CHARGER 0
#define CP_ROLE_MASTER 0
#define FG_IC_SLAVE 1
#define STRATEGY_FUNC_TYPE_QUICK_CHARGE 0
#define STRATEGY_FUNC_TYPE_BUCK_CHARGE 1
#define STRATEGY_STATUS_TYPE_QC_TYPE 2
struct class { const char *name; unsigned refs; bool destroyed; };
struct class_attribute {
    const char *name;
    ssize_t (*show)(const struct class *, const struct class_attribute *, char *);
    ssize_t (*store)(const struct class *, const struct class_attribute *, const char *, size_t);
};
#define CLASS_ATTR_RO(n) const struct class_attribute class_attr_##n = { #n, n##_show, NULL }
#define CLASS_ATTR_RW(n) const struct class_attribute class_attr_##n = { #n, n##_show, n##_store }
struct device { void *of_node; };
struct platform_device { struct device dev; void *data; };
struct of_device_id { const char *compatible; };
struct platform_driver {
    struct { const char *name; const struct of_device_id *of_match_table; } driver;
    int (*probe)(struct platform_device *);
    void (*remove)(struct platform_device *);
};
#define MODULE_DEVICE_TABLE(bus, table)
#define module_platform_driver(driver) static void *test_driver __attribute__((used)) = &(driver)
#define MODULE_DESCRIPTION(s)
#define MODULE_AUTHOR(s)
#define MODULE_LICENSE(s)
enum provider {
    ADAPTER_TYPE, AUTHENTIC, SLAVE_AUTHENTIC, VERIFIED_GET, VERIFIED_SET,
    QUICK_TYPE, BUCK_TYPE, SOC_DECIMAL, CID_SUPPORT, CID_STATUS, CC_GET, CC_SET,
    HAS_DP, OVP_GET, OVP_SET, IBAT, SHIP_GET, SHIP_SET, SOH, NUM_PROVIDERS
};
static int errors[NUM_PROVIDERS], values[NUM_PROVIDERS], calls[NUM_PROVIDERS];
static int last_write[NUM_PROVIDERS];
static int getter(enum provider p, int *out) {
    calls[p]++;
    if (!errors[p]) *out = values[p];
    return errors[p];
}
static int bool_getter(enum provider p, bool *out) {
    int value;
    int ret = getter(p, &value);
    if (!ret) *out = value;
    return ret;
}
static int setter(enum provider p, int value) {
    calls[p]++;
    if (!errors[p]) last_write[p] = value;
    return errors[p];
}
static int protocol_class_get_adapter_type(unsigned protocol, unsigned *out) {
    assert(protocol == ADAPTER_PROTOCOL_BC12);
    int value;
    int ret = getter(ADAPTER_TYPE, &value);
    if (!ret) *out = value;
    return ret;
}
static int strategy_class_fg_get_authentic(bool *out) { return bool_getter(AUTHENTIC, out); }
static int platform_fg_ops_get_authentic(unsigned role, int *out) {
    assert(role == FG_IC_SLAVE);
    return getter(SLAVE_AUTHENTIC, out);
}
static int protocol_class_get_adapter_verified(unsigned protocol, int *out) {
    assert(protocol == ADAPTER_PROTOCOL_PD);
    return getter(VERIFIED_GET, out);
}
static int protocol_class_set_adapter_verified(unsigned protocol, int value) {
    assert(protocol == ADAPTER_PROTOCOL_PD);
    return setter(VERIFIED_SET, value);
}
static int mca_strategy_func_get_status(unsigned strategy, unsigned status, int *out) {
    assert(status == STRATEGY_STATUS_TYPE_QC_TYPE && strategy <= 1);
    return getter(strategy == STRATEGY_FUNC_TYPE_QUICK_CHARGE ? QUICK_TYPE : BUCK_TYPE, out);
}
static int strategy_class_fg_ops_get_soc_decimal(int *decimal, int *rate) {
    int ret = getter(SOC_DECIMAL, decimal);
    if (!ret) *rate = 7;
    return ret;
}
#define BOOL_ROLE_GETTER(name, provider) \
static int name(unsigned role, bool *out) { assert(!role); return bool_getter(provider, out); }
#define INT_ROLE_GETTER(name, provider) \
static int name(unsigned role, int *out) { assert(!role); return getter(provider, out); }
#define BOOL_ROLE_SETTER(name, provider) \
static int name(unsigned role, bool value) { assert(!role); return setter(provider, value); }
BOOL_ROLE_GETTER(platform_class_buckchg_ops_is_support_cid, CID_SUPPORT)
BOOL_ROLE_GETTER(protocol_class_pd_get_cid_status, CID_STATUS)
BOOL_ROLE_GETTER(protocol_class_pd_get_cc_toggle, CC_GET)
BOOL_ROLE_GETTER(protocol_class_pd_get_has_dp, HAS_DP)
BOOL_ROLE_GETTER(platform_class_cp_get_ovpgate_status, OVP_GET)
BOOL_ROLE_GETTER(platform_class_buckchg_ops_get_ship_mode, SHIP_GET)
INT_ROLE_GETTER(platform_class_buckchg_ops_get_pack_ibat, IBAT)
BOOL_ROLE_SETTER(protocol_class_pd_set_cc_toggle, CC_SET)
BOOL_ROLE_SETTER(platform_class_cp_enable_ovpgate, OVP_SET)
BOOL_ROLE_SETTER(platform_class_buckchg_ops_set_ship_mode, SHIP_SET)
static int strategy_class_fg_get_soh(int *out) { return getter(SOH, out); }
static int sysfs_emit(char *out, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out, PAGE_SIZE, format, args);
    va_end(args);
    return n;
}
static int kstrtoint(const char *s, unsigned base, int *out) {
    char *end;
    if (!*s || *s == ' ' || *s == '\t') return -EINVAL;
    errno = 0;
    long value = strtol(s, &end, base);
    if (end == s) return -EINVAL;
    if (errno || value > INT_MAX || value < INT_MIN) return -ERANGE;
    if (*end == '\n') end++;
    if (*end) return -EINVAL;
    *out = value;
    return 0;
}
static int kstrtobool(const char *s, bool *out) {
    if (*s == '1' || *s == 'y' || *s == 'Y' || !strncmp(s, "on", 2)) *out = true;
    else if (*s == '0' || *s == 'n' || *s == 'N' || !strncmp(s, "off", 3)) *out = false;
    else return -EINVAL;
    return 0;
}
static void *managed;
static struct class *last_class;
static int alloc_fail, class_error, file_fail_index = -1;
static unsigned create_attempts, removals, destroys;
static const struct class_attribute *created[29];
static void *devm_kzalloc(struct device *dev, size_t size, int flags) {
    (void)dev; (void)flags;
    managed = alloc_fail ? NULL : calloc(1, size);
    return managed;
}
static bool of_property_read_bool(void *node, const char *name) {
    (void)node; (void)name;
    return false;
}
static struct class *class_create(const char *name) {
    if (class_error) return ERR_PTR(class_error);
    last_class = calloc(1, sizeof(*last_class));
    assert(last_class);
    last_class->name = name;
    return last_class;
}
static int class_create_file(const struct class *class, const struct class_attribute *attr) {
    assert(class == last_class);
    unsigned index = create_attempts++;
    if ((int)index == file_fail_index) return -EIO;
    assert(index < ARRAY_SIZE(created));
    created[index] = attr;
    return 0;
}
static void class_remove_file(const struct class *class, const struct class_attribute *attr) {
    assert(class == last_class && !class->destroyed);
    unsigned found = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(created); i++) {
        if (created[i] == attr) { created[i] = NULL; found++; }
    }
    assert(found == 1);
    removals++;
}
static void class_destroy(struct class *class) {
    assert(class == last_class);
    for (unsigned i = 0; i < ARRAY_SIZE(created); i++) assert(!created[i]);
    assert(!class->destroyed);
    class->destroyed = true;
    destroys++;
    if (!class->refs) { free(class); last_class = NULL; }
}
static void platform_set_drvdata(struct platform_device *p, void *data) { p->data = data; }
static void *platform_get_drvdata(struct platform_device *p) { return p->data; }
/* INSERT_DRIVER */
static void reset_lifetime(void) {
    free(managed);
    managed = NULL;
    memset(created, 0, sizeof(created));
    create_attempts = removals = destroys = 0;
    alloc_fail = class_error = 0;
    file_fail_index = -1;
}
int main(void) {
    unsigned cases = 0;
    char output[PAGE_SIZE];
    /* Every retained name is present, including unsupported wireless attributes. */
    const char *names[] = { "real_type", "usb_real_type", "wireless_type", "authentic",
        "slave_authentic", "pd_verifed", "quick_charge_type", "power_max", "soc_decimal",
        "soc_decimal_rate", "otg_ui_support", "cid_status", "cc_toggle", "has_dp",
        "dam_ovpgate", "pmic_ibat", "wireless_chip_fw", "reverse_chg_mode",
        "reverse_chg_state", "magnetic_case_flag", "tx_adapter", "rx_vout", "rx_vrect",
        "rx_iout", "rx_ss", "shipmode_count_reset", "wls_thermal_remove", "hall_phone_case", "soh" };
    assert(ARRAY_SIZE(mca_qcom_sysfs_attrs) == ARRAY_SIZE(names) + 1);
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
        assert(!strcmp(names[i], mca_qcom_sysfs_attrs[i]->name)); cases++;
    }
    /* Neither unfamiliar signed adapter values nor a failed read indexes the table. */
    for (int type = -16; type <= 256; type++) {
        values[ADAPTER_TYPE] = type;
        ssize_t size = real_type_show(NULL, NULL, output);
        assert(size > 0 && size == (ssize_t)strlen(output));
        if (type < 0 || type >= 16) assert(!strcmp(output, "Unknown\n"));
        assert(usb_real_type_show(NULL, NULL, output) == size); cases += 2;
    }
    struct { const struct class_attribute *attr; enum provider provider; } reads[] = {
        { &class_attr_real_type, ADAPTER_TYPE }, { &class_attr_usb_real_type, ADAPTER_TYPE },
        { &class_attr_authentic, AUTHENTIC }, { &class_attr_slave_authentic, SLAVE_AUTHENTIC },
        { &class_attr_pd_verifed, VERIFIED_GET }, { &class_attr_soc_decimal, SOC_DECIMAL },
        { &class_attr_soc_decimal_rate, SOC_DECIMAL }, { &class_attr_otg_ui_support, CID_SUPPORT },
        { &class_attr_cid_status, CID_STATUS }, { &class_attr_cc_toggle, CC_GET },
        { &class_attr_has_dp, HAS_DP }, { &class_attr_dam_ovpgate, OVP_GET },
        { &class_attr_pmic_ibat, IBAT }, { &class_attr_shipmode_count_reset, SHIP_GET },
        { &class_attr_soh, SOH } };
    int injected[] = { -EIO, -EAGAIN, -ENODEV, -EOPNOTSUPP };
    for (unsigned i = 0; i < ARRAY_SIZE(reads); i++) {
        for (unsigned e = 0; e < ARRAY_SIZE(injected); e++) {
            memset(output, 0x5a, sizeof(output));
            errors[reads[i].provider] = injected[e];
            assert(reads[i].attr->show(NULL, NULL, output) == injected[e]);
            assert(output[0] == 0x5a);
            errors[reads[i].provider] = 0; cases++;
        }
    }
    for (int authenticated = 0; authenticated <= 1; authenticated++) {
        values[AUTHENTIC] = values[SLAVE_AUTHENTIC] = authenticated;
        assert(authentic_show(NULL, NULL, output) == 2 && output[0] == '0' + authenticated);
        assert(slave_authentic_show(NULL, NULL, output) == 2 && output[0] == '0' + authenticated);
        cases += 2;
    }
    values[QUICK_TYPE] = 3;
    int before = calls[BUCK_TYPE];
    assert(quick_charge_type_show(NULL, NULL, output) == 2 && !strcmp(output, "3\n"));
    assert(calls[BUCK_TYPE] == before); cases++;
    values[QUICK_TYPE] = 0; values[BUCK_TYPE] = 4;
    assert(quick_charge_type_show(NULL, NULL, output) == 2 && !strcmp(output, "4\n")); cases++;
    errors[QUICK_TYPE] = -EIO;
    before = calls[BUCK_TYPE];
    assert(quick_charge_type_show(NULL, NULL, output) == -EIO && calls[BUCK_TYPE] == before);
    errors[QUICK_TYPE] = 0; errors[BUCK_TYPE] = -EAGAIN;
    assert(quick_charge_type_show(NULL, NULL, output) == -EAGAIN);
    errors[BUCK_TYPE] = 0; cases += 2;
    const struct class_attribute *unsupported[] = { &class_attr_wireless_type, &class_attr_power_max,
        &class_attr_wireless_chip_fw, &class_attr_reverse_chg_mode, &class_attr_reverse_chg_state,
        &class_attr_magnetic_case_flag, &class_attr_tx_adapter, &class_attr_rx_vout, &class_attr_rx_vrect,
        &class_attr_rx_iout, &class_attr_rx_ss, &class_attr_wls_thermal_remove, &class_attr_hall_phone_case };
    for (unsigned i = 0; i < ARRAY_SIZE(unsupported); i++) {
        assert(unsupported[i]->show(NULL, NULL, output) == -EOPNOTSUPP); cases++;
        if (unsupported[i]->store) {
            assert(unsupported[i]->store(NULL, NULL, "1", 1) == -EOPNOTSUPP); cases++;
        }
    }
    assert(authentic_store(NULL, NULL, "1", 1) == -EOPNOTSUPP);
    assert(slave_authentic_store(NULL, NULL, "1", 1) == -EOPNOTSUPP); cases += 2;
    struct { const struct class_attribute *attr; enum provider provider; } writes[] = {
        { &class_attr_pd_verifed, VERIFIED_SET }, { &class_attr_cc_toggle, CC_SET },
        { &class_attr_dam_ovpgate, OVP_SET }, { &class_attr_shipmode_count_reset, SHIP_SET } };
    const char *bad[] = { "", "2", "-1", "abc", "2147483648", " 1" };
    for (unsigned i = 0; i < ARRAY_SIZE(writes); i++) {
        for (unsigned b = 0; b < ARRAY_SIZE(bad); b++) {
            before = calls[writes[i].provider];
            assert(writes[i].attr->store(NULL, NULL, bad[b], strlen(bad[b])) < 0);
            assert(calls[writes[i].provider] == before); cases++;
        }
        for (unsigned e = 0; e < ARRAY_SIZE(injected); e++) {
            errors[writes[i].provider] = injected[e];
            assert(writes[i].attr->store(NULL, NULL, "1\n", 2) == injected[e]); cases++;
        }
        errors[writes[i].provider] = 0;
        assert(writes[i].attr->store(NULL, NULL, "1\n", 2) == 2 && last_write[writes[i].provider] == 1);
        assert(writes[i].attr->store(NULL, NULL, "0", 1) == 1 && last_write[writes[i].provider] == 0); cases += 2;
    }
    struct platform_device p = {0};
    alloc_fail = 1; assert(mca_qcom_sysfs_probe(&p) == -ENOMEM); cases++;
    reset_lifetime();
    class_error = -EEXIST;
    assert(mca_qcom_sysfs_probe(&p) == -EEXIST && !create_attempts && !destroys); cases++;
    reset_lifetime();
    for (int i = 0; i < 29; i++) {
        file_fail_index = i; p.data = NULL;
        assert(mca_qcom_sysfs_probe(&p) == -EIO && !p.data);
        assert(create_attempts == (unsigned)i + 1 && removals == (unsigned)i && destroys == 1);
        reset_lifetime(); cases++;
    }
    assert(!mca_qcom_sysfs_probe(&p) && p.data && create_attempts == 29);
    last_class->refs = 1;
    struct class *held = last_class;
    mca_qcom_sysfs_remove(&p);
    assert(removals == 29 && destroys == 1 && held->destroyed);
    /* A final class reference is independent of devm driver data lifetime. */
    reset_lifetime();
    assert(!strcmp(held->name, "qcom-battery"));
    assert(real_type_show(held, NULL, output) > 0);
    free(held); last_class = NULL; cases += 3;
    printf("%u\n", cases);
}
