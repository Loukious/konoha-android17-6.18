#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define __init
#define __exit
#define __iomem
#define __packed __attribute__((packed))
#define IS_MODULE(x) 1
#define module_init(x)
#define module_exit(x)
#define pure_initcall(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_LICENSE(x)
struct resource { uint64_t start, end; };
struct device_node { int id; bool available; unsigned refs; };
static struct device_node nodes[2];
static bool missing[2], map_fail[2];
static int address_error[2], frequency_error;
static uint64_t sizes[2];
static u32 clock_hz, imem[8], counter;
static unsigned unmapped[2], reads, logs;
static char last_log[256];
static struct device_node *of_find_compatible_node(void *a, void *b, const char *name) {
    (void)a; (void)b;
    unsigned id = strstr(name, "boot_stats") ? 0 : 1;
    if (missing[id]) return NULL;
    nodes[id].refs++;
    return &nodes[id];
}
static bool of_device_is_available(struct device_node *node) { return node->available; }
static int of_address_to_resource(struct device_node *node, int index, struct resource *res) {
    assert(index == 0);
    if (address_error[node->id]) return address_error[node->id];
    res->start = 0; res->end = sizes[node->id] - 1;
    return 0;
}
static uint64_t resource_size(struct resource *res) { return res->end - res->start + 1; }
static void *of_iomap(struct device_node *node, int index) {
    assert(index == 0);
    return map_fail[node->id] ? NULL : node->id ? (void *)&counter : (void *)imem;
}
static int of_property_read_u32(struct device_node *node, const char *name, u32 *value) {
    assert(node->id == 1 && !strcmp(name, "clock-frequency"));
    if (frequency_error) return frequency_error;
    *value = clock_hz; return 0;
}
static void of_node_put(struct device_node *node) {
    if (node) { assert(node->refs); node->refs--; }
}
static void iounmap(void *pointer) {
    if (pointer == imem) unmapped[0]++;
    else { assert(pointer == &counter); unmapped[1]++; }
}
static u32 readl_relaxed(const void *pointer) { reads++; return *(const u32 *)pointer; }
static void pr_info(const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(last_log, sizeof(last_log), format, args); va_end(args); logs++;
}
static int epoch_failure[2];
static bool hooks[2];
static unsigned synchronizations;
static int register_trace_android_vh_show_suspend_epoch_val(void (*fn)(void *,u64,u64), void *data) {
    (void)fn; (void)data; assert(!hooks[0]); if (epoch_failure[0]) return epoch_failure[0]; hooks[0] = true; return 0;
}
static int register_trace_android_vh_show_resume_epoch_val(void (*fn)(void *,u64), void *data) {
    (void)fn; (void)data; assert(!hooks[1]); if (epoch_failure[1]) return epoch_failure[1]; hooks[1] = true; return 0;
}
static void unregister_trace_android_vh_show_suspend_epoch_val(void (*fn)(void *,u64,u64), void *data) {
    (void)fn; (void)data; assert(hooks[0]); hooks[0] = false;
}
static void unregister_trace_android_vh_show_resume_epoch_val(void (*fn)(void *,u64), void *data) {
    (void)fn; (void)data; assert(hooks[1]); hooks[1] = false;
}
static void tracepoint_synchronize_unregister(void) { assert(!hooks[0] && !hooks[1]); synchronizations++; }
/* INSERT_DRIVERS */
static void reset(void) {
    memset(missing, 0, sizeof(missing)); memset(map_fail, 0, sizeof(map_fail));
    memset(address_error, 0, sizeof(address_error)); memset(unmapped, 0, sizeof(unmapped));
    frequency_error = 0; clock_hz = 32768; sizes[0] = 32; sizes[1] = 4;
    for (unsigned id=0; id<2; id++) { assert(!nodes[id].refs); nodes[id].id=id; nodes[id].available=true; }
    logs=reads=0;
    assert(!boot_stats && !mpm_counter_base);
}
int main(void) {
    unsigned cases = 0;
    for (unsigned failure=0; failure<12; failure++) {
        reset(); int wanted = -EINVAL; unsigned unmaps = 1;
        switch (failure) {
        case 0: missing[0]=true; wanted=-ENODEV; unmaps=0; break;
        case 1: nodes[0].available=false; wanted=-ENODEV; unmaps=0; break;
        case 2: address_error[0]=-EIO; wanted=-EIO; unmaps=0; break;
        case 3: sizes[0]=31; unmaps=0; break;
        case 4: map_fail[0]=true; wanted=-ENOMEM; unmaps=0; break;
        case 5: missing[1]=true; wanted=-ENODEV; break;
        case 6: nodes[1].available=false; wanted=-ENODEV; break;
        case 7: frequency_error=-EIO; wanted=-EIO; break;
        case 8: clock_hz=0; break;
        case 9: address_error[1]=-EIO; wanted=-EIO; break;
        case 10: sizes[1]=3; break;
        case 11: map_fail[1]=true; wanted=-ENOMEM; break;
        }
        assert(boot_stats_init()==wanted);
        assert(!nodes[0].refs && !nodes[1].refs && !reads && !logs);
        assert(unmapped[0]==unmaps && !unmapped[1] && !boot_stats && !mpm_counter_base); cases++;
    }
    reset(); imem[0]=13; imem[1]=24; imem[2]=UINT32_MAX-3; imem[3]=7; counter=123;
    assert(!boot_stats_init()); assert(reads==5 && logs==5 && unmapped[0]==1 && unmapped[1]==1);
    assert(!nodes[0].refs && !nodes[1].refs && !boot_stats && !mpm_counter_base); boot_stats_exit(); cases++;
    epoch_failure[0]=-EIO;
    assert(msm_show_epoch_init()==-EIO && !hooks[0] && !hooks[1] && !synchronizations); cases++;
    epoch_failure[0]=0; epoch_failure[1]=-ENOMEM;
    assert(msm_show_epoch_init()==-ENOMEM && !hooks[0] && !hooks[1] && synchronizations==1); cases++;
    epoch_failure[1]=0; assert(!msm_show_epoch_init() && hooks[0] && hooks[1]);
    msm_show_suspend_epoch_val(NULL, UINT64_MAX, 42); assert(strstr(last_log,"18446744073709551615") && strstr(last_log,"42"));
    msm_show_resume_epoch_val(NULL, UINT64_MAX); assert(strstr(last_log,"18446744073709551615"));
    msm_show_epoch_exit(); assert(!hooks[0] && !hooks[1] && synchronizations==2); cases++;
    printf("%u\n",cases);
}
