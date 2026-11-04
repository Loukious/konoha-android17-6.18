// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 *
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/soc/qcom/qcom_aoss.h>
#include <soc/qcom/qcom_stats.h>

#define MAX_QMP_MSG_SIZE	96
#define MODE_AOSS		0xaa
#define MODE_CXPC		0xcc
#define MODE_DDR		0xdd
#define MODE_STR(m)		(m == MODE_CXPC ? "CXPC" :	\
				(m == MODE_AOSS ? "AOSS" :	\
				(m == MODE_DDR  ? "DDR"  : "")))

#define VX_MODE_MASK_TYPE		0xFF
#define VX_MODE_MASK_LOGSIZE		0xFF
#define VX_MODE_SHIFT_LOGSIZE		8
#define VX_FLAG_MASK_DUR		0xFFFF
#define VX_FLAG_MASK_TS			0xFF
#define VX_FLAG_SHIFT_TS		16
#define VX_FLAG_MASK_FLUSH_THRESH	0xFF
#define VX_FLAG_SHIFT_FLUSH_THRESH	24

#define DEFAULT_DEBUG_TIME (10 * 1000)
#define DEFAULT_TIMER (5000)
#define MAX_DRV_NAMES	27

struct vx_header {
	struct {
		u16 unused;
		u8 logsize;
		u8 type;
	} mode;
	struct {
		u8 flush_threshold;
		u8 ts_shift;
		u16 dur_ms;
	} flags;
};

struct vx_data {
	u64 ts;
	u32 *drv_vx;
};

struct vx_log {
	struct vx_header header;
	struct vx_data *data;
	int loglines;
};

struct vx_platform_data {
	void __iomem *base;
	resource_size_t size;
	struct dentry *vx_dir;
	size_t n_cxpc_drv;
	size_t n_aoss_drv;
	const char * const *cxpc_drvs;
	const char * const *aoss_drvs;
	struct mutex lock;
	struct qmp *qmp;
	ktime_t suspend_time;
	ktime_t resume_time;
	bool debug_enable;
	u32 detect_time_ms;
	u32 timer_ms;
	int monitor_status;
	bool debug_dump_enable;
};

static struct vx_platform_data *g_pd;
/* Exports hold this lock until all uses of the published instance finish. */
static DEFINE_MUTEX(vx_global_lock);

void trigger_dump_enable(void);
void trigger_dump_disable(void);

enum {
	CXPC_DRV_NAME,
	AOSS_DRV_NAME,
};

enum {
	DISABLE_MSG,
	CXPC_MSG,
	AOSS_SHUTDOWN_MSG,
};

static const char * const drv_names_kalama[][MAX_DRV_NAMES] = {
	[CXPC_DRV_NAME] = {"TZ", "HYP", "HLOS", "L3", "SECPROC", "AUDIO", "AOP", "DEBUG",
			"GPU", "DISPLAY", "COMPUTE_DSP", "TME_SW", "TME_HW", "MDM SW",
			"MDM HW", "WLAN RF", "WLAN BB", "CAM_IFE0", "CAM_IFE1", "CAM_IFE2",
			"DDR AUX", "ARC CPRF", ""},
	[AOSS_DRV_NAME] = {"APPS", "SP", "AUDIO", "AOP", "DEBUG", "GPU", "DISPLAY", "COMPUTE",
			"TME", "MODEM", "WLAN RF", "WLAN BB", "CAM", ""},
};

static const char * const drv_names_pineapple[][MAX_DRV_NAMES] = {
	[CXPC_DRV_NAME] = {"TZ", "L3", "HLOS", "HYP", "SECPROC", "AUDIO", "AOP", "DEBUG",
			"GPU", "DISPLAY", "COMPUTE_DSP", "TME_HW", "TME_SW", "MDM SW",
			"MDM HW", "MDM Q6 CESTA", "WLAN RF", "WLAN BB", "CAM_IFE0 CESTA",
			"CAM_IFE1", "CAM_IFE2", "PCI0 CESTA", "PCI1 CESTA",
			"DDR AUX", "ARC CPRF", ""},
	[AOSS_DRV_NAME] = {"APPS", "SP", "AUDIO", "AOP", "DEBUG", "GPU", "DISPLAY", "COMPUTE",
			"TME", "MODEM", "WLAN RF", "WLAN BB", "CAM", "PCIE", ""},
};

static const char * const drv_names_sun[][MAX_DRV_NAMES] = {
	[CXPC_DRV_NAME] = {"TZ", "L3", "HLOS", "HYP", "SECPROC", "AUDIO", "AUDIO CESTA", "AOP",
			"DEBUG", "GPU", "DISPLAY", "COMPUTE_DSP", "TME_HW", "TME_SW", "MDM SW",
			"MDM HW", "MDM Q6 CESTA", "WLAN RF", "WLAN BB", "CAM_IFE0 CESTA",
			"CAM_IFE1", "CAM_IFE2", "PCI0 CESTA", "MM CESTA",
			"DDR AUX", "ARC CPRF", ""},
	[AOSS_DRV_NAME] = {"APPS", "SP", "AUDIO", "AOP", "DEBUG", "GPU", "DISPLAY", "COMPUTE",
			"TME", "MODEM", "WLAN RF", "WLAN BB", "CAM", "PCIE", "MM", ""},
};

static const char * const drv_names_tuna[][MAX_DRV_NAMES] = {
	[CXPC_DRV_NAME] = {"TZ", "L3", "HLOS", "HYP", "WPSS", "AUDIO", "AUDIO CESTA", "AOP",
			"DEBUG", "GPU", "DISPLAY", "COMPUTE_DSP", "TME_HW", "TME_SW", "MDM SW",
			"MDM HW", "MDM Q6 CESTA", "WLAN RF", "WLAN BB", "CAM_IFE0 CESTA",
			"CAM_IFE1", "CAM_IFE2", "PCI0 CESTA", "MM CESTA",
			"DDR AUX", "ARC CPRF", ""},
	[AOSS_DRV_NAME] = {"APPS", "WPSS", "AUDIO", "AOP", "DEBUG", "GPU", "DISPLAY", "COMPUTE",
			"TME", "MODEM", "WLAN RF", "WLAN BB", "CAM", "PCIE", "MM", ""},
};

static const char * const drv_names_kera[][MAX_DRV_NAMES] = {
	[CXPC_DRV_NAME] = {"TZ", "L3", "HLOS", "HYP", "WPSS", "AUDIO", "AUDIO CESTA", "AOP",
			"DEBUG", "GPU", "DISPLAY", "COMPUTE_DSP", "TME_HW", "TME_SW", "MDM SW",
			"MDM HW", "MDM Q6 CESTA", "WLAN RF", "WLAN BB", "CAM_IFE0 CESTA",
			"CAM_IFE1", "CAM_IFE2", "PCI0 CESTA", "MM CESTA",
			"DDR AUX", "ARC CPRF", ""},
	[AOSS_DRV_NAME] = {"APPS", "WPSS", "AUDIO", "AOP", "DEBUG", "GPU", "DISPLAY", "COMPUTE",
			"TME", "MODEM", "WLAN RF", "WLAN BB", "CAM", "PCIE", "MM", ""},
};

static const char * const drv_names_sdxkova[][MAX_DRV_NAMES] = {
	[CXPC_DRV_NAME] = {"TZ", "L3", "HLOS", "HYP", "AOP", "DEBUG", "TME_HW", "TME_SW",
			"MDM SW", "MDM HW", "MDM_CESTA", "PCIe_CESTA0", "PCIe_CESTA1",
			"PCIe_CESTA2", "WLAN RF", "WLAN BB", "DDR AUX", "ARC CPRF",
			""},
	[AOSS_DRV_NAME] = {"APPS", "AOP", "DEBUG", "TME", "MODEM", "PCIE", "WLAN RF",
			"WLAN BB", ""},
};

static ssize_t debug_time_ms_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct vx_platform_data *pd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(pd->detect_time_ms));
}

static ssize_t debug_time_ms_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct vx_platform_data *pd = dev_get_drvdata(dev);
	int val;

	if (kstrtos32(buf, 0, &val))
		return -EINVAL;

	if (val <= 0) {
		pr_err("debug time ms should be greater than zero\n");
		return -EINVAL;
	}

	mutex_lock(&pd->lock);
	pd->detect_time_ms = val;
	mutex_unlock(&pd->lock);

	return count;
}
static DEVICE_ATTR_RW(debug_time_ms);

static ssize_t set_timer_ms_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct vx_platform_data *pd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(pd->timer_ms));
}

static ssize_t set_timer_ms_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct vx_platform_data *pd = dev_get_drvdata(dev);
	int val;

	if (kstrtos32(buf, 0, &val))
		return -EINVAL;

	if (val <= 0) {
		pr_err("timer ms should be greater than zero\n");
		return -EINVAL;
	}

	mutex_lock(&pd->lock);
	pd->timer_ms = val;
	mutex_unlock(&pd->lock);

	return count;
}
static DEVICE_ATTR_RW(set_timer_ms);

static ssize_t debug_enable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct vx_platform_data *pd = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(pd->debug_enable));
}

static ssize_t debug_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct vx_platform_data *pd = dev_get_drvdata(dev);
	int val;

	if (kstrtos32(buf, 0, &val))
		return -EINVAL;

	if ((val < 0) || (val > 1)) {
		pr_err("input error\n");
		return -EINVAL;
	}

	mutex_lock(&pd->lock);
	pd->debug_enable = val;
	subsystem_sleep_debug_enable(pd->debug_enable);
	mutex_unlock(&pd->lock);

	return count;
}
static DEVICE_ATTR_RW(debug_enable);

/* Callers hold pd->lock for monitor state and QMP transactions. */
static void trigger_dump(struct vx_platform_data *pd)
{
	int ret = 0;
	char buf[MAX_QMP_MSG_SIZE] = {};

	if (!pd->debug_dump_enable)
		return;

	scnprintf(buf, sizeof(buf),
			"{class: misc_debug, res: do_crash_dump, tmr: %d}", pd->timer_ms);

	ret = qmp_send(pd->qmp, "%s", buf);
	if (ret)
		pr_err("Error sending qmp message: %d\n", ret);

}

static void sys_pm_vx_send_msg(struct vx_platform_data *pd)
{
	int ret = 0;
	char buf[MAX_QMP_MSG_SIZE] = {};

	switch (pd->monitor_status) {
	case AOSS_SHUTDOWN_MSG:
		scnprintf(buf, sizeof(buf),
			"{class: lpm_mon, type: rbsc, dur: 1000, flush: 5, ts_adj: 1}");
		break;
	case CXPC_MSG:
		scnprintf(buf, sizeof(buf),
			"{class: lpm_mon, type: cxpc, dur: 1000, flush: 5, ts_adj: 1}");
		break;
	case DISABLE_MSG:
		scnprintf(buf, sizeof(buf),
			"{class: lpm_mon, type: cxpc, dur: 1000, flush: 1, log_once: 1}");
		break;
	default:
		return;
	}

	ret = qmp_send(pd->qmp, "%s", buf);
	if (ret)
		pr_err("Error sending qmp message: %d\n", ret);

}

static const char * const *vx_get_drvs_info(u8 type, struct vx_platform_data *pd,
				      size_t *ndrvs)
{
	const char * const *drvs;

	switch (type) {
	case MODE_CXPC:
	case MODE_DDR:
		*ndrvs = pd->n_cxpc_drv;
		drvs = pd->cxpc_drvs;
		break;
	case MODE_AOSS:
		*ndrvs = pd->n_aoss_drv;
		drvs = pd->aoss_drvs;
		break;
	default:
		return NULL;
	}

	return drvs;
}

static void free_vx_data(struct vx_log *log)
{
	int i;

	for (i = 0; i < log->loglines; i++)
		kfree(log->data[i].drv_vx);
	kfree(log->data);
	log->data = NULL;
	log->loglines = 0;
}

static int read_vx_data(struct vx_platform_data *pd, struct vx_log *log)
{
	struct vx_header *hdr = &log->header;
	const char * const *drvs;
	size_t n_drv, words, extent;
	u32 mode, flags, val, itr = 8, timestamp;
	int i, j, k, ret;

	memset(log, 0, sizeof(*log));
	if (pd->size < 8)
		return -EMSGSIZE;
	mode = readl_relaxed(pd->base);
	if (!mode)
		return -ENOENT;
	hdr->mode.type = mode & VX_MODE_MASK_TYPE;
	hdr->mode.logsize = (mode >> VX_MODE_SHIFT_LOGSIZE) & VX_MODE_MASK_LOGSIZE;
	drvs = vx_get_drvs_info(hdr->mode.type, pd, &n_drv);
	if (!drvs || !n_drv || n_drv > MAX_DRV_NAMES)
		return -ENODEV;
	if (!hdr->mode.logsize)
		return -EINVAL;
	flags = readl_relaxed(pd->base + 4);
	if (!flags)
		return -ENOENT;
	hdr->flags.dur_ms = flags & VX_FLAG_MASK_DUR;
	hdr->flags.ts_shift = (flags >> VX_FLAG_SHIFT_TS) & VX_FLAG_MASK_TS;
	hdr->flags.flush_threshold = (flags >> VX_FLAG_SHIFT_FLUSH_THRESH) & VX_FLAG_MASK_FLUSH_THRESH;
	/* Native timestamps are 32-bit words; retain all shifted bits in u64. */
	if (hdr->flags.ts_shift >= 32)
		return -EINVAL;
	words = DIV_ROUND_UP(n_drv, 4);
	extent = 8 + hdr->mode.logsize * (sizeof(u32) + words * sizeof(u32));
	if (extent > pd->size)
		return -EMSGSIZE;
	log->data = kcalloc(hdr->mode.logsize, sizeof(*log->data), GFP_KERNEL);
	if (!log->data)
		return -ENOMEM;
	for (i = 0; i < hdr->mode.logsize; i++) {
		timestamp = readl_relaxed(pd->base + itr);
		itr += sizeof(u32);
		if (!timestamp)
			break;
		log->data[i].ts = (u64)timestamp << hdr->flags.ts_shift;
		log->data[i].drv_vx = kcalloc(n_drv, sizeof(u32), GFP_KERNEL);
		if (!log->data[i].drv_vx) {
			ret = -ENOMEM;
			goto fail;
		}
		log->loglines++;
		for (j = 0; j < words; j++) {
			val = readl_relaxed(pd->base + itr);
			itr += sizeof(u32);
			for (k = 0; k < 4 && 4 * j + k < n_drv; k++)
				log->data[i].drv_vx[4 * j + k] = (val >> (8 * k)) & 0xff;
		}
	}
	/* Detect changed headers; the firmware has no whole-log sequence lock. */
	if (readl(pd->base) != mode || readl(pd->base + 4) != flags) {
		ret = -EAGAIN;
		goto fail;
	}
	return 0;
fail:
	free_vx_data(log);
	return ret;
}

static void vx_check_drv(struct vx_platform_data *pd)
{
	struct vx_log log;
	const char * const *drvs;
	size_t n_drv;
	int i, j, ret;

	ret = read_vx_data(pd, &log);
	if (ret) {
		pr_err("fail to read vx data: %d\n", ret);
		return;
	}
	drvs = vx_get_drvs_info(log.header.mode.type, pd, &n_drv);
	for (i = 0; i < n_drv; i++) {
		for (j = 0; j < log.loglines; j++) {
			if (!log.data[j].drv_vx[i])
				break;
			if (j == log.loglines - 1) {
				pr_warn("DRV: %s has blocked power collapse\n", drvs[i]);
				trigger_dump(pd);
			}
		}
	}
	free_vx_data(&log);
}

static void show_vx_data(struct vx_platform_data *pd, struct vx_log *log,
			 struct seq_file *seq)
{
	int i, j;
	struct vx_header *hdr = &log->header;
	struct vx_data *data;
	u32 prev;
	bool from_exit = false;
	const char * const *drvs;
	size_t ndrv;

	drvs = vx_get_drvs_info(hdr->mode.type, pd, &ndrv);
	if (!drvs || !ndrv)
		return;

	seq_printf(seq, "Mode           : %s\n"
			"Duration (ms)  : %u\n"
			"Time Shift     : %u\n"
			"Flush Threshold: %u\n"
			"Max Log Entries: %u\n",
			MODE_STR(hdr->mode.type),
			hdr->flags.dur_ms,
			hdr->flags.ts_shift,
			hdr->flags.flush_threshold,
			hdr->mode.logsize);

	seq_puts(seq, "Timestamp|");

	for (i = 0; i < ndrv; i++)
		seq_printf(seq, "%*s|", 8, drvs[i]);
	seq_puts(seq, "\n");

	for (i = 0; i < log->loglines; i++) {
		data = &log->data[i];
		seq_printf(seq, "%*llx|", 9, data->ts);
		/* An all-zero line indicates we entered LPM */
		for (j = 0, prev = data->drv_vx[0]; j < ndrv; j++)
			prev |= data->drv_vx[j];
		if (!prev) {
			if (!from_exit) {
				seq_printf(seq, "%s Enter\n", MODE_STR(hdr->mode.type));
				from_exit = true;
			} else {
				seq_printf(seq, "%s Exit\n", MODE_STR(hdr->mode.type));
				from_exit = false;
			}
			continue;
		}
		for (j = 0; j < ndrv; j++)
			seq_printf(seq, "%*u|", 8, data->drv_vx[j]);
		seq_puts(seq, "\n");
	}
}

static int vx_show(struct seq_file *seq, void *data)
{
	struct vx_platform_data *pd = seq->private;
	struct vx_log log;
	int ret;

	mutex_lock(&pd->lock);
	ret = read_vx_data(pd, &log);
	if (!ret) {
		show_vx_data(pd, &log, seq);
		free_vx_data(&log);
	}
	mutex_unlock(&pd->lock);
	return ret;
}

static int open_vx(struct inode *inode, struct file *file)
{
	return single_open(file, vx_show, inode->i_private);
}

static const struct file_operations sys_pm_vx_fops = {
	.owner = THIS_MODULE,
	.open = open_vx,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int trigger_dump_enable_get(void *data, u64 *val)
{
	struct vx_platform_data *pd = data;

	mutex_lock(&pd->lock);
	*val = (u64)pd->debug_dump_enable;
	mutex_unlock(&pd->lock);

	return 0;
}

static int trigger_dump_enable_set(void *data, u64 val)
{
	struct vx_platform_data *pd = data;

	mutex_lock(&pd->lock);
	if (val > 1) {
		mutex_unlock(&pd->lock);
		return -EINVAL;
	}
	pd->debug_dump_enable = val;
	mutex_unlock(&pd->lock);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(trigger_dump_enable_fops,
			trigger_dump_enable_get,
			trigger_dump_enable_set,
			"%lld\n");

static void set_global_dump_enable(bool enable)
{
	mutex_lock(&vx_global_lock);
	if (g_pd) {
		mutex_lock(&g_pd->lock);
		g_pd->debug_dump_enable = enable;
		mutex_unlock(&g_pd->lock);
	}
	mutex_unlock(&vx_global_lock);
}

void trigger_dump_enable(void)
{
	set_global_dump_enable(true);
}
EXPORT_SYMBOL_GPL(trigger_dump_enable);

void trigger_dump_disable(void)
{
	set_global_dump_enable(false);
}
EXPORT_SYMBOL_GPL(trigger_dump_disable);

static int vx_create_debug_nodes(struct vx_platform_data *pd)
{
	struct dentry *entry;

	pd->vx_dir = debugfs_create_dir("sys_pm_vx", NULL);
	if (IS_ERR_OR_NULL(pd->vx_dir))
		return pd->vx_dir ? PTR_ERR(pd->vx_dir) : -ENOMEM;
	entry = debugfs_create_file("sys_pm_violators", 0400, pd->vx_dir, pd, &sys_pm_vx_fops);
	if (IS_ERR_OR_NULL(entry))
		return entry ? PTR_ERR(entry) : -ENOMEM;
	entry = debugfs_create_file("trigger_dump", 0600, pd->vx_dir, pd, &trigger_dump_enable_fops);
	return IS_ERR_OR_NULL(entry) ? (entry ? PTR_ERR(entry) : -ENOMEM) : 0;
}

static const struct of_device_id drv_match_table[] = {
	{ .compatible = "qcom,sys-pm-kalama",
	  .data = drv_names_kalama },
	{ .compatible = "qcom,sys-pm-pineapple",
	  .data = drv_names_pineapple },
	{ .compatible = "qcom,sys-pm-sun",
	  .data = drv_names_sun },
	{ .compatible = "qcom,sys-pm-tuna",
	  .data = drv_names_tuna },
	{ .compatible = "qcom,sys-pm-kera",
	  .data = drv_names_kera },
	{ .compatible = "qcom,sys-pm-sdxkova",
	  .data = drv_names_sdxkova },
	{ }
};

static int vx_probe(struct platform_device *pdev)
{
	const struct of_device_id *match_id;
	struct vx_platform_data *pd;
	struct resource *res;
	const char * const *drvs;
	int i, ret;

	pd = devm_kzalloc(&pdev->dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;
	mutex_init(&pd->lock);
	pd->detect_time_ms = DEFAULT_DEBUG_TIME;
	pd->timer_ms = DEFAULT_TIMER;
	pd->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(pd->base))
		return PTR_ERR(pd->base);
	pd->size = resource_size(res);
	if (pd->size < 8)
		return -EINVAL;
	match_id = of_match_node(drv_match_table, pdev->dev.of_node);
	if (!match_id)
		return -ENODEV;
	drvs = match_id->data;
	for (i = 0; i < MAX_DRV_NAMES && drvs[i] && drvs[i][0]; i++)
		;
	if (!i || i == MAX_DRV_NAMES)
		return -EINVAL;
	pd->n_cxpc_drv = i;
	pd->cxpc_drvs = drvs;
	drvs += MAX_DRV_NAMES;
	for (i = 0; i < MAX_DRV_NAMES && drvs[i] && drvs[i][0]; i++)
		;
	if (!i || i == MAX_DRV_NAMES)
		return -EINVAL;
	pd->n_aoss_drv = i;
	pd->aoss_drvs = drvs;
	pd->qmp = qmp_get(&pdev->dev);
	if (IS_ERR(pd->qmp))
		return PTR_ERR(pd->qmp);
	platform_set_drvdata(pdev, pd);
	/* Serialize singleton publication and removal, including exported calls. */
	mutex_lock(&vx_global_lock);
	if (g_pd) {
		ret = -EBUSY;
		goto fail_qmp;
	}
	ret = vx_create_debug_nodes(pd);
	if (ret)
		goto fail_debugfs;
	ret = device_create_file(&pdev->dev, &dev_attr_debug_time_ms);
	if (ret)
		goto fail_debugfs;
	ret = device_create_file(&pdev->dev, &dev_attr_debug_enable);
	if (ret)
		goto fail_time;
	ret = device_create_file(&pdev->dev, &dev_attr_set_timer_ms);
	if (ret)
		goto fail_enable;
	g_pd = pd;
	mutex_unlock(&vx_global_lock);
	return 0;
fail_enable:
	device_remove_file(&pdev->dev, &dev_attr_debug_enable);
	/* A published setting may have been enabled before a later probe failure. */
	mutex_lock(&pd->lock);
	if (pd->debug_enable)
		subsystem_sleep_debug_enable(false);
	mutex_unlock(&pd->lock);
fail_time:
	device_remove_file(&pdev->dev, &dev_attr_debug_time_ms);
fail_debugfs:
	debugfs_remove_recursive(pd->vx_dir);
fail_qmp:
	mutex_unlock(&vx_global_lock);
	platform_set_drvdata(pdev, NULL);
	qmp_put(pd->qmp);
	return ret;
}

static void vx_remove(struct platform_device *pdev)
{
	struct vx_platform_data *pd = platform_get_drvdata(pdev);

	mutex_lock(&vx_global_lock);
	if (g_pd == pd)
		g_pd = NULL;
	device_remove_file(&pdev->dev, &dev_attr_debug_time_ms);
	device_remove_file(&pdev->dev, &dev_attr_debug_enable);
	device_remove_file(&pdev->dev, &dev_attr_set_timer_ms);
	debugfs_remove_recursive(pd->vx_dir);
	mutex_lock(&pd->lock);
	pd->debug_enable = false;
	subsystem_sleep_debug_enable(false);
	mutex_unlock(&pd->lock);
	qmp_put(pd->qmp);
	platform_set_drvdata(pdev, NULL);
	mutex_unlock(&vx_global_lock);
}

static int vx_suspend(struct device *dev)
{
	struct vx_platform_data *pd = dev_get_drvdata(dev);

	mutex_lock(&pd->lock);
	if (pd->debug_enable) {
		pd->suspend_time = ktime_get_boottime();
		if (pd->monitor_status)
			sys_pm_vx_send_msg(pd);
	}
	mutex_unlock(&pd->lock);
	return 0;
}

static int vx_resume(struct device *dev)
{
	struct vx_platform_data *pd = dev_get_drvdata(dev);
	s64 time_delta_ms;
	bool aosd_enhancement = false;

	mutex_lock(&pd->lock);
	if (!pd->debug_enable)
		goto out;
	pd->resume_time = ktime_get_boottime();
	time_delta_ms = ktime_ms_delta(pd->resume_time, pd->suspend_time);
	if (time_delta_ms <= pd->detect_time_ms)
		goto out;
	if (has_system_slept(&aosd_enhancement))
		goto disable;
	if (aosd_enhancement)
		pd->monitor_status = AOSS_SHUTDOWN_MSG;
	if (!has_subsystem_slept())
		goto disable;
	if (pd->monitor_status)
		vx_check_drv(pd);
	else
		pd->monitor_status = CXPC_MSG;
	goto out;
disable:
	if (pd->monitor_status) {
		pd->monitor_status = DISABLE_MSG;
		sys_pm_vx_send_msg(pd);
	}
out:
	mutex_unlock(&pd->lock);
	return 0;
}

static const struct of_device_id vx_table[] = {
	{ .compatible = "qcom,sys-pm-violators" },
	{ }
};

static const struct dev_pm_ops vx_pm_ops = {
	.suspend = vx_suspend,
	.resume = vx_resume,
};

static struct platform_driver vx_driver = {
	.probe = vx_probe,
	.remove = vx_remove,
	.driver = {
		.name = "sys-pm-violators",
		.of_match_table = vx_table,
		.pm = &vx_pm_ops,
	},
};
module_platform_driver(vx_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. (QTI) System PM Violators driver");
MODULE_ALIAS("platform:sys_pm_vx");
