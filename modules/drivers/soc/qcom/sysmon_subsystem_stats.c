// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/limits.h>
#include <clocksource/arm_arch_timer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/smem.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/soc/qcom/sysmon_subsystem_stats.h>

#define SYSMON_SMEM_ID					634
#define SLEEPSTATS_SMEM_ID_ADSP			606
#define SLEEPSTATS_SMEM_ID_CDSP			607
#define SLEEPSTATS_SMEM_ID_SLPI			608
#define SLEEPSTATS_LPI_SMEM_ID			613
#define DSPPMSTATS_SMEM_ID				624
#define SYS_CLK_TICKS_PER_MS			19200
#define SYS_CLK_TICKS_PER_SEC			19200000
#define DSPPMSTATS_NUMPD				5
#define HMX_HVX_PMU_EVENTS_NA			0xFFFF

struct pd_clients {
	int pid;
	u32 num_active;
};

struct dsppm_stats {
	u32 version;
	u32 latency_us;
	u32 timestamp;
	struct pd_clients pd[DSPPMSTATS_NUMPD];
};

struct sysmon_smem_power_stats_extended {
	struct sysmon_smem_power_stats powerstats;
	/**< Powerstats data */

	u32 last_update_time_powerstats_lsb;
	/**< Powerstats updated timestamp (lower significant 32 bits) */

	u32 last_update_time_powerstats_msb;
	/**< Powerstats updated timestamp (Most significant 32 bits) */
};

struct sysmon_smem_q6_event_stats_local {
	u32 QDSP6_clk;
	/**< Q6 core clock in KHz */

	u32 Ab_vote_lsb;
	/**< Lower 32bits of the average bandwidth vote in bytes */

	u32 Ab_vote_msb;
	/**< Upper 32bits of the average bandwidth vote in bytes */

	u32 Ib_vote_lsb;
	/**< Lower 32bits of instantaneous bandwidth vote in bytes */

	u32 Ib_vote_msb;
	/**< Upper 32bits of instantaneous bandwidth vote in bytes */

	u32 Sleep_latency;
	/**< Sleep latency vote in micro-seconds */

};

struct sysmon_smem_q6_event_stats_ext {

	int HMX_Power_state;
	/**< HMX Power state 1 : HMX ON
	 *                   0 : HMX OFF
	 *                  -1 : not supported
	 */

	int HMX_clk;
	/**< HMX clock in KHz, -1 When not supported */
};

struct sysmon_smem_q6_event_stats_extended {
	u32 featureId_q6Event;
	/**< Feature [31:28] Size [27:16] version[15:0] */

	struct sysmon_smem_q6_event_stats_local event_stats;
	/*Structure type to hold DSP Q6 event based statistics*/

	uint32_t Last_update_time_event_lsb;
	/**< LSB of Smem update lsb timestamp for event based stat updates*/

	uint32_t Last_update_time_event_msb;
	/**< MSB of Smem updatemsb timestamp for event based stat updates*/

	struct sysmon_smem_q6_event_stats_ext event_stats_ext;
	/**< Extended DSP Q6 event stats */
};

struct sysmon_smem_stats {
	bool smem_init_adsp;
	bool smem_init_cdsp;
	bool smem_init_slpi;
	bool smem_init_cdsp_v2;
	struct sysmon_smem_power_stats_extended *sysmon_power_stats_adsp;
	struct sysmon_smem_power_stats_extended *sysmon_power_stats_cdsp;
	struct sysmon_smem_power_stats_extended *sysmon_power_stats_slpi;
	struct sysmon_smem_q6_event_stats_extended *sysmon_event_stats_adsp;
	struct sysmon_smem_q6_event_stats_extended *sysmon_event_stats_cdsp;
	struct sysmon_smem_q6_event_stats_extended *sysmon_event_stats_slpi;
	struct sleep_stats *sleep_stats_adsp;
	struct sleep_stats *sleep_stats_cdsp;
	struct sleep_stats *sleep_stats_slpi;
	struct sleep_stats_island *sleep_lpi_adsp;
	struct sleep_stats_island *sleep_lpi_slpi;
	u32 *q6_avg_load_adsp;
	u32 *q6_avg_load_cdsp;
	u32 *q6_avg_load_slpi;
	struct dsppm_stats *dsppm_stats_adsp;
	struct dsppm_stats *dsppm_stats_cdsp;
	struct dentry *debugfs_dir;
	struct dentry *debugfs_master_adsp_stats;
	struct dentry *debugfs_master_cdsp_stats;
	u32 *hmx_util;
	u32 *hvx_util;
};

enum feature_id {
	SYSMONSTATS_Q6_LOAD_FEATUREID = 1,
	SYSMONSTATS_Q6_EVENT_FEATUREID,
	SYSMON_POWER_STATS_FEATUREID,
	SYSMON_HMX_UTIL_FEATUREID,
	SYSMON_HVX_UTIL_FEATUREID
};

struct sysmon_smem_q6_load_stats {
	u32 q6load_avg;
	u32 Last_update_time_load_lsb;
	u32 Last_update_time_load_msb;
};

struct sysmon_smem_hvx_stats {
	u32 hvx_util_avg;
	u32 Last_update_time_load_lsb;
	u32 Last_update_time_load_msb;
};

struct sysmon_smem_hmx_stats {
	u32 hmx_util_avg;
	u32 Last_update_time_load_lsb;
	u32 Last_update_time_load_msb;
};

static struct sysmon_smem_stats g_sysmon_stats;

/* Adds delta between SMEM powerstats updated time to current time */
static u64 sysmon_accumulated(u64 accumulated, u64 entered, u64 exited)
{
    u64 now = arch_timer_read_counter(), delta;

    if (entered <= exited || now < entered)
        return accumulated;
    delta = now - entered;
    return delta > U64_MAX - accumulated ? U64_MAX : accumulated + delta;
}

static u32 sysmon_add_seconds(u32 value, u64 delta)
{
    return delta > U32_MAX - value ? U32_MAX : value + delta;
}

static int add_delta_time(u8 ver, u64 lpi_acc, u64 lpm_acc,
                         struct sysmon_smem_power_stats_extended *stats)
{
    u64 updated, now, elapsed, pc, lpi, active;
    unsigned int j;

    if (ver < 2)
        return 0;
    updated = ((u64)stats->last_update_time_powerstats_msb << 32) |
              stats->last_update_time_powerstats_lsb;
    now = arch_timer_read_counter();
    /* A future timestamp can follow a firmware reset or torn update. */
    if (now < updated)
        return 0;
    elapsed = (now - updated) / SYS_CLK_TICKS_PER_SEC;
    if (!elapsed)
        return 0;
    pc = lpm_acc / SYS_CLK_TICKS_PER_SEC;
    pc = pc >= stats->powerstats.pc_time ? pc - stats->powerstats.pc_time : 0;
    pc = min(pc, elapsed);
    lpi = lpi_acc / SYS_CLK_TICKS_PER_SEC;
    lpi = lpi >= stats->powerstats.lpi_time ? lpi - stats->powerstats.lpi_time : 0;
    lpi = min(lpi, pc);
    active = elapsed - pc;
    for (j = 0; j < SYSMON_POWER_STATS_MAX_CLK_LEVELS; j++)
        if (stats->powerstats.clk_arr[j] == stats->powerstats.current_clk)
            break;
    if (j == SYSMON_POWER_STATS_MAX_CLK_LEVELS)
        return -EINVAL;
    stats->powerstats.active_time[j] = sysmon_add_seconds(stats->powerstats.active_time[j], active);
    stats->powerstats.island_time[j] = sysmon_add_seconds(stats->powerstats.island_time[j], lpi);
    stats->powerstats.pc_time = sysmon_add_seconds(stats->powerstats.pc_time, pc - lpi);
    stats->powerstats.lpi_time = sysmon_add_seconds(stats->powerstats.lpi_time, lpi);
    return 0;
}

/* A mutex protects all cached pointers and the complete native query/show.
 * Each call refreshes private snapshots. Firmware-owned SMEM is never written.
 * SMEM has no sequence protocol; fields can still reflect a concurrent DSP
 * update, so these diagnostics do not claim a globally atomic firmware sample.
 */
static DEFINE_MUTEX(sysmon_lock);
static bool sysmon_stopping;

struct sysmon_snapshot {
    struct sysmon_smem_power_stats_extended power;
    struct sysmon_smem_q6_event_stats_extended event;
    struct sysmon_smem_q6_load_stats load;
    struct sysmon_smem_hmx_stats hmx;
    struct sysmon_smem_hvx_stats hvx;
    struct dsppm_stats dsppm;
    struct sleep_stats lpm;
    struct sleep_stats_island lpi;
    bool has_power, has_event, has_load, has_hmx, has_hvx;
    bool has_dsppm, has_lpm, has_lpi;
};
static struct sysmon_snapshot snapshots[3];

static int sysmon_parse_features(const void *data, size_t size,
                                struct sysmon_snapshot *dst)
{
    const u8 *p = data;
    u32 header, feature, length, version, seen = 0;
    size_t required;

    if (IS_ERR_OR_NULL(data) || !size)
        return -ENOKEY;
    while (size) {
        if (size < sizeof(header))
            return -EMSGSIZE;
        memcpy(&header, p, sizeof(header));
        feature = header >> 28;
        length = (header >> 16) & 0xfff;
        version = header & 0xffff;
        if (length < sizeof(header) || length > size)
            return -EMSGSIZE;
        if (feature >= SYSMONSTATS_Q6_LOAD_FEATUREID && feature <= SYSMON_HVX_UTIL_FEATUREID) {
            if (seen & BIT(feature))
                return -EINVAL;
            seen |= BIT(feature);
        }
        switch (feature) {
        case SYSMON_POWER_STATS_FEATUREID:
            required = version >= 2 ? sizeof(dst->power) :
                       offsetof(struct sysmon_smem_power_stats, island_time);
            if (length < required)
                return -EMSGSIZE;
            memcpy(&dst->power, p, min_t(size_t, length, sizeof(dst->power)));
            dst->has_power = true;
            break;
        case SYSMONSTATS_Q6_EVENT_FEATUREID:
            required = version > 1 ? sizeof(dst->event) :
                       sizeof(header) + sizeof(dst->event.event_stats);
            if (length < required)
                return -EMSGSIZE;
            memcpy(&dst->event, p, min_t(size_t, length, sizeof(dst->event)));
            dst->has_event = true;
            break;
        case SYSMONSTATS_Q6_LOAD_FEATUREID:
        case SYSMON_HMX_UTIL_FEATUREID:
        case SYSMON_HVX_UTIL_FEATUREID:
            if (length < sizeof(header) + sizeof(dst->load))
                return -EMSGSIZE;
            if (feature == SYSMONSTATS_Q6_LOAD_FEATUREID) {
                memcpy(&dst->load, p + sizeof(header), sizeof(dst->load));
                dst->has_load = true;
            } else if (feature == SYSMON_HMX_UTIL_FEATUREID) {
                memcpy(&dst->hmx, p + sizeof(header), sizeof(dst->hmx));
                dst->has_hmx = true;
            } else {
                memcpy(&dst->hvx, p + sizeof(header), sizeof(dst->hvx));
                dst->has_hvx = true;
            }
            break;
        default:
            /* Unknown, bounded records remain forward-compatible. */
            break;
        }
        p += length;
        size -= length;
    }
    return 0;
}

static bool sysmon_copy_item(enum dsp_id_t dsp, unsigned int item,
                             void *dst, size_t required)
{
    size_t size = 0;
    const void *data = qcom_smem_get(dsp, item, &size);

    if (IS_ERR_OR_NULL(data) || size < required)
        return false;
    memcpy(dst, data, required);
    return true;
}

static void sysmon_refresh(enum dsp_id_t dsp)
{
    struct sysmon_snapshot parsed = { 0 }, *snap;
    const void *data;
    size_t size = 0;
    int idx = dsp == ADSP ? 0 : dsp == CDSP ? 1 : 2;
    unsigned int sleep_item = dsp == ADSP ? SLEEPSTATS_SMEM_ID_ADSP :
                              dsp == CDSP ? SLEEPSTATS_SMEM_ID_CDSP : SLEEPSTATS_SMEM_ID_SLPI;

    snap = &snapshots[idx];
    memset(snap, 0, sizeof(*snap));
    data = qcom_smem_get(dsp, SYSMON_SMEM_ID, &size);
    /* Do not publish a partially validated malformed feature chain. */
    if (!sysmon_parse_features(data, size, &parsed))
        *snap = parsed;
    snap->has_lpm = sysmon_copy_item(dsp, sleep_item, &snap->lpm, sizeof(snap->lpm));
    if (dsp != CDSP)
        snap->has_lpi = sysmon_copy_item(dsp, SLEEPSTATS_LPI_SMEM_ID, &snap->lpi, sizeof(snap->lpi));
    if (dsp != SLPI)
        snap->has_dsppm = sysmon_copy_item(dsp, DSPPMSTATS_SMEM_ID, &snap->dsppm, sizeof(snap->dsppm));

    if (dsp == ADSP) {
        g_sysmon_stats.sysmon_power_stats_adsp = snap->has_power ? &snap->power : NULL;
        g_sysmon_stats.sysmon_event_stats_adsp = snap->has_event ? &snap->event : NULL;
        g_sysmon_stats.q6_avg_load_adsp = snap->has_load ? &snap->load.q6load_avg : NULL;
        g_sysmon_stats.sleep_stats_adsp = snap->has_lpm ? &snap->lpm : NULL;
        g_sysmon_stats.sleep_lpi_adsp = snap->has_lpi ? &snap->lpi : NULL;
        g_sysmon_stats.dsppm_stats_adsp = snap->has_dsppm ? &snap->dsppm : NULL;
        g_sysmon_stats.smem_init_adsp = true;
    } else if (dsp == CDSP) {
        g_sysmon_stats.sysmon_power_stats_cdsp = snap->has_power ? &snap->power : NULL;
        g_sysmon_stats.sysmon_event_stats_cdsp = snap->has_event ? &snap->event : NULL;
        g_sysmon_stats.q6_avg_load_cdsp = snap->has_load ? &snap->load.q6load_avg : NULL;
        g_sysmon_stats.sleep_stats_cdsp = snap->has_lpm ? &snap->lpm : NULL;
        g_sysmon_stats.dsppm_stats_cdsp = snap->has_dsppm ? &snap->dsppm : NULL;
        g_sysmon_stats.hmx_util = snap->has_hmx ? &snap->hmx.hmx_util_avg : NULL;
        g_sysmon_stats.hvx_util = snap->has_hvx ? &snap->hvx.hvx_util_avg : NULL;
        g_sysmon_stats.smem_init_cdsp = true;
        g_sysmon_stats.smem_init_cdsp_v2 = true;
    } else {
        g_sysmon_stats.sysmon_power_stats_slpi = snap->has_power ? &snap->power : NULL;
        g_sysmon_stats.sysmon_event_stats_slpi = snap->has_event ? &snap->event : NULL;
        g_sysmon_stats.q6_avg_load_slpi = snap->has_load ? &snap->load.q6load_avg : NULL;
        g_sysmon_stats.sleep_stats_slpi = snap->has_lpm ? &snap->lpm : NULL;
        g_sysmon_stats.sleep_lpi_slpi = snap->has_lpi ? &snap->lpi : NULL;
        g_sysmon_stats.smem_init_slpi = true;
    }
}

static void sysmon_smem_init_adsp(void) { sysmon_refresh(ADSP); }
static void sysmon_smem_init_cdsp(void) { sysmon_refresh(CDSP); }
static void sysmon_smem_init_slpi(void) { sysmon_refresh(SLPI); }

/**
 * sysmon_read_hmx_util() - Checks for CDSP power collapse
 * and resets the HMX utilization to zero if dsp is power collapsed.
 */
static u32 sysmon_read_hmx_util(void)
{
	struct sysmon_smem_hmx_stats sysmon_hmx_util;
	u64 curr_timestamp = arch_timer_read_counter();
	u64 last_hmx_update_at = 0;

	memcpy(&sysmon_hmx_util,
		g_sysmon_stats.hmx_util,
		sizeof(struct sysmon_smem_hmx_stats));

	last_hmx_update_at =
		(u64) (((u64)sysmon_hmx_util.Last_update_time_load_msb << 32)|
				sysmon_hmx_util.Last_update_time_load_lsb);

	if ((curr_timestamp > last_hmx_update_at) &&
		((curr_timestamp - last_hmx_update_at) / SYS_CLK_TICKS_PER_MS) > 100) {
		sysmon_hmx_util.hmx_util_avg = 0;
	}

	if (sysmon_hmx_util.hmx_util_avg == HMX_HVX_PMU_EVENTS_NA)
		pr_err("HMX PMU not registered, user ovveride pmu counter\n");

	return sysmon_hmx_util.hmx_util_avg;
}

/**
 * __sysmon_stats_query_hmx_utlization() - * API to query
 * CDSP hmx utlization.On success, returns HMX utilization
 * in the hmx_util parameter.
 * @arg1: u32 pointer to HMX utilization in percentage.
 * @return: SUCCESS (0) if Query is successful
 *        FAILURE (Non-zero) if Query could not be processed, refer error codes.
 */
static int __sysmon_stats_query_hmx_utlization(u32 *hmx_util)
{
	if (!hmx_util)
		return -EINVAL;
	u32 hmx_utilization = 0;
	int ret = 0;
	sysmon_smem_init_cdsp();

	if (!IS_ERR_OR_NULL(g_sysmon_stats.hmx_util)) {
		hmx_utilization = sysmon_read_hmx_util();

		if (hmx_utilization == HMX_HVX_PMU_EVENTS_NA) {
			hmx_utilization = 0;
			ret = DSP_PMU_COUNTER_NA;
		}

		memcpy(hmx_util, &hmx_utilization, sizeof(u32));
	} else
		return -ENOKEY;

	return ret;
}
int sysmon_stats_query_hmx_utlization(u32 *hmx_util)
{
    int ret;
    mutex_lock(&sysmon_lock);
    ret = sysmon_stopping ? -ESHUTDOWN : __sysmon_stats_query_hmx_utlization(hmx_util);
    mutex_unlock(&sysmon_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(sysmon_stats_query_hmx_utlization);
/**
 * sysmon_read_hvx_util() - Checks for CDSP power collapse
 * and resets the HVX utilization to zero if dsp is power collapsed.
 */
static u32 sysmon_read_hvx_util(void)
{
	struct sysmon_smem_hvx_stats sysmon_hvx_util;
	u64 curr_timestamp = arch_timer_read_counter();
	u64 last_hvx_update_at = 0;

	memcpy(&sysmon_hvx_util,
		g_sysmon_stats.hvx_util,
		sizeof(struct sysmon_smem_hvx_stats));

	last_hvx_update_at =
		(u64) (((u64)sysmon_hvx_util.Last_update_time_load_msb << 32)|
				sysmon_hvx_util.Last_update_time_load_lsb);

	if ((curr_timestamp > last_hvx_update_at) &&
		((curr_timestamp - last_hvx_update_at) / SYS_CLK_TICKS_PER_MS) > 100) {
		sysmon_hvx_util.hvx_util_avg = 0;
	}

	if (sysmon_hvx_util.hvx_util_avg == HMX_HVX_PMU_EVENTS_NA)
		pr_err("HVX PMU not registered, user ovveride pmu counter\n");

	return sysmon_hvx_util.hvx_util_avg;
}

/**
 * __sysmon_stats_query_hvx_utlization() - * API to query
 * CDSP subsystem hvx utlization.On success, returns HVX utilization
 * in the hvx_util parameter.
 * @arg1: u32 pointer to HVX utilization in percentage.
 * @return: SUCCESS (0) if Query is successful
 *        FAILURE (Non-zero) if Query could not be processed, refer error codes.
 */
static int __sysmon_stats_query_hvx_utlization(u32 *hvx_util)
{
	if (!hvx_util)
		return -EINVAL;
	u32 hvx_utilization = 0;
	int ret = 0;
	sysmon_smem_init_cdsp();

	if (!IS_ERR_OR_NULL(g_sysmon_stats.hvx_util)) {
		hvx_utilization = sysmon_read_hvx_util();

		if (hvx_utilization == HMX_HVX_PMU_EVENTS_NA) {
			hvx_utilization = 0;
			ret = DSP_PMU_COUNTER_NA;
		}

		memcpy(hvx_util, &hvx_utilization, sizeof(u32));
	} else
		return -ENOKEY;

	return ret;
}
int sysmon_stats_query_hvx_utlization(u32 *hvx_util)
{
    int ret;
    mutex_lock(&sysmon_lock);
    ret = sysmon_stopping ? -ESHUTDOWN : __sysmon_stats_query_hvx_utlization(hvx_util);
    mutex_unlock(&sysmon_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(sysmon_stats_query_hvx_utlization);
/**
 * __sysmon_stats_query_power_residency() - * API to query requested
 * DSP subsystem power residency.On success, returns power residency
 * statistics in the given sysmon_smem_power_stats structure.
 */
static int __sysmon_stats_query_power_residency(enum dsp_id_t dsp_id,
					struct sysmon_smem_power_stats *sysmon_power_stats)
{
	int ret = 0;
	u64 lpi_accumulated = 0;
	u64 lpm_accumulated = 0;
	struct sysmon_smem_power_stats_extended *ptr = NULL, smem_sysmon_power_stats_l = { 0 };

	if (!sysmon_power_stats) {
		pr_err("%s: Null pointer received\n", __func__);
		return -EINVAL;
	}

	switch (dsp_id) {
	case ADSP:
	sysmon_smem_init_adsp();

		if (!IS_ERR_OR_NULL(g_sysmon_stats.sysmon_power_stats_adsp)) {
			ptr = g_sysmon_stats.sysmon_power_stats_adsp;

			memcpy(&smem_sysmon_power_stats_l,
					g_sysmon_stats.sysmon_power_stats_adsp,
					sizeof(smem_sysmon_power_stats_l));

			if (g_sysmon_stats.sleep_stats_adsp) {
				lpm_accumulated = sysmon_accumulated(g_sysmon_stats.sleep_stats_adsp->accumulated, g_sysmon_stats.sleep_stats_adsp->last_entered_at, g_sysmon_stats.sleep_stats_adsp->last_exited_at);
			}

			if (g_sysmon_stats.sleep_lpi_adsp) {
				lpi_accumulated = sysmon_accumulated(g_sysmon_stats.sleep_lpi_adsp->accumulated, g_sysmon_stats.sleep_lpi_adsp->last_entered_at, g_sysmon_stats.sleep_lpi_adsp->last_exited_at);
			}
		} else
			ret = -ENOKEY;
	break;
	case CDSP:
	sysmon_smem_init_cdsp();

		if (!IS_ERR_OR_NULL(g_sysmon_stats.sysmon_power_stats_cdsp)) {
			ptr = g_sysmon_stats.sysmon_power_stats_cdsp;

			memcpy(&smem_sysmon_power_stats_l, g_sysmon_stats.sysmon_power_stats_cdsp,
				sizeof(smem_sysmon_power_stats_l));
			if (g_sysmon_stats.sleep_stats_cdsp) {
				lpm_accumulated = sysmon_accumulated(g_sysmon_stats.sleep_stats_cdsp->accumulated, g_sysmon_stats.sleep_stats_cdsp->last_entered_at, g_sysmon_stats.sleep_stats_cdsp->last_exited_at);
			}
		} else
			ret = -ENOKEY;
	break;
	case SLPI:
	sysmon_smem_init_slpi();

		if (!IS_ERR_OR_NULL(g_sysmon_stats.sysmon_power_stats_slpi)) {
			ptr = g_sysmon_stats.sysmon_power_stats_slpi;

			memcpy(&smem_sysmon_power_stats_l, g_sysmon_stats.sysmon_power_stats_slpi,
				sizeof(smem_sysmon_power_stats_l));
			if (g_sysmon_stats.sleep_stats_slpi) {
				lpm_accumulated = sysmon_accumulated(g_sysmon_stats.sleep_stats_slpi->accumulated, g_sysmon_stats.sleep_stats_slpi->last_entered_at, g_sysmon_stats.sleep_stats_slpi->last_exited_at);
			}
			if (g_sysmon_stats.sleep_lpi_slpi) {
				lpi_accumulated = sysmon_accumulated(g_sysmon_stats.sleep_lpi_slpi->accumulated, g_sysmon_stats.sleep_lpi_slpi->last_entered_at, g_sysmon_stats.sleep_lpi_slpi->last_exited_at);
			}
		} else
			ret = -ENOKEY;
	break;
	default:
		pr_err("%s:Provided subsystem %d is not supported\n", __func__, dsp_id);
		ret = -EINVAL;
	break;
	}

	if (ret == 0) {
		ret = add_delta_time((ptr->powerstats.version) & 0xFF, lpi_accumulated,
				lpm_accumulated, &smem_sysmon_power_stats_l);
		if (ret == 0) {
			memcpy(sysmon_power_stats, &smem_sysmon_power_stats_l.powerstats,
				sizeof(struct sysmon_smem_power_stats));
			sysmon_power_stats->version = (ptr->powerstats.version) & 0xFF;
		}
	}

	return ret;
}
int sysmon_stats_query_power_residency(enum dsp_id_t dsp_id, struct sysmon_smem_power_stats *sysmon_power_stats)
{
    int ret;
    mutex_lock(&sysmon_lock);
    ret = sysmon_stopping ? -ESHUTDOWN : __sysmon_stats_query_power_residency(dsp_id, sysmon_power_stats);
    mutex_unlock(&sysmon_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(sysmon_stats_query_power_residency);
/**
 * API to query requested DSP subsystem's Q6 clock and bandwidth.
 * On success, returns Q6 clock and bandwidth statistics in the given
 * sysmon_smem_q6_event_stats structure.
 */
static int __sysmon_stats_query_q6_votes(enum dsp_id_t dsp_id,
		struct sysmon_smem_q6_event_stats *sysmon_q6_event_stats)
{
	int ret = 0;
	int ver = 0;
	struct sysmon_smem_q6_event_stats_extended *events_ptr = NULL;

	if (!sysmon_q6_event_stats) {
		pr_err("%s: Null pointer received\n", __func__);
		return -EINVAL;
	}

	switch (dsp_id) {
	case ADSP:
	sysmon_smem_init_adsp();

		if (!IS_ERR_OR_NULL(g_sysmon_stats.sysmon_event_stats_adsp)) {
			events_ptr = g_sysmon_stats.sysmon_event_stats_adsp;
			sysmon_q6_event_stats->QDSP6_clk =
				events_ptr->event_stats.QDSP6_clk;
			sysmon_q6_event_stats->Ab_vote_lsb =
				events_ptr->event_stats.Ab_vote_lsb;
			sysmon_q6_event_stats->Ab_vote_msb =
				events_ptr->event_stats.Ab_vote_msb;
			sysmon_q6_event_stats->Ib_vote_lsb =
				events_ptr->event_stats.Ib_vote_lsb;
			sysmon_q6_event_stats->Ib_vote_msb =
				events_ptr->event_stats.Ib_vote_msb;
			sysmon_q6_event_stats->Sleep_latency =
				events_ptr->event_stats.Sleep_latency;
			sysmon_q6_event_stats->HMX_Power_state = -1;
			sysmon_q6_event_stats->HMX_clk = -1;
		}
		else
			ret = -ENOKEY;
	break;
	case CDSP:
	sysmon_smem_init_cdsp();

		if (!IS_ERR_OR_NULL(g_sysmon_stats.sysmon_event_stats_cdsp)) {
			events_ptr = g_sysmon_stats.sysmon_event_stats_cdsp;
			ver = events_ptr->featureId_q6Event & 0xFFFF;
			sysmon_q6_event_stats->QDSP6_clk =
				events_ptr->event_stats.QDSP6_clk;
			sysmon_q6_event_stats->Ab_vote_lsb =
				events_ptr->event_stats.Ab_vote_lsb;
			sysmon_q6_event_stats->Ab_vote_msb =
				events_ptr->event_stats.Ab_vote_msb;
			sysmon_q6_event_stats->Ib_vote_lsb =
				events_ptr->event_stats.Ib_vote_lsb;
			sysmon_q6_event_stats->Ib_vote_msb =
				events_ptr->event_stats.Ib_vote_msb;
			sysmon_q6_event_stats->Sleep_latency =
				events_ptr->event_stats.Sleep_latency;
			if (ver > 1) {
				sysmon_q6_event_stats->HMX_Power_state =
					events_ptr->event_stats_ext.HMX_Power_state;
				sysmon_q6_event_stats->HMX_clk =
					events_ptr->event_stats_ext.HMX_clk;
			} else {
				sysmon_q6_event_stats->HMX_Power_state = -1;
				sysmon_q6_event_stats->HMX_clk = -1;
			}
		}
		else
			ret = -ENOKEY;
	break;
	case SLPI:
	sysmon_smem_init_slpi();

		if (!IS_ERR_OR_NULL(g_sysmon_stats.sysmon_event_stats_slpi)) {
			events_ptr = g_sysmon_stats.sysmon_event_stats_slpi;
			sysmon_q6_event_stats->QDSP6_clk =
				events_ptr->event_stats.QDSP6_clk;
			sysmon_q6_event_stats->Ab_vote_lsb =
				events_ptr->event_stats.Ab_vote_lsb;
			sysmon_q6_event_stats->Ab_vote_msb =
				events_ptr->event_stats.Ab_vote_msb;
			sysmon_q6_event_stats->Ib_vote_lsb =
				events_ptr->event_stats.Ib_vote_lsb;
			sysmon_q6_event_stats->Ib_vote_msb =
				events_ptr->event_stats.Ib_vote_msb;
			sysmon_q6_event_stats->Sleep_latency =
				events_ptr->event_stats.Sleep_latency;
			sysmon_q6_event_stats->HMX_Power_state = -1;
			sysmon_q6_event_stats->HMX_clk = -1;
		}
		else
			ret = -ENOKEY;
	break;
	default:
		pr_err("%s:Provided subsystem %d is not supported\n", __func__, dsp_id);
		ret = -EINVAL;
	break;
	}

	return ret;
}
int sysmon_stats_query_q6_votes(enum dsp_id_t dsp_id, struct sysmon_smem_q6_event_stats *sysmon_q6_event_stats)
{
    int ret;
    mutex_lock(&sysmon_lock);
    ret = sysmon_stopping ? -ESHUTDOWN : __sysmon_stats_query_q6_votes(dsp_id, sysmon_q6_event_stats);
    mutex_unlock(&sysmon_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(sysmon_stats_query_q6_votes);

/*Checks for DSP power collapse and resets the q6 average
 *load to zero if dsp is power collapsed.
 */
static u32 sysmon_read_q6_load(enum dsp_id_t dsp_id)
{
	struct sysmon_smem_q6_load_stats sysmon_q6_load;
	u64 last_q6load_update_at = 0;
	u64 curr_timestamp = arch_timer_read_counter();

	switch (dsp_id) {
	case ADSP:
		memcpy(&sysmon_q6_load,
			g_sysmon_stats.q6_avg_load_adsp,
			sizeof(struct sysmon_smem_q6_load_stats));

		last_q6load_update_at =
			(u64) (((u64)sysmon_q6_load.Last_update_time_load_msb<<32)|
					sysmon_q6_load.Last_update_time_load_lsb);

		if ((curr_timestamp > last_q6load_update_at) &&
			((curr_timestamp - last_q6load_update_at) / SYS_CLK_TICKS_PER_MS) > 100) {
			sysmon_q6_load.q6load_avg = 0;
		}
	break;
	case CDSP:
		memcpy(&sysmon_q6_load,
			g_sysmon_stats.q6_avg_load_cdsp,
			sizeof(struct sysmon_smem_q6_load_stats));

		last_q6load_update_at =
			(u64) (((u64)sysmon_q6_load.Last_update_time_load_msb << 32)|
					sysmon_q6_load.Last_update_time_load_lsb);

		if ((curr_timestamp > last_q6load_update_at) &&
			((curr_timestamp - last_q6load_update_at) / SYS_CLK_TICKS_PER_MS) > 100) {
			sysmon_q6_load.q6load_avg = 0;
		}
	break;
	case SLPI:
		memcpy(&sysmon_q6_load,
			g_sysmon_stats.q6_avg_load_slpi,
			sizeof(struct sysmon_smem_q6_load_stats));

		last_q6load_update_at =
			(u64) (((u64)sysmon_q6_load.Last_update_time_load_msb << 32)|
						sysmon_q6_load.Last_update_time_load_lsb);

		if ((curr_timestamp > last_q6load_update_at) &&
			((curr_timestamp - last_q6load_update_at) / SYS_CLK_TICKS_PER_MS) > 100) {
			sysmon_q6_load.q6load_avg = 0;
		}
	break;
	default:
		pr_err("%s:Provided subsystem %d is not supported\n", __func__, dsp_id);
		return -EINVAL;
	break;
	}

	return sysmon_q6_load.q6load_avg;
}

/**
 * API to query requested DSP subsystem's Q6 load.
 * On success, returns average Q6 load in KCPS for the given
 * q6load_avg parameter.
 */
static int __sysmon_stats_query_q6_load(enum dsp_id_t dsp_id, u32 *q6load_avg)
{
	int ret = 0;
	u32 q6_average_load = 0;

	if (!q6load_avg) {
		pr_err("%s: Null pointer received\n", __func__);
		return -EINVAL;
	}

	switch (dsp_id) {
	case ADSP:
	sysmon_smem_init_adsp();

		if (!IS_ERR_OR_NULL(g_sysmon_stats.q6_avg_load_adsp)) {
			q6_average_load = sysmon_read_q6_load(ADSP);
			memcpy(q6load_avg, &q6_average_load, sizeof(u32));
		} else
			ret = -ENOKEY;
	break;
	case CDSP:
	sysmon_smem_init_cdsp();

		if (!IS_ERR_OR_NULL(g_sysmon_stats.q6_avg_load_cdsp)) {
			q6_average_load = sysmon_read_q6_load(CDSP);
			memcpy(q6load_avg, &q6_average_load, sizeof(u32));
		} else
			ret = -ENOKEY;
	break;
	case SLPI:
	sysmon_smem_init_slpi();

		if (!IS_ERR_OR_NULL(g_sysmon_stats.q6_avg_load_slpi)) {
			q6_average_load = sysmon_read_q6_load(SLPI);
			memcpy(q6load_avg, &q6_average_load, sizeof(u32));
		} else
			ret = -ENOKEY;
	break;
	default:
		pr_err("%s:Provided subsystem %d is not supported\n", __func__, dsp_id);
		ret = -EINVAL;
	break;
	}

	return ret;

}
int sysmon_stats_query_q6_load(enum dsp_id_t dsp_id, u32 *q6load_avg)
{
    int ret;
    mutex_lock(&sysmon_lock);
    ret = sysmon_stopping ? -ESHUTDOWN : __sysmon_stats_query_q6_load(dsp_id, q6load_avg);
    mutex_unlock(&sysmon_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(sysmon_stats_query_q6_load);
/*
 * API to query requested DSP subsystem sleep stats for
 * LPM and LPI.On success, returns sleep
 * statistics in the given sleep_stats structure for LPM and
 * LPI(on supported subsystems).
 */
static int __sysmon_stats_query_sleep(enum dsp_id_t dsp_id,
                              struct sleep_stats *sleep_stats_lpm,
                              struct sleep_stats_island *sleep_stats_lpi)
{
    struct sysmon_snapshot *snap;
    struct sleep_stats lpm;
    struct sleep_stats_island lpi;
    int idx;

    if (!sleep_stats_lpm && !sleep_stats_lpi)
        return -EINVAL;
    if (dsp_id != ADSP && dsp_id != CDSP && dsp_id != SLPI)
        return -EINVAL;
    if (dsp_id == CDSP && sleep_stats_lpi)
        return -EOPNOTSUPP;
    idx = dsp_id == ADSP ? 0 : dsp_id == CDSP ? 1 : 2;
    sysmon_refresh(dsp_id);
    snap = &snapshots[idx];
    if ((sleep_stats_lpm && !snap->has_lpm) || (sleep_stats_lpi && !snap->has_lpi))
        return -ENOKEY;
    lpm = snap->lpm;
    lpi = snap->lpi;
    if (sleep_stats_lpm) {
        lpm.accumulated = sysmon_accumulated(lpm.accumulated, lpm.last_entered_at, lpm.last_exited_at);
        *sleep_stats_lpm = lpm;
    }
    if (sleep_stats_lpi) {
        lpi.accumulated = sysmon_accumulated(lpi.accumulated, lpi.last_entered_at, lpi.last_exited_at);
        *sleep_stats_lpi = lpi;
    }
    return 0;
}
int sysmon_stats_query_sleep(enum dsp_id_t dsp_id, struct sleep_stats *sleep_stats_lpm, struct sleep_stats_island *sleep_stats_lpi)
{
    int ret;
    mutex_lock(&sysmon_lock);
    ret = sysmon_stopping ? -ESHUTDOWN : __sysmon_stats_query_sleep(dsp_id, sleep_stats_lpm, sleep_stats_lpi);
    mutex_unlock(&sysmon_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(sysmon_stats_query_sleep);

static int __master_adsp_stats_show(struct seq_file *s, void *d)
{
	int i = 0, j = 0, ret = 0;
	u64 lpi_accumulated = 0;
	u64 lpm_accumulated = 0;
	u32 q6_load;
	u8 ver = 0;
	struct sysmon_smem_power_stats_extended *ptr = NULL, sysmon_power_stats = { 0 };
	struct sysmon_smem_q6_event_stats_extended *events_ptr = NULL;
	sysmon_smem_init_adsp();

	if (g_sysmon_stats.sysmon_event_stats_adsp) {
		events_ptr = g_sysmon_stats.sysmon_event_stats_adsp;
		seq_puts(s, "\nsysMon stats:\n\n");
		seq_printf(s, "Core clock(KHz): %d\n",
				events_ptr->event_stats.QDSP6_clk);
		seq_printf(s, "Ab vote(Bytes): %llu\n",
				(((u64)events_ptr->event_stats.Ab_vote_msb << 32) |
					   events_ptr->event_stats.Ab_vote_lsb));
		seq_printf(s, "Ib vote(Bytes): %llu\n",
				(((u64)events_ptr->event_stats.Ib_vote_msb << 32) |
					   events_ptr->event_stats.Ib_vote_lsb));
		seq_printf(s, "Sleep latency(usec): %u\n",
				events_ptr->event_stats.Sleep_latency > 0 ?
				events_ptr->event_stats.Sleep_latency : U32_MAX);
	}

	if (g_sysmon_stats.dsppm_stats_adsp) {
		seq_puts(s, "\nDSPPM stats:\n\n");
		seq_printf(s, "Version: %u\n", g_sysmon_stats.dsppm_stats_adsp->version);
		seq_printf(s, "Sleep latency(usec): %u\n",
					g_sysmon_stats.dsppm_stats_adsp->latency_us ?
					g_sysmon_stats.dsppm_stats_adsp->latency_us : U32_MAX);
		seq_printf(s, "Timestamp: %u\n", g_sysmon_stats.dsppm_stats_adsp->timestamp);

		for (; i < DSPPMSTATS_NUMPD; i++) {
			seq_printf(s, "Pid: %d, Num active clients: %d\n",
						g_sysmon_stats.dsppm_stats_adsp->pd[i].pid,
						g_sysmon_stats.dsppm_stats_adsp->pd[i].num_active);
		}
	}

	if (g_sysmon_stats.sleep_stats_adsp) {
		lpm_accumulated = sysmon_accumulated(g_sysmon_stats.sleep_stats_adsp->accumulated, g_sysmon_stats.sleep_stats_adsp->last_entered_at, g_sysmon_stats.sleep_stats_adsp->last_exited_at);

		seq_puts(s, "\nLPM stats:\n\n");
		seq_printf(s, "Count = %u\n", g_sysmon_stats.sleep_stats_adsp->count);
		seq_printf(s, "Last Entered At = %llu\n",
			g_sysmon_stats.sleep_stats_adsp->last_entered_at);
		seq_printf(s, "Last Exited At = %llu\n",
			g_sysmon_stats.sleep_stats_adsp->last_exited_at);
		seq_printf(s, "Accumulated Duration = %llu\n", lpm_accumulated);
	}

	if (g_sysmon_stats.sleep_lpi_adsp) {
		lpi_accumulated = sysmon_accumulated(g_sysmon_stats.sleep_lpi_adsp->accumulated, g_sysmon_stats.sleep_lpi_adsp->last_entered_at, g_sysmon_stats.sleep_lpi_adsp->last_exited_at);

		seq_puts(s, "\nLPI stats:\n\n");
		seq_printf(s, "Count = %u\n", g_sysmon_stats.sleep_lpi_adsp->count);
		seq_printf(s, "Last Entered At = %llu\n",
			g_sysmon_stats.sleep_lpi_adsp->last_entered_at);
		seq_printf(s, "Last Exited At = %llu\n",
			g_sysmon_stats.sleep_lpi_adsp->last_exited_at);
		seq_printf(s, "Accumulated Duration = %llu\n",
			lpi_accumulated);
	}

	if (g_sysmon_stats.sysmon_power_stats_adsp) {
		memcpy(&sysmon_power_stats, g_sysmon_stats.sysmon_power_stats_adsp,
				sizeof(struct sysmon_smem_power_stats_extended));
		ptr = g_sysmon_stats.sysmon_power_stats_adsp;
		ver = (ptr->powerstats.version) & 0xFF;

		ret = add_delta_time(ver, lpi_accumulated,
				lpm_accumulated, &sysmon_power_stats);

		if (ret != 0)
			seq_puts(s, "\nWarning: Power Stats are might be Invalid\n");

		seq_puts(s, "\nPower Stats:\n\n");
		for (j = 0; j < SYSMON_POWER_STATS_MAX_CLK_LEVELS; j++) {
			if (sysmon_power_stats.powerstats.clk_arr[j]) {
				if (ver >= 2) {
					seq_printf(s, "%u : Core Clock(KHz) : %u \tActive Time(sec) : %u \tLPI time(sec) : %u\n",
						j,
						sysmon_power_stats.powerstats.clk_arr[j],
						sysmon_power_stats.powerstats.active_time[j],
						sysmon_power_stats.powerstats.island_time[j]);
				} else {
					seq_printf(s, "%u : Core Clock(KHz): %u \tActive Time(sec): %u\n",
						j,
						sysmon_power_stats.powerstats.clk_arr[j],
						sysmon_power_stats.powerstats.active_time[j]);
				}
			}
		}

		seq_printf(s, "Power collapse time(sec) = %u\n",
			sysmon_power_stats.powerstats.pc_time);
		seq_printf(s, "Total LPI time(sec) = %u\n",
			sysmon_power_stats.powerstats.lpi_time);
		if (ver >= 2)
			seq_printf(s, "Current core clock(KHz) = %u\n",
				sysmon_power_stats.powerstats.current_clk);
	}

	if (g_sysmon_stats.q6_avg_load_adsp) {
		seq_puts(s, "\nQ6 load:\n\n");
		q6_load = sysmon_read_q6_load(ADSP);
		seq_printf(s, "Average Q6 load in KCPS = %u\n", q6_load);
	}

	return 0;
}
static int master_adsp_stats_show(struct seq_file *seq, void *data)
{
    int ret;
    mutex_lock(&sysmon_lock);
    ret = sysmon_stopping ? -ESHUTDOWN : __master_adsp_stats_show(seq, data);
    mutex_unlock(&sysmon_lock);
    return ret;
}
DEFINE_SHOW_ATTRIBUTE(master_adsp_stats);

static int __master_cdsp_stats_show(struct seq_file *s, void *d)
{
	int i = 0, j = 0, ret = 0;
	u32 hmx_util, hvx_util;
	u32 q6_load;
	u64 lpm_accumulated = 0;
	u8 ver = 0;
	struct sysmon_smem_power_stats_extended *ptr = NULL, sysmon_power_stats = { 0 };
	struct sysmon_smem_q6_event_stats_extended *events_ptr = NULL;
	sysmon_smem_init_cdsp();

	if (g_sysmon_stats.sysmon_event_stats_cdsp) {
		events_ptr = g_sysmon_stats.sysmon_event_stats_cdsp;
		ver = events_ptr->featureId_q6Event & 0xFFFF;
		seq_puts(s, "\nsysMon stats:\n\n");
		seq_printf(s, "Core clock(KHz): %d\n",
				events_ptr->event_stats.QDSP6_clk);
		seq_printf(s, "Ab vote(Bytes): %llu\n",
			(((u64)events_ptr->event_stats.Ab_vote_msb << 32) |
					   events_ptr->event_stats.Ab_vote_lsb));
		seq_printf(s, "Ib vote(Bytes): %llu\n",
			(((u64)events_ptr->event_stats.Ib_vote_msb << 32) |
					   events_ptr->event_stats.Ib_vote_lsb));
		seq_printf(s, "Sleep latency(usec): %u\n",
			events_ptr->event_stats.Sleep_latency > 0 ?
			events_ptr->event_stats.Sleep_latency : U32_MAX);
		if ((ver > 1) &&
			(events_ptr->event_stats_ext.HMX_Power_state >= 0)) {
			seq_printf(s, "HMX Power state: %s\n",
				events_ptr->event_stats_ext.HMX_Power_state ? "ON" : "OFF");
			seq_printf(s, "HMX Clock(KHz): %d\n",
				events_ptr->event_stats_ext.HMX_clk);
		}
	}

	if (g_sysmon_stats.dsppm_stats_cdsp) {
		seq_puts(s, "\nDSPPM stats:\n\n");
		seq_printf(s, "Version: %u\n",
			g_sysmon_stats.dsppm_stats_cdsp->version);
		seq_printf(s, "Sleep latency(usec): %u\n",
			g_sysmon_stats.dsppm_stats_cdsp->latency_us ?
			g_sysmon_stats.dsppm_stats_cdsp->latency_us : U32_MAX);
		seq_printf(s, "Timestamp: %u\n",
			g_sysmon_stats.dsppm_stats_cdsp->timestamp);

		for (; i < DSPPMSTATS_NUMPD; i++) {
			seq_printf(s, "Pid: %d, Num active clients: %d\n",
						g_sysmon_stats.dsppm_stats_cdsp->pd[i].pid,
						g_sysmon_stats.dsppm_stats_cdsp->pd[i].num_active);
		}
	}

	if (g_sysmon_stats.sleep_stats_cdsp) {
		lpm_accumulated = sysmon_accumulated(g_sysmon_stats.sleep_stats_cdsp->accumulated, g_sysmon_stats.sleep_stats_cdsp->last_entered_at, g_sysmon_stats.sleep_stats_cdsp->last_exited_at);

		seq_puts(s, "\nLPM stats:\n\n");
		seq_printf(s, "Count = %u\n",
			g_sysmon_stats.sleep_stats_cdsp->count);
		seq_printf(s, "Last Entered At = %llu\n",
			g_sysmon_stats.sleep_stats_cdsp->last_entered_at);
		seq_printf(s, "Last Exited At = %llu\n",
			g_sysmon_stats.sleep_stats_cdsp->last_exited_at);
		seq_printf(s, "Accumulated Duration = %llu\n", lpm_accumulated);
	}

	if (g_sysmon_stats.sysmon_power_stats_cdsp) {
		memcpy(&sysmon_power_stats, g_sysmon_stats.sysmon_power_stats_cdsp,
				sizeof(struct sysmon_smem_power_stats_extended));
		ptr = g_sysmon_stats.sysmon_power_stats_cdsp;
		ver = (ptr->powerstats.version) & 0xFF;
		ret = add_delta_time(ver, 0, lpm_accumulated, &sysmon_power_stats);

		if (ret)
			seq_puts(s, "\nWarning: Power Stats might be Invalid\n");

		seq_puts(s, "\nPower Stats:\n\n");
		for (j = 0; j < SYSMON_POWER_STATS_MAX_CLK_LEVELS; j++) {
			if (sysmon_power_stats.powerstats.clk_arr[j])
				seq_printf(s, "%u : Core Clock(KHz) : %u \tActive Time(sec) : %u\n",
					j,
					sysmon_power_stats.powerstats.clk_arr[j],
					sysmon_power_stats.powerstats.active_time[j]);
		}
		seq_printf(s, "Power collapse time(sec) = %u\n",
			sysmon_power_stats.powerstats.pc_time);
		seq_printf(s, "Total LPI time(sec) = %u\n",
			sysmon_power_stats.powerstats.lpi_time);

		if (ver >= 2)
			seq_printf(s, "Current core clock(KHz) = %u\n",
				sysmon_power_stats.powerstats.current_clk);

		if (ret)
			return ret;

	}

	if (g_sysmon_stats.q6_avg_load_cdsp) {
		seq_puts(s, "\nQ6 load:\n\n");
		q6_load = sysmon_read_q6_load(CDSP);
		seq_printf(s, "Average Q6 load in KCPS = %u\n", q6_load);
	}

	ret = __sysmon_stats_query_hmx_utlization(&hmx_util);

	if (ret) {
		seq_printf(s, "\nHMX stats not available, error code: %d\n", ret);
	} else {
		seq_puts(s, "\nHMX stats:\n\n");
		seq_printf(s, "HMX utilization in percentage = %u\n", hmx_util);
	}

	ret = __sysmon_stats_query_hvx_utlization(&hvx_util);

	if (ret) {
		seq_printf(s, "\nHVX stats not available, error code: %d\n", ret);
	} else {
		seq_puts(s, "\nHVX Stats:\n\n");
		seq_printf(s, "HVX utilization in percentage = %u\n", hvx_util);
	}

	return 0;
}

static int master_cdsp_stats_show(struct seq_file *seq, void *data)
{
    int ret;
    mutex_lock(&sysmon_lock);
    ret = sysmon_stopping ? -ESHUTDOWN : __master_cdsp_stats_show(seq, data);
    mutex_unlock(&sysmon_lock);
    return ret;
}
DEFINE_SHOW_ATTRIBUTE(master_cdsp_stats);

static int __init sysmon_stats_init(void)
{
    struct dentry *dir, *file;
    int ret;

    dir = debugfs_create_dir("sysmon_subsystem_stats", NULL);
    if (IS_ERR_OR_NULL(dir))
        return dir ? PTR_ERR(dir) : -ENOMEM;
    g_sysmon_stats.debugfs_dir = dir;
    file = debugfs_create_file("master_adsp_stats", 0444, dir, NULL, &master_adsp_stats_fops);
    if (IS_ERR_OR_NULL(file))
        goto failed;
    g_sysmon_stats.debugfs_master_adsp_stats = file;
    file = debugfs_create_file("master_cdsp_stats", 0444, dir, NULL, &master_cdsp_stats_fops);
    if (IS_ERR_OR_NULL(file))
        goto failed;
    g_sysmon_stats.debugfs_master_cdsp_stats = file;
    return 0;
failed:
    ret = file ? PTR_ERR(file) : -ENOMEM;
    debugfs_remove_recursive(dir);
    memset(&g_sysmon_stats, 0, sizeof(g_sysmon_stats));
    return ret;
}

static void __exit sysmon_stats_exit(void)
{
    debugfs_remove_recursive(g_sysmon_stats.debugfs_dir);
    mutex_lock(&sysmon_lock);
    sysmon_stopping = true;
    memset(&g_sysmon_stats, 0, sizeof(g_sysmon_stats));
    memset(snapshots, 0, sizeof(snapshots));
    mutex_unlock(&sysmon_lock);
}

module_init(sysmon_stats_init);
module_exit(sysmon_stats_exit);


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. Sysmon subsystem Stats driver");

MODULE_SOFTDEP("pre: smem");
