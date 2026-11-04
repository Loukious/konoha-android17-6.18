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
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define U32_MAX UINT32_MAX
#define U64_MAX UINT64_MAX
#define BIT(n) (1U << (n))
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define IS_ERR(p) ((uintptr_t)(p) > (uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(n) ((void *)(intptr_t)(n))
#define __init
#define __exit
#define EXPORT_SYMBOL_GPL(n)
#define MODULE_LICENSE(n)
#define MODULE_DESCRIPTION(n)
#define MODULE_SOFTDEP(n)
#define module_init(n)
#define module_exit(n)
#define pr_err(...) ((void)0)
struct mutex { pthread_mutex_t lock; };
#define DEFINE_MUTEX(n) struct mutex n = { PTHREAD_MUTEX_INITIALIZER }
#define mutex_lock(p) assert(!pthread_mutex_lock(&(p)->lock))
#define mutex_unlock(p) assert(!pthread_mutex_unlock(&(p)->lock))
static u64 counter;
static u64 arch_timer_read_counter(void) { return counter; }
struct item { unsigned host, id; void *data; size_t len; int error; };
static struct item items[18];
static unsigned item_count;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool block_reader, reader_entered, resume_reader;
static atomic_int active_readers, removal_started, removal_done;
static void *qcom_smem_get(unsigned host, unsigned id, size_t *len)
{
    if (block_reader) {
        pthread_mutex_lock(&gate);
        reader_entered = true;
        pthread_cond_broadcast(&changed);
        while (!resume_reader) pthread_cond_wait(&changed, &gate);
        pthread_mutex_unlock(&gate);
    }
    for (unsigned i = 0; i < item_count; i++) {
        if (items[i].host == host && items[i].id == id) {
            *len = items[i].len;
            return items[i].error ? ERR_PTR(items[i].error) : items[i].data;
        }
    }
    /* Kernel provider failures need not initialize the size output. */
    return ERR_PTR(-ENOENT);
}
struct seq_file { char buf[16384]; size_t used; };
static void seq_printf(struct seq_file *s, const char *fmt, ...)
{
    va_list args; va_start(args, fmt);
    int n = vsnprintf(s->buf + s->used, sizeof(s->buf) - s->used, fmt, args);
    va_end(args); assert(n >= 0 && (size_t)n < sizeof(s->buf) - s->used);
    s->used += n;
}
static void seq_puts(struct seq_file *s, const char *text) { seq_printf(s, "%s", text); }
#define DEFINE_SHOW_ATTRIBUTE(n) static int n##_fops
struct dentry { bool present; };
static struct dentry directory, files[2];
static unsigned debug_calls, debug_fail, debug_null, debug_index;
static struct dentry *debugfs_create_dir(const char *name, void *parent)
{
    assert(!directory.present);
    if (++debug_calls == debug_fail) return ERR_PTR(-ENOMEM);
    if (debug_calls == debug_null) return NULL;
    directory.present = true; debug_index = 0; return &directory;
}
static struct dentry *debugfs_create_file(const char *name, unsigned mode, struct dentry *d, void *data, void *fops)
{
    assert(d && d->present && mode == 0444 && debug_index < 2);
    if (++debug_calls == debug_fail) return ERR_PTR(-ENOMEM);
    if (debug_calls == debug_null) return NULL;
    struct dentry *p = &files[debug_index++]; p->present = true; return p;
}
static void debugfs_remove_recursive(struct dentry *d)
{
    if (!d) return;
    pthread_mutex_lock(&gate);
    atomic_store(&removal_started, 1);
    pthread_cond_broadcast(&changed);
    while (atomic_load(&active_readers)) pthread_cond_wait(&changed, &gate);
    d->present = false; memset(files, 0, sizeof(files));
    pthread_mutex_unlock(&gate);
}
/* HEADER */
/* DRIVER */
static void reset(void)
{
    assert(!directory.present && !atomic_load(&active_readers));
    memset(&g_sysmon_stats, 0, sizeof(g_sysmon_stats));
    memset(snapshots, 0, sizeof(snapshots));
    sysmon_stopping = false; item_count = 0;
    debug_calls = debug_fail = debug_null = 0;
    block_reader = reader_entered = resume_reader = false;
    atomic_store(&removal_started, 0); atomic_store(&removal_done, 0);
}
static void item(unsigned host, unsigned id, void *data, size_t len)
{
    assert(item_count < ARRAY_SIZE(items));
    items[item_count++] = (struct item){host, id, data, len, 0};
}
static u32 feature_header(unsigned id, unsigned len, unsigned ver)
{
    return (id << 28) | (len << 16) | ver;
}
static size_t minimum(unsigned id, unsigned ver)
{
    if (id == 1 || id == 4 || id == 5) return 16;
    if (id == 2) return ver > 1 ? 44 : 28;
    if (id == 3) return ver >= 2 ? 216 : 140;
    return 4;
}
static unsigned char firmware[512];
static struct sleep_stats sleep_fw;
static struct sleep_stats_island island_fw;
static struct dsppm_stats dsppm_fw;
static size_t firmware_size;
static void prepare_firmware(unsigned host)
{
    unsigned char *p = firmware;
    struct sysmon_smem_q6_load_stats load = {99, (u32)counter, (u32)(counter >> 32)};
    u32 h = feature_header(1, 16, 1); memcpy(p, &h, 4); memcpy(p + 4, &load, 12); p += 16;
    struct sysmon_smem_q6_event_stats_extended event = {
        .featureId_q6Event = feature_header(2, 44, host == CDSP ? 2 : 1),
        .event_stats = {1200, 22, 1, 33, 2, 500}, .event_stats_ext = {1, 600}};
    memcpy(p, &event, 44); p += 44;
    struct sysmon_smem_power_stats_extended power = {0};
    power.powerstats.version = feature_header(3, 216, 2);
    power.powerstats.clk_arr[0] = power.powerstats.current_clk = 1200;
    power.powerstats.active_time[0] = 42;
    power.last_update_time_powerstats_lsb = (u32)(5ULL * SYS_CLK_TICKS_PER_SEC);
    memcpy(p, &power, 216); p += 216;
    if (host == CDSP) {
        struct sysmon_smem_hmx_stats hmx = {65, (u32)counter, (u32)(counter >> 32)};
        struct sysmon_smem_hvx_stats hvx = {77, (u32)counter, (u32)(counter >> 32)};
        h = feature_header(4, 16, 1); memcpy(p, &h, 4); memcpy(p + 4, &hmx, 12); p += 16;
        h = feature_header(5, 16, 1); memcpy(p, &h, 4); memcpy(p + 4, &hvx, 12); p += 16;
    }
    firmware_size = p - firmware;
    sleep_fw = (struct sleep_stats){1, 17, 9ULL * SYS_CLK_TICKS_PER_SEC, 8ULL * SYS_CLK_TICKS_PER_SEC, SYS_CLK_TICKS_PER_SEC};
    island_fw = (struct sleep_stats_island){1, 3, 0, 0, SYS_CLK_TICKS_PER_SEC};
    dsppm_fw = (struct dsppm_stats){1, 200, 50, {{1,2},{2,3}}};
    item(host, 634, firmware, firmware_size);
    item(host, host == ADSP ? 606 : host == CDSP ? 607 : 608, &sleep_fw, sizeof(sleep_fw));
    if (host != CDSP) item(host, 613, &island_fw, sizeof(island_fw));
    if (host != SLPI) item(host, 624, &dsppm_fw, sizeof(dsppm_fw));
}
static void *thread_query(void *unused)
{
    for (unsigned i = 0; i < 2500; i++) {
        u32 load = 0; struct sleep_stats sleep;
        assert(!sysmon_stats_query_q6_load(ADSP, &load) && load == 99);
        assert(!sysmon_stats_query_sleep(ADSP, &sleep, NULL) && sleep.accumulated == 2ULL * SYS_CLK_TICKS_PER_SEC);
    }
    return NULL;
}
static void *thread_show(void *unused)
{
    struct seq_file seq = {0};
    atomic_fetch_add(&active_readers, 1);
    assert(!master_adsp_stats_show(&seq, NULL));
    pthread_mutex_lock(&gate);
    atomic_fetch_sub(&active_readers, 1); pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    return NULL;
}
static void *thread_remove(void *unused)
{
    sysmon_stats_exit(); atomic_store(&removal_done, 1); return NULL;
}
int main(void)
{
    unsigned cases = 0; counter = 10ULL * SYS_CLK_TICKS_PER_SEC;
    assert(sizeof(struct sysmon_smem_power_stats_extended) == 216);
    assert(sizeof(struct sysmon_smem_q6_event_stats_extended) == 44);
    for (unsigned fail = 1; fail <= 3; fail++) for (unsigned null = 0; null < 2; null++) {
        reset(); if (null) debug_null = fail; else debug_fail = fail;
        assert(sysmon_stats_init() == -ENOMEM && !directory.present && !files[0].present && !files[1].present); cases++;
    }
    /* All header/extent boundaries use an allocation with exactly that extent. */
    for (unsigned id = 0; id < 8; id++) for (unsigned ver = 1; ver <= 4; ver++)
        for (unsigned extent = 0; extent <= 220; extent++) {
            unsigned char *p = calloc(1, extent ? extent : 1); assert(p);
            if (extent >= 4) { u32 h = feature_header(id, extent, ver); memcpy(p, &h, 4); }
            struct sysmon_snapshot snap = {0};
            int r = sysmon_parse_features(p, extent, &snap);
            assert((extent >= minimum(id, ver)) ? !r : r < 0); free(p); cases++;
        }
    for (unsigned len = 0; len < 512; len++) {
        u32 h = feature_header(6, len, 1); struct sysmon_snapshot snap = {0};
        int r = sysmon_parse_features(&h, 4, &snap); assert(len == 4 ? !r : r < 0); cases++;
    }
    struct sysmon_snapshot snap = {0};
    assert(sysmon_parse_features(NULL, 999, &snap) == -ENOKEY);
    assert(sysmon_parse_features(ERR_PTR(-ENOENT), 999, &snap) == -ENOKEY); cases += 2;
    /* Duplicate known records and a trailing partial header never publish. */
    unsigned char duplicate[32] = {0}; u32 h = feature_header(1, 16, 1);
    memcpy(duplicate, &h, 4); memcpy(duplicate + 16, &h, 4);
    assert(sysmon_parse_features(duplicate, 32, &snap) == -EINVAL); cases++;
    reset(); assert(!sysmon_stats_init()); prepare_firmware(ADSP);
    unsigned char original[512]; memcpy(original, firmware, sizeof(original));
    struct sleep_stats sleep_original = sleep_fw;
    struct sleep_stats_island island_original = island_fw;
    for (unsigned i = 0; i < 100; i++) {
        struct sleep_stats lpm; struct sleep_stats_island lpi;
        struct sysmon_smem_power_stats power; struct sysmon_smem_q6_event_stats votes; u32 load;
        assert(!sysmon_stats_query_sleep(ADSP, &lpm, &lpi));
        assert(lpm.accumulated == 2ULL * SYS_CLK_TICKS_PER_SEC && lpi.accumulated == SYS_CLK_TICKS_PER_SEC);
        assert(!sysmon_stats_query_q6_votes(ADSP, &votes) && votes.QDSP6_clk == 1200 && votes.HMX_clk == -1);
        assert(!sysmon_stats_query_q6_load(ADSP, &load) && load == 99);
        assert(!sysmon_stats_query_power_residency(ADSP, &power));
        assert(power.version == 2 && power.active_time[0] == 45 && power.pc_time == 1 && power.lpi_time == 1 && power.island_time[0] == 1);
        struct seq_file seq = {0}; assert(!master_adsp_stats_show(&seq, NULL));
        assert(strstr(seq.buf, "DSPPM stats:") && strstr(seq.buf, "LPM stats:") && strstr(seq.buf, "Power Stats:") && strstr(seq.buf, "99"));
        assert(!memcmp(original, firmware, sizeof(original)) && !memcmp(&sleep_original, &sleep_fw, sizeof(sleep_fw)) && !memcmp(&island_original, &island_fw, sizeof(island_fw))); cases += 6;
    }
    /* Fresh snapshots observe new firmware values after the first successful read. */
    u32 changed_load = 101; memcpy(firmware + 4, &changed_load, 4); u32 out;
    assert(!sysmon_stats_query_q6_load(ADSP, &out) && out == 101); memcpy(firmware + 4, original + 4, 4); cases++;
    pthread_t threads[4]; for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, thread_query, NULL));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL)); cases += 20000;
    assert(!memcmp(original, firmware, sizeof(original)) && !memcmp(&sleep_original, &sleep_fw, sizeof(sleep_fw)));
    sysmon_stats_exit(); reset(); assert(!sysmon_stats_init()); prepare_firmware(CDSP);
    assert(!sysmon_stats_query_hmx_utlization(&out) && out == 65);
    assert(!sysmon_stats_query_hvx_utlization(&out) && out == 77);
    struct sysmon_smem_q6_event_stats votes;
    assert(!sysmon_stats_query_q6_votes(CDSP, &votes) && votes.HMX_clk == 600 && votes.HMX_Power_state == 1);
    assert(sysmon_stats_query_sleep(CDSP, NULL, &island_original) == -EOPNOTSUPP);
    assert(sysmon_stats_query_hmx_utlization(NULL) == -EINVAL && sysmon_stats_query_hvx_utlization(NULL) == -EINVAL); cases += 6;
    counter += 101ULL * SYS_CLK_TICKS_PER_MS;
    assert(!sysmon_stats_query_q6_load(CDSP, &out) && out == 0);
    assert(!sysmon_stats_query_hmx_utlization(&out) && out == 0 && !sysmon_stats_query_hvx_utlization(&out) && out == 0); cases += 3;
    counter = 10ULL * SYS_CLK_TICKS_PER_SEC;
    unsigned offset = firmware_size - 28; u32 na = 0xffff; memcpy(firmware + offset, &na, 4);
    assert(sysmon_stats_query_hmx_utlization(&out) == DSP_PMU_COUNTER_NA && out == 0); cases++;
    sysmon_stats_exit(); reset(); assert(!sysmon_stats_init());
    /* No firmware and short items never become stale or error pointers. */
    for (unsigned dsp = 0; dsp < 8; dsp++) {
        struct sysmon_smem_power_stats p; struct sleep_stats lpm;
        assert(sysmon_stats_query_q6_load(dsp, &out) < 0);
        assert(sysmon_stats_query_q6_votes(dsp, &votes) < 0);
        assert(sysmon_stats_query_power_residency(dsp, &p) < 0);
        assert(sysmon_stats_query_sleep(dsp, &lpm, NULL) < 0); cases += 4;
    }
    for (unsigned len = 0; len < 32; len++) {
        item_count = 0; unsigned char *p = calloc(1, len ? len : 1); assert(p);
        item(ADSP, 606, p, len); struct sleep_stats lpm;
        assert(sysmon_stats_query_sleep(ADSP, &lpm, NULL) == -ENOKEY); free(p); cases++;
    }
    item_count = 0; prepare_firmware(ADSP);
    sleep_fw.last_entered_at = counter + 1;
    struct sleep_stats lpm; assert(!sysmon_stats_query_sleep(ADSP, &lpm, NULL) && lpm.accumulated == sleep_fw.accumulated); cases++;
    sleep_fw.last_entered_at = counter - 100; sleep_fw.accumulated = UINT64_MAX - 50;
    assert(!sysmon_stats_query_sleep(ADSP, &lpm, NULL) && lpm.accumulated == UINT64_MAX); cases++;
    for (unsigned i = 0; i < 10000; i++) {
        u32 value = i & 1 ? UINT32_MAX - (i % 30) : i;
        u64 delta = i & 2 ? UINT64_MAX - i : i * 3ULL;
        u32 expected = delta > (u64)UINT32_MAX - value ? UINT32_MAX : value + delta;
        assert(sysmon_add_seconds(value, delta) == expected); cases++;
    }
    sysmon_stats_exit(); reset(); assert(!sysmon_stats_init()); prepare_firmware(ADSP);
    /* Removal waits for an active show before clearing cached pointers. */
    block_reader = true; pthread_t reader, remover;
    assert(!pthread_create(&reader, NULL, thread_show, NULL));
    pthread_mutex_lock(&gate); while (!reader_entered) pthread_cond_wait(&changed, &gate); pthread_mutex_unlock(&gate);
    assert(!pthread_create(&remover, NULL, thread_remove, NULL));
    pthread_mutex_lock(&gate); while (!atomic_load(&removal_started)) pthread_cond_wait(&changed, &gate);
    assert(!atomic_load(&removal_done) && directory.present); resume_reader = true; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&gate);
    assert(!pthread_join(reader, NULL) && !pthread_join(remover, NULL));
    assert(atomic_load(&removal_done) && !directory.present && sysmon_stats_query_q6_load(ADSP, &out) == -ESHUTDOWN); cases++;
    pthread_mutex_destroy(&sysmon_lock.lock); printf("%u\n", cases);
}
