// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt)	"CHARGER_ULOG: %s: " fmt, __func__

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/ipc_logging.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qti_pmic_glink.h>
#include <mca/common/mca_log.h>

#define MSG_OWNER_CHG_ULOG		32778
#define MSG_TYPE_REQ_RESP		1
#define GET_CHG_ULOG_REQ		0x18
#define SET_CHG_ULOG_PROP_REQ		0x19
#define GET_CHG_INIT_ULOG_REQ		0x23
#define GET_MCA_LOG_REQ 0x52
#define GET_MCA_REALTIME_LOG_REQ 0x53

#define LOG_CATEGORY_INIT		(1ULL << 32)
#define LOG_MIN_TIME_MS			500
#define LOG_DEFAULT_TIME_MS		1000

#define MAX_ULOG_SIZE			8192
#define NUM_LOG_PAGES			10
#define NUM_INIT_LOG_PAGES		8
#define MAX_ADSP_LOG_SIZE 7680
#define SINGLE_LOG_SIZE 256
#define MAX_LOG_PR_SIZE 512

#ifndef MCA_LOG_TAG
#define MCA_LOG_TAG "charger_ulog_glink"
#endif

struct set_ulog_prop_req_msg {
	struct pmic_glink_hdr		hdr;
	u64				log_category;
	u32				log_level;
};

struct get_ulog_req_msg {
	struct pmic_glink_hdr		hdr;
	u32				log_size;
};

struct get_ulog_resp_msg {
	struct pmic_glink_hdr		hdr;
	u8				buf[MAX_ULOG_SIZE];
};

struct get_adsp_log_msg {
	struct pmic_glink_hdr hdr;
	char log_info[MAX_ADSP_LOG_SIZE];
	int cur_pos;
	u64 cur_time;
};

struct adsp_realtime_log_msg {
	struct pmic_glink_hdr hdr;
	char log_info[SINGLE_LOG_SIZE];
};

struct chg_ulog_glink_dev {
	struct device			*dev;
	struct pmic_glink_client	*client;
	struct dentry			*debugfs_dir;
	void				*ipc_log;
	void				*ipc_init_log;
	struct mutex lock; /* Serializes transport writes and client removal. */
	struct mutex rx_lock; /* Responses, log parsing and polling state. */
	struct mutex control_lock; /* Serializes debug settings and cancel/rearm. */
	bool stopping;
	bool link_up;
	bool pending;
	bool props_valid;
	bool props_dirty;
	u64 link_generation;
	u32 pending_opcode;
	int result;
	struct completion		ack;
	struct delayed_work		ulog_work;
	struct work_struct adsp_dump_log_work;
	u8				ulog_buf[MAX_ULOG_SIZE];
	u64				log_category;
	u32				log_level;
	u32				log_time_ms;
	bool				log_enable;
	bool				init_log_enable;
};

#define WAIT_TIME_MS			1000
static u32 chg_ulog_opcode_mask(u32 opcode)
{
	switch (opcode) {
	case SET_CHG_ULOG_PROP_REQ: return BIT(0);
	case GET_CHG_ULOG_REQ: return BIT(1);
	case GET_CHG_INIT_ULOG_REQ: return BIT(2);
	default: return 0;
	}
}

static int chg_ulog_write(struct chg_ulog_glink_dev *cd, void *data, size_t len)
{
	struct pmic_glink_hdr hdr;
	u32 mask;
	int rc;

	if (!data || len < sizeof(hdr))
		return -EINVAL;
	memcpy(&hdr, data, sizeof(hdr));
	mask = chg_ulog_opcode_mask(hdr.opcode);
	if (!mask)
		return -EINVAL;
	mutex_lock(&cd->lock);
	mutex_lock(&cd->rx_lock);
	if (cd->stopping)
		rc = -ESHUTDOWN;
	else if (!cd->link_up)
		rc = -ENOTCONN;
	else
		rc = 0;
	if (rc) {
		mutex_unlock(&cd->rx_lock);
		goto out;
	}
	rc = pmic_glink_request_begin(cd->client, hdr.opcode, MSG_TYPE_REQ_RESP,
		hdr.opcode == SET_CHG_ULOG_PROP_REQ ? sizeof(hdr) : sizeof(struct get_ulog_resp_msg),
		hdr.opcode == SET_CHG_ULOG_PROP_REQ ? SIZE_MAX : sizeof(struct get_ulog_resp_msg));
	if (rc) {
		mutex_unlock(&cd->rx_lock);
		goto out;
	}
	reinit_completion(&cd->ack);
	cd->pending = true;
	cd->pending_opcode = hdr.opcode;
	cd->result = -EINPROGRESS;
	mutex_unlock(&cd->rx_lock);
	rc = pmic_glink_write(cd->client, data, len);
	if (!rc)
		wait_for_completion_timeout(&cd->ack, msecs_to_jiffies(WAIT_TIME_MS));
	mutex_lock(&cd->rx_lock);
	if (rc && cd->pending) {
		cd->pending = false;
		cd->result = rc;
		pmic_glink_request_end(cd->client, hdr.opcode);
	} else if (!rc && cd->pending) {
		/* Native firmware has no transaction ID. Do not reuse this opcode
		 * until its late response has been consumed by the RX callback.
		 */
		cd->pending = false;
		/* Transport retains the unanswered owner/opcode across unregister. */
		cd->result = -ETIMEDOUT;
	}
	rc = cd->result;
	mutex_unlock(&cd->rx_lock);
out:
	mutex_unlock(&cd->lock);
	return rc;
}

static int chg_ulog_request(struct chg_ulog_glink_dev *cd, bool init)
{
	struct get_ulog_req_msg req_msg = { { 0 } };

	req_msg.hdr.owner = MSG_OWNER_CHG_ULOG;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = init ? GET_CHG_INIT_ULOG_REQ : GET_CHG_ULOG_REQ;
	req_msg.log_size = MAX_ULOG_SIZE;

	return chg_ulog_write(cd, &req_msg, sizeof(req_msg));
}

static int chg_ulog_set_log_type(struct chg_ulog_glink_dev *cd, u64 category,
				u32 level)
{
	struct set_ulog_prop_req_msg req_msg = { { 0 } };
	int rc;

	req_msg.hdr.owner = MSG_OWNER_CHG_ULOG;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = SET_CHG_ULOG_PROP_REQ;
	req_msg.log_category = category;
	req_msg.log_level = level;

	rc = chg_ulog_write(cd, &req_msg, sizeof(req_msg));
	if (!rc)
		pr_debug("Set log category %llu log level %u\n", category,
			level);

	return rc;
}

static void chg_ulog_work(struct work_struct *work)
{
	struct chg_ulog_glink_dev *cd = container_of(work,
			struct chg_ulog_glink_dev, ulog_work.work);
	bool init, restore;
	u64 category, generation;
	u32 level;
	int rc;

	mutex_lock(&cd->rx_lock);
	if (cd->stopping || !cd->link_up || !(cd->log_enable || cd->init_log_enable)) {
		mutex_unlock(&cd->rx_lock);
		return;
	}
	init = cd->init_log_enable;
	restore = cd->props_valid && cd->props_dirty;
	category = cd->log_category;
	generation = cd->link_generation;
	level = cd->log_level;
	mutex_unlock(&cd->rx_lock);
	rc = restore ? chg_ulog_set_log_type(cd, category, level) : 0;
	if (!rc && restore) {
		mutex_lock(&cd->rx_lock);
		cd->props_dirty = !cd->link_up || generation != cd->link_generation;
		mutex_unlock(&cd->rx_lock);
	}
	if (!rc)
		rc = chg_ulog_request(cd, init);
	if (rc)
		pr_err("Error requesting ulog, rc=%d\n", rc);
	mutex_lock(&cd->rx_lock);
	if ((!rc || rc == -EAGAIN) && !cd->stopping && cd->link_up &&
	    (cd->log_enable || cd->init_log_enable))
		schedule_delayed_work(&cd->ulog_work, msecs_to_jiffies(cd->log_time_ms));
	mutex_unlock(&cd->rx_lock);
}

static void adsp_dump_log_work_func(struct work_struct *work)
{
	struct chg_ulog_glink_dev *cd = container_of(work,
			struct chg_ulog_glink_dev, adsp_dump_log_work);
	struct get_ulog_req_msg req_msg = { { 0 } };
	int rc;

	req_msg.hdr.owner = MSG_OWNER_CHG_ULOG;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = GET_MCA_LOG_REQ;
	mutex_lock(&cd->lock);
	mutex_lock(&cd->rx_lock);
	rc = cd->stopping ? -ESHUTDOWN : (!cd->link_up ? -ENOTCONN : 0);
	mutex_unlock(&cd->rx_lock);
	if (!rc)
		rc = pmic_glink_write(cd->client, &req_msg, sizeof(req_msg));
	mutex_unlock(&cd->lock);
	if (rc)
		pr_err("Error requesting ADSP log, rc=%d\n", rc);
}

static void ulog_store(struct chg_ulog_glink_dev *cd, void *ipc_ctxt, size_t len)
{
	char *buf = (char *)cd->ulog_buf, *token;

	if (!len || len > sizeof(cd->ulog_buf) || !ipc_ctxt)
		return;
	if (!buf[0]) {
		cd->init_log_enable = false;
		return;
	}
	buf[len - 1] = '\0';
	if (len >= MAX_MSG_SIZE) {
		while ((token = strsep(&buf, "\n")))
			ipc_log_string(ipc_ctxt, "%s", token);
	} else {
		ipc_log_string(ipc_ctxt, "%s", buf);
	}
}

static int handle_ulog_message(struct chg_ulog_glink_dev *cd,
			      const void *data, size_t len, u32 opcode)
{
	void *ipc_ctxt;

	if (len != sizeof(struct get_ulog_resp_msg))
		return -EMSGSIZE;
	memcpy(cd->ulog_buf, (const u8 *)data + sizeof(struct pmic_glink_hdr),
	       sizeof(cd->ulog_buf));
	ipc_ctxt = opcode == GET_CHG_INIT_ULOG_REQ ? cd->ipc_init_log : cd->ipc_log;
	ulog_store(cd, ipc_ctxt, sizeof(cd->ulog_buf));
	return 0;
}

static int handle_adsp_log(struct chg_ulog_glink_dev *cd, const void *data, size_t len)
{
	const char *text = (const char *)data + sizeof(struct pmic_glink_hdr);
	char buf[MAX_LOG_PR_SIZE + 1];
	u64 timestamp;
	int remaining, pos = 0, count, limit;

	if (len != sizeof(struct get_adsp_log_msg))
		return -EMSGSIZE;
	memcpy(&remaining, (const u8 *)data + offsetof(struct get_adsp_log_msg, cur_pos),
	       sizeof(remaining));
	if (remaining < 0 || remaining > MAX_ADSP_LOG_SIZE)
		return -EINVAL;
	memcpy(&timestamp, (const u8 *)data + offsetof(struct get_adsp_log_msg, cur_time),
	       sizeof(timestamp));
	pr_info("adsp_cur_time=%llu\n", timestamp);
	while (remaining) {
		limit = min(remaining, MAX_LOG_PR_SIZE - 1);
		count = min(remaining, SINGLE_LOG_SIZE);
		while (count < limit && text[pos + count] != '\n' && text[pos + count] != '\0')
			count++;
		if (count < limit)
			count++;
		memcpy(buf, text + pos, count);
		buf[count] = '\0';
		mca_log_info("%s", buf);
		remaining -= count;
		pos += count;
	}
	return 0;
}

static int chg_ulog_callback(void *priv, void *data, size_t len)
{
	struct chg_ulog_glink_dev *cd = priv;
	struct pmic_glink_hdr hdr;
	int rc = 0;
	u32 mask;

	if (!cd || !data || len < sizeof(hdr))
		return -EMSGSIZE;
	memcpy(&hdr, data, sizeof(hdr));
	if (hdr.owner != MSG_OWNER_CHG_ULOG ||
	    (chg_ulog_opcode_mask(hdr.opcode) && hdr.type != MSG_TYPE_REQ_RESP))
		return -EINVAL;
	mutex_lock(&cd->rx_lock);
	if (cd->stopping) {
		rc = -ESHUTDOWN;
		goto out;
	}
	switch (hdr.opcode) {
	case SET_CHG_ULOG_PROP_REQ:
		break;
	case GET_CHG_ULOG_REQ:
	case GET_CHG_INIT_ULOG_REQ:
		rc = handle_ulog_message(cd, data, len, hdr.opcode);
		break;
	case GET_MCA_LOG_REQ:
		rc = handle_adsp_log(cd, data, len);
		break;
	case GET_MCA_REALTIME_LOG_REQ:
		if (len != sizeof(struct adsp_realtime_log_msg))
			rc = -EMSGSIZE;
		else
			mca_log_info("%.*s", SINGLE_LOG_SIZE,
				    (const char *)data + sizeof(hdr));
		break;
	default:
		rc = -EINVAL;
		break;
	}
	mask = chg_ulog_opcode_mask(hdr.opcode);
	if (!rc && mask) {
		if (cd->client)
			pmic_glink_request_end(cd->client, hdr.opcode);
		if (!cd->pending || cd->pending_opcode != hdr.opcode) {
			/* A late reply finishes the old request; it cannot finish a
			 * later request while this opcode is quarantined.
			 */
			if (cd->link_up && (cd->log_enable || cd->init_log_enable))
				schedule_delayed_work(&cd->ulog_work,
						msecs_to_jiffies(cd->log_time_ms));
		} else if (cd->pending && cd->pending_opcode == hdr.opcode) {
			cd->result = 0;
			cd->pending = false;
			complete(&cd->ack);
		}
	}
out:
	mutex_unlock(&cd->rx_lock);
	return rc;
}

static void chg_ulog_state_cb(void *priv, enum pmic_glink_state state)
{
	struct chg_ulog_glink_dev *cd = priv;

	mutex_lock(&cd->rx_lock);
	if (cd->stopping)
		goto out;
	cd->link_generation++;
	cd->link_up = state == PMIC_GLINK_STATE_UP;
	if (!cd->link_up) {
		cd->props_dirty = cd->props_valid;
		if (cd->pending) {
			/* Keep the transport fence until the reply or full channel drain. */
			cd->pending = false;
			cd->result = -ENOTCONN;
			complete(&cd->ack);
		}
	} else if (cd->log_enable || cd->init_log_enable) {
		schedule_delayed_work(&cd->ulog_work, msecs_to_jiffies(cd->log_time_ms));
	}
out:
	mutex_unlock(&cd->rx_lock);
}

static int ulog_cat_get(void *data, u64 *val)
{
	struct chg_ulog_glink_dev *cd = data;
	int rc;
	mutex_lock(&cd->rx_lock);
	rc = cd->stopping ? -ESHUTDOWN : 0;
	if (!rc)
		*val = cd->log_category;
	mutex_unlock(&cd->rx_lock);
	return rc;
}

static int ulog_cat_set(void *data, u64 val)
{
	struct chg_ulog_glink_dev *cd = data;
	u64 category, generation;
	u32 level;
	int rc;
	
	mutex_lock(&cd->control_lock);
	mutex_lock(&cd->rx_lock);
	if (cd->stopping)
		rc = -ESHUTDOWN;
	else if (cd->log_enable || cd->init_log_enable)
		rc = -EINVAL;
	else if (val == cd->log_category)
		rc = 1;
	else
		rc = 0;
	category = cd->log_category;
	generation = cd->link_generation;
	level = cd->log_level;
	mutex_unlock(&cd->rx_lock);
	if (!rc) {
		rc = chg_ulog_set_log_type(cd, val, level);
		if (!rc) {
			mutex_lock(&cd->rx_lock);
			cd->log_category = val;
			cd->props_valid = true;
			cd->props_dirty = !cd->link_up || generation != cd->link_generation;
			mutex_unlock(&cd->rx_lock);
		}
	} else if (rc == 1) {
		rc = 0;
	}
	mutex_unlock(&cd->control_lock);
	return rc;
}
DEFINE_DEBUGFS_ATTRIBUTE(ulog_cat_fops, ulog_cat_get, ulog_cat_set,
			"%llu\n");

static int ulog_level_get(void *data, u64 *val)
{
	struct chg_ulog_glink_dev *cd = data;
	int rc;
	mutex_lock(&cd->rx_lock);
	rc = cd->stopping ? -ESHUTDOWN : 0;
	if (!rc)
		*val = cd->log_level;
	mutex_unlock(&cd->rx_lock);
	return rc;
}

static int ulog_level_set(void *data, u64 val)
{
	struct chg_ulog_glink_dev *cd = data;
	u64 category, generation;
	u32 level;
	int rc;
	if (val > U32_MAX)
		return -ERANGE;
	mutex_lock(&cd->control_lock);
	mutex_lock(&cd->rx_lock);
	if (cd->stopping)
		rc = -ESHUTDOWN;
	else if (cd->log_enable || cd->init_log_enable)
		rc = -EINVAL;
	else if (val == cd->log_level)
		rc = 1;
	else
		rc = 0;
	category = cd->log_category;
	generation = cd->link_generation;
	level = cd->log_level;
	mutex_unlock(&cd->rx_lock);
	if (!rc) {
		rc = chg_ulog_set_log_type(cd, category, val);
		if (!rc) {
			mutex_lock(&cd->rx_lock);
			cd->log_level = val;
			cd->props_valid = true;
			cd->props_dirty = !cd->link_up || generation != cd->link_generation;
			mutex_unlock(&cd->rx_lock);
		}
	} else if (rc == 1) {
		rc = 0;
	}
	mutex_unlock(&cd->control_lock);
	return rc;
}
DEFINE_DEBUGFS_ATTRIBUTE(ulog_level_fops, ulog_level_get, ulog_level_set,
			"%llu\n");

static int ulog_en_get(void *data, u64 *val)
{
	struct chg_ulog_glink_dev *cd = data;
	int rc;
	mutex_lock(&cd->rx_lock);
	rc = cd->stopping ? -ESHUTDOWN : 0;
	if (!rc)
		*val = cd->log_category == LOG_CATEGORY_INIT ? cd->init_log_enable : cd->log_enable;
	mutex_unlock(&cd->rx_lock);
	return rc;
}

static int ulog_en_set(void *data, u64 val)
{
	struct chg_ulog_glink_dev *cd = data;
	bool *enable;
	int rc = 0;

	if (val > 1)
		return -EINVAL;
	mutex_lock(&cd->control_lock);
	mutex_lock(&cd->rx_lock);
	if (cd->stopping) {
		rc = -ESHUTDOWN;
		mutex_unlock(&cd->rx_lock);
		goto out;
	}
	enable = cd->log_category == LOG_CATEGORY_INIT ? &cd->init_log_enable : &cd->log_enable;
	*enable = val;
	if (val && cd->link_up)
		schedule_delayed_work(&cd->ulog_work, msecs_to_jiffies(cd->log_time_ms));
	mutex_unlock(&cd->rx_lock);
	if (!val)
		cancel_delayed_work_sync(&cd->ulog_work);
out:
	mutex_unlock(&cd->control_lock);
	return rc;
}
DEFINE_DEBUGFS_ATTRIBUTE(ulog_en_fops, ulog_en_get, ulog_en_set, "%llu\n");

static int ulog_time_get(void *data, u64 *val)
{
	struct chg_ulog_glink_dev *cd = data;
	int rc;
	mutex_lock(&cd->rx_lock);
	rc = cd->stopping ? -ESHUTDOWN : 0;
	if (!rc)
		*val = cd->log_time_ms;
	mutex_unlock(&cd->rx_lock);
	return rc;
}

static int ulog_time_set(void *data, u64 val)
{
	struct chg_ulog_glink_dev *cd = data;
	int rc;

	if (val < LOG_MIN_TIME_MS || val > U32_MAX)
		return -EINVAL;
	mutex_lock(&cd->control_lock);
	mutex_lock(&cd->rx_lock);
	rc = cd->stopping ? -ESHUTDOWN : 0;
	if (!rc)
		cd->log_time_ms = val;
	mutex_unlock(&cd->rx_lock);
	mutex_unlock(&cd->control_lock);
	return rc;
}
DEFINE_DEBUGFS_ATTRIBUTE(ulog_time_fops, ulog_time_get, ulog_time_set,
			"%llu\n");

static int dump_adsp_log_get(void *data, u64 *val)
{
	struct chg_ulog_glink_dev *cd = data;
	int rc;
	mutex_lock(&cd->rx_lock);
	rc = cd->stopping ? -ESHUTDOWN : 0;
	if (!rc) {
		*val = 0;
		schedule_work(&cd->adsp_dump_log_work);
	}
	mutex_unlock(&cd->rx_lock);
	return rc;
}

static int dump_adsp_log_set(void *data, u64 val)
{
	u64 unused;
	return dump_adsp_log_get(data, &unused);
}
DEFINE_DEBUGFS_ATTRIBUTE(dump_adsp_log_fops, dump_adsp_log_get,
			 dump_adsp_log_set, "%llu\n");

static int chg_ulog_add_debugfs(struct chg_ulog_glink_dev *cd)
{
	struct dentry *dir, *file;
	int rc;

	dir = debugfs_create_dir("charger_ulog", NULL);
	if (IS_ERR_OR_NULL(dir)) {
		rc = dir ? PTR_ERR(dir) : -ENOMEM;
		pr_err("Failed to create charger_ulog debugfs directory: %d\n",
			rc);
		return rc;
	}

	file = debugfs_create_file_unsafe("category", 0600, dir, cd,
					&ulog_cat_fops);
	if (IS_ERR_OR_NULL(file)) {
		rc = file ? PTR_ERR(file) : -ENOMEM;
		pr_err("Failed to create category %d\n", rc);
		goto out;
	}

	file = debugfs_create_file_unsafe("level", 0600, dir, cd,
					&ulog_level_fops);
	if (IS_ERR_OR_NULL(file)) {
		rc = file ? PTR_ERR(file) : -ENOMEM;
		pr_err("Failed to create level %d\n", rc);
		goto out;
	}

	file = debugfs_create_file_unsafe("enable", 0600, dir, cd,
					&ulog_en_fops);
	if (IS_ERR_OR_NULL(file)) {
		rc = file ? PTR_ERR(file) : -ENOMEM;
		pr_err("Failed to create enable %d\n", rc);
		goto out;
	}

	file = debugfs_create_file_unsafe("time_ms", 0600, dir, cd,
					&ulog_time_fops);
	if (IS_ERR_OR_NULL(file)) {
		rc = file ? PTR_ERR(file) : -ENOMEM;
		pr_err("Failed to create time_ms %d\n", rc);
		goto out;
	}

	file = debugfs_create_file_unsafe("dump_adsp_log", 0600, dir, cd,
					  &dump_adsp_log_fops);
	if (IS_ERR_OR_NULL(file)) {
		rc = file ? PTR_ERR(file) : -ENOMEM;
		pr_err("Failed to create time_ms %d\n", rc);
		goto out;
	}
	cd->debugfs_dir = dir;

	return 0;
out:
	debugfs_remove_recursive(dir);
	return rc;
}

static void chg_ulog_cleanup(struct chg_ulog_glink_dev *cd)
{
	mutex_lock(&cd->rx_lock);
	cd->stopping = true;
	cd->log_enable = false;
	cd->init_log_enable = false;
	if (cd->pending) {
		cd->pending = false;
		cd->result = -ESHUTDOWN;
		complete(&cd->ack);
	}
	mutex_unlock(&cd->rx_lock);
	/* Attribute wrappers drain active operations before their data disappears. */
	debugfs_remove_recursive(cd->debugfs_dir);
	cancel_delayed_work_sync(&cd->ulog_work);
	cancel_work_sync(&cd->adsp_dump_log_work);
	mutex_lock(&cd->lock);
	if (cd->client) {
		pmic_glink_unregister_client(cd->client);
		cd->client = NULL;
	}
	mutex_unlock(&cd->lock);
	if (cd->ipc_log)
		ipc_log_context_destroy(cd->ipc_log);
	if (cd->ipc_init_log)
		ipc_log_context_destroy(cd->ipc_init_log);
}

static int chg_ulog_probe(struct platform_device *pdev)
{
	struct chg_ulog_glink_dev *cd;
	struct pmic_glink_client_data client_data = { };
	struct pmic_glink_client *client;
	int rc;

	cd = devm_kzalloc(&pdev->dev, sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;
	mutex_init(&cd->lock);
	mutex_init(&cd->rx_lock);
	mutex_init(&cd->control_lock);
	init_completion(&cd->ack);
	INIT_DELAYED_WORK(&cd->ulog_work, chg_ulog_work);
	INIT_WORK(&cd->adsp_dump_log_work, adsp_dump_log_work_func);
	cd->log_time_ms = LOG_DEFAULT_TIME_MS;
	cd->dev = &pdev->dev;
	cd->link_up = true; /* Successful client registration requires an up parent. */
	cd->ipc_log = ipc_log_context_create(NUM_LOG_PAGES, "charger_ulog", 0);
	if (!cd->ipc_log) {
		rc = -ENOMEM;
		goto cleanup;
	}
	cd->ipc_init_log = ipc_log_context_create(NUM_INIT_LOG_PAGES, "charger_ulog_init", 0);
	if (!cd->ipc_init_log) {
		rc = -ENOMEM;
		goto cleanup;
	}
	client_data.id = MSG_OWNER_CHG_ULOG;
	client_data.name = "chg_ulog";
	client_data.msg_cb = chg_ulog_callback;
	client_data.state_cb = chg_ulog_state_cb;
	client_data.priv = cd;
	client = pmic_glink_register_client(cd->dev, &client_data);
	if (IS_ERR(client)) {
		rc = PTR_ERR(client);
		goto cleanup;
	}
	mutex_lock(&cd->rx_lock);
	cd->client = client;
	mutex_unlock(&cd->rx_lock);
	rc = chg_ulog_add_debugfs(cd);
	if (rc)
		goto cleanup;
	platform_set_drvdata(pdev, cd);
	return 0;
cleanup:
	chg_ulog_cleanup(cd);
	return dev_err_probe(cd->dev, rc, "Error preparing charger ulog\n");
}

static void chg_ulog_remove(struct platform_device *pdev)
{
	struct chg_ulog_glink_dev *cd = platform_get_drvdata(pdev);
	chg_ulog_cleanup(cd);
	platform_set_drvdata(pdev, NULL);
}

static const struct of_device_id chg_ulog_match_table[] = {
	{ .compatible = "qcom,charger-ulog-glink" },
	{},
};

MODULE_DEVICE_TABLE(of, chg_ulog_match_table);

static struct platform_driver chg_ulog_driver = {
	.driver	= {
		.name = "charger_ulog_glink",
		.of_match_table = chg_ulog_match_table,
	},
	.probe	= chg_ulog_probe,
	.remove	= chg_ulog_remove,
};
module_platform_driver(chg_ulog_driver);

MODULE_DESCRIPTION("QTI charger ulog glink driver");
MODULE_LICENSE("GPL");

MODULE_SOFTDEP("pre: qti_pmic_glink");
