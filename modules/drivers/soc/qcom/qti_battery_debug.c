// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"BATTERY_DBG: %s: " fmt, __func__

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/limits.h>
#include <linux/spinlock.h>
#include <linux/platform_device.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qti_pmic_glink.h>

/* owner/type/opcodes for battery debug */
#define MSG_OWNER_BD			32781
#define MSG_TYPE_REQ_RESP		1
#define BD_GET_AGGREGATOR_INFO_REQ	0x15
#define BD_OVERWRITE_VOTABLE_REQ	0x16
#define BD_GET_VOTABLE_REQ		0x17
#define BD_QBG_DUMP_REQ			0x36

/* Generic definitions */
#define MAX_BUF_LEN		SZ_4K
#define BD_WAIT_TIME_MS		1000

#define MAX_NUM_VOTABLES	12
#define MAX_NUM_VOTERS		32
#define MAX_NAME_LEN		12

struct all_votables_data {
	u32			num_votables;
	u32			num_voters;
	char			votables[MAX_NUM_VOTABLES][MAX_NAME_LEN];
	char			voters[MAX_NUM_VOTERS][MAX_NAME_LEN];
};

struct votable_data {
	u32				votable_id;
	u32				eff_val;
	u8				voter_ids[MAX_NUM_VOTERS]; /* unused */
	u32				votes[MAX_NUM_VOTERS];
	u32				active_voter_mask;
	u32				eff_voter;
	u32				override_voter;
};

struct votable {
	char				*name;
	u32				id;
	struct battery_dbg_dev		*bd;
	struct votable_data		data;
	u32				override_val;
};

struct qbg_context_req_msg {
	struct pmic_glink_hdr hdr;
	u32 battery_cell_id;
};

struct qbg_context_resp_msg {
	struct pmic_glink_hdr		hdr;
	u32				length;
	u8				buf[MAX_BUF_LEN];
};

struct votables_list_req_msg {
	struct pmic_glink_hdr hdr;
	/* Shipped Onyx client sends a zero-filled word after the header. */
	u32 reserved;
};

struct votables_list_resp_msg {
	struct pmic_glink_hdr		hdr;
	struct all_votables_data	all_data;
};

struct votable_req_msg {
	struct pmic_glink_hdr		hdr;
	u32				votable_id;
};

struct votable_resp_msg {
	struct pmic_glink_hdr		hdr;
	struct votable_data		v_data;
};

struct override_req_msg {
	struct pmic_glink_hdr		hdr;
	u32				votable_id;
	u32				override_val;
};

struct battery_dbg_dev {
	struct device			*dev;
	struct pmic_glink_client	*client;
	struct mutex			lock;
	struct completion ack;
	spinlock_t response_lock;
	bool pending, stopping, link_up;
	u32 pending_opcode, pending_id;
	int response_error;
	int votables_error;
	struct qbg_context_resp_msg	qbg_dump;
	struct dentry			*debugfs_dir;
	struct debugfs_blob_wrapper	qbg_blob;
	struct all_votables_data	all_data;
	struct votable			*votable;
	u32 override_voter_id;
	u32				battery_cell_id;
};

/* Caller holds lock through response consumption, not only transmission. */
static int battery_dbg_write(struct battery_dbg_dev *bd, void *data, size_t len)
{
	struct pmic_glink_hdr *hdr = data;
	size_t min_len, max_len;
	unsigned long flags;
	bool send_failed;
	int rc;

	switch (hdr->opcode) {
	case BD_GET_AGGREGATOR_INFO_REQ:
		min_len = max_len = sizeof(struct votables_list_resp_msg);
		break;
	case BD_GET_VOTABLE_REQ:
		min_len = max_len = sizeof(struct votable_resp_msg);
		break;
	case BD_OVERWRITE_VOTABLE_REQ:
		min_len = sizeof(*hdr);
		max_len = SIZE_MAX;
		break;
	case BD_QBG_DUMP_REQ:
		min_len = offsetof(struct qbg_context_resp_msg, buf);
		max_len = sizeof(struct qbg_context_resp_msg);
		break;
	default:
		return -EINVAL;
	}
	spin_lock_irqsave(&bd->response_lock, flags);
	rc = bd->stopping ? -ESHUTDOWN : (!bd->link_up ? -ENOTCONN : 0);
	spin_unlock_irqrestore(&bd->response_lock, flags);
	if (rc)
		return rc;
	if (hdr->opcode == BD_GET_VOTABLE_REQ)
		rc = pmic_glink_request_begin_keyed(bd->client, hdr->opcode, MSG_TYPE_REQ_RESP,
			min_len, max_len, offsetof(struct votable_resp_msg, v_data.votable_id),
			((struct votable_req_msg *)data)->votable_id);
	else
		rc = pmic_glink_request_begin(bd->client, hdr->opcode, MSG_TYPE_REQ_RESP,
					      min_len, max_len);
	if (rc)
		return rc;
	spin_lock_irqsave(&bd->response_lock, flags);
	if (bd->stopping || !bd->link_up) {
		rc = bd->stopping ? -ESHUTDOWN : -ENOTCONN;
		spin_unlock_irqrestore(&bd->response_lock, flags);
		pmic_glink_request_end(bd->client, hdr->opcode);
		return rc;
	}
	reinit_completion(&bd->ack);
	bd->pending_opcode = hdr->opcode;
	bd->pending_id = hdr->opcode == BD_GET_VOTABLE_REQ ?
		((struct votable_req_msg *)data)->votable_id : 0;
	bd->response_error = -EINPROGRESS;
	bd->pending = true;
	spin_unlock_irqrestore(&bd->response_lock, flags);
	rc = pmic_glink_write(bd->client, data, len);
	send_failed = rc != 0;
	if (!rc)
		wait_for_completion_timeout(&bd->ack, msecs_to_jiffies(BD_WAIT_TIME_MS));
	spin_lock_irqsave(&bd->response_lock, flags);
	if (rc && bd->pending) {
		bd->pending = false;
		bd->response_error = rc;
	} else if (!rc && bd->pending) {
		bd->pending = false;
		bd->response_error = -ETIMEDOUT;
	}
	if (!rc)
		rc = bd->response_error;
	spin_unlock_irqrestore(&bd->response_lock, flags);
	/* A failed submission has no outstanding reply. Timeouts and link loss
	 * keep the transport-owned record until reply or actual channel drain.
	 */
	if (send_failed)
		pmic_glink_request_end(bd->client, hdr->opcode);
	return rc;
}

static bool battery_dbg_names_valid(const struct all_votables_data *all)
{
	int i, j;

	if (all->num_votables > MAX_NUM_VOTABLES || all->num_voters > MAX_NUM_VOTERS)
		return false;
	for (i = 0; i < all->num_votables; i++) {
		if (!all->votables[i][0] ||
		    !strcmp(all->votables[i], ".") || !strcmp(all->votables[i], "..") ||
		    strchr(all->votables[i], '/'))
			return false;
		for (j = 0; j < i; j++)
			if (!strcmp(all->votables[i], all->votables[j]))
				return false;
	}
	return true;
}

static int battery_dbg_callback(void *priv, void *data, size_t len)
{
	struct battery_dbg_dev *bd = priv;
	struct pmic_glink_hdr hdr;
	struct all_votables_data all;
	struct votable_data vote;
	unsigned long flags;
	bool completed = false;
	int rc = 0, i;

	if (!data || len < sizeof(hdr))
		return -EINVAL;
	memcpy(&hdr, data, sizeof(hdr));
	if (hdr.owner != MSG_OWNER_BD || hdr.type != MSG_TYPE_REQ_RESP)
		return -EINVAL;
	if (!READ_ONCE(bd->client))
		return 0;
	/* Parse into local objects before touching published state. */
	switch (hdr.opcode) {
	case BD_GET_AGGREGATOR_INFO_REQ:
		if (len != sizeof(struct votables_list_resp_msg)) {
			rc = -EPROTO;
			break;
		}
		memcpy(&all, &((struct votables_list_resp_msg *)data)->all_data, sizeof(all));
		for (i = 0; i < MAX_NUM_VOTABLES; i++)
			all.votables[i][MAX_NAME_LEN - 1] = '\0';
		for (i = 0; i < MAX_NUM_VOTERS; i++)
			all.voters[i][MAX_NAME_LEN - 1] = '\0';
		if (!battery_dbg_names_valid(&all))
			rc = -EPROTO;
		break;
	case BD_GET_VOTABLE_REQ:
		if (len != sizeof(struct votable_resp_msg)) {
			rc = -EPROTO;
			break;
		}
		memcpy(&vote, &((struct votable_resp_msg *)data)->v_data, sizeof(vote));
		if (!bd->votable || vote.votable_id >= bd->all_data.num_votables ||
		    (vote.active_voter_mask && vote.eff_voter >= bd->all_data.num_voters) ||
		    (bd->all_data.num_voters < MAX_NUM_VOTERS &&
		     (vote.active_voter_mask >> bd->all_data.num_voters)))
			rc = -EPROTO;
		break;
	case BD_OVERWRITE_VOTABLE_REQ:
		break;
	case BD_QBG_DUMP_REQ:
		if (len < offsetof(struct qbg_context_resp_msg, buf) ||
		    len > sizeof(struct qbg_context_resp_msg)) {
			rc = -EPROTO;
		} else {
			struct qbg_context_resp_msg *response = data;

			if (response->length > sizeof(response->buf) ||
			    response->length > len - offsetof(struct qbg_context_resp_msg, buf))
				rc = -EPROTO;
		}
		break;
	default:
		return -EINVAL;
	}
	spin_lock_irqsave(&bd->response_lock, flags);
	/* The requested ID remains authoritative after timeout/link loss too. */
	if (!rc && hdr.opcode == BD_GET_VOTABLE_REQ && vote.votable_id != bd->pending_id)
		rc = -EPROTO;
	if (!bd->stopping && bd->pending && bd->pending_opcode == hdr.opcode) {
		if (!rc) {
			if (hdr.opcode == BD_GET_AGGREGATOR_INFO_REQ) {
				bd->all_data = all;
				bd->override_voter_id = MAX_NUM_VOTERS;
				for (i = 0; i < all.num_voters; i++)
					if (!strcmp(all.voters[i], "glink")) {
						bd->override_voter_id = i;
						break;
					}
			} else if (hdr.opcode == BD_GET_VOTABLE_REQ) {
				bd->votable[vote.votable_id].data = vote;
			} else if (hdr.opcode == BD_QBG_DUMP_REQ) {
				struct qbg_context_resp_msg *response = data;

				memcpy(bd->qbg_dump.buf, response->buf, response->length);
				bd->qbg_blob.size = response->length;
			}
		}
		/* A malformed matching reply is a protocol error, not silence.
		 * Keep the transport fence, but finish this waiting operation.
		 */
		bd->pending = false;
		bd->response_error = rc;
		completed = true;
	}
	spin_unlock_irqrestore(&bd->response_lock, flags);
	if (rc)
		pmic_glink_request_reject(bd->client, hdr.opcode);
	else
		pmic_glink_request_end(bd->client, hdr.opcode);
	if (completed)
		complete(&bd->ack);
	return rc;
}

static void battery_dbg_state_cb(void *priv, enum pmic_glink_state state)
{
	struct battery_dbg_dev *bd = priv;
	unsigned long flags;
	bool wake = false;

	spin_lock_irqsave(&bd->response_lock, flags);
	bd->link_up = state == PMIC_GLINK_STATE_UP;
	if (!bd->link_up && bd->pending) {
		bd->pending = false;
		bd->response_error = -ENOTCONN;
		wake = true;
	}
	spin_unlock_irqrestore(&bd->response_lock, flags);
	if (wake)
		complete(&bd->ack);
}

static int battery_dbg_file_result(struct dentry *file)
{
	return IS_ERR_OR_NULL(file) ? (file ? PTR_ERR(file) : -ENOMEM) : 0;
}

#ifdef CONFIG_DEBUG_FS
static int battery_dbg_request_read_votable(struct battery_dbg_dev *bd,
					    u32 id)
{
	struct votable_req_msg req_msg = { { 0 } };

	req_msg.hdr.owner = MSG_OWNER_BD;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BD_GET_VOTABLE_REQ;
	req_msg.votable_id = id;

	return battery_dbg_write(bd, &req_msg, sizeof(req_msg));
}

#ifdef CONFIG_QTI_PMIC_GLINK_CLIENT_DEBUG
static int battery_dbg_request_override(struct battery_dbg_dev *bd, u32 id,
					u32 val)
{
	struct override_req_msg req_msg = { { 0 } };

	req_msg.hdr.owner = MSG_OWNER_BD;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BD_OVERWRITE_VOTABLE_REQ;
	req_msg.votable_id = id;
	req_msg.override_val = val;

	pr_debug("requesting override of %s with value %u\n",
			bd->votable[id].name, val);

	return battery_dbg_write(bd, &req_msg, sizeof(req_msg));
}
#endif

static int active_show(struct seq_file *s, void *unused)
{
	int rc;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	mutex_lock(&bd->lock);
	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		mutex_unlock(&bd->lock);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	seq_printf(s, "%#lx\n", voter_mask);

	mutex_unlock(&bd->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(active);

static int winvote_show(struct seq_file *s, void *unused)
{
	int rc, winvote;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	mutex_lock(&bd->lock);
	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		mutex_unlock(&bd->lock);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;
	winvote = voter_mask ? v->data.eff_val : -EINVAL;

	seq_printf(s, "%d\n", winvote);

	mutex_unlock(&bd->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(winvote);

static int winner_show(struct seq_file *s, void *unused)
{
	int rc;
	char *winner;
	u32 eff_voter;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	mutex_lock(&bd->lock);
	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		mutex_unlock(&bd->lock);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	if (voter_mask) {
		eff_voter = v->data.eff_voter;
		winner = bd->all_data.voters[eff_voter];
	} else {
		winner = "";
	}

	seq_printf(s, "%s\n", winner);

	mutex_unlock(&bd->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(winner);

static int voters_show(struct seq_file *s, void *unused)
{
	int rc, i;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	mutex_lock(&bd->lock);
	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		mutex_unlock(&bd->lock);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	for_each_set_bit(i, &voter_mask, MAX_NUM_VOTERS)
		seq_printf(s, "%s ", bd->all_data.voters[i]);
	seq_puts(s, "\n");

	mutex_unlock(&bd->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(voters);

static int votes_show(struct seq_file *s, void *unused)
{
	int rc, i;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	mutex_lock(&bd->lock);
	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		mutex_unlock(&bd->lock);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	for_each_set_bit(i, &voter_mask, MAX_NUM_VOTERS)
		seq_printf(s, "%d ", v->data.votes[i]);
	seq_puts(s, "\n");

	mutex_unlock(&bd->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(votes);

static int status_show(struct seq_file *s, void *unused)
{
	int rc, i, winvote;
	char *winner;
	u32 eff_voter;
	unsigned long voter_mask;
	struct votable *v = s->private;
	struct battery_dbg_dev *bd = v->bd;

	mutex_lock(&bd->lock);
	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		mutex_unlock(&bd->lock);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	if (!voter_mask) {
		seq_puts(s, "\n");
		mutex_unlock(&bd->lock);
		return 0;
	}

	winvote = v->data.eff_val;
	eff_voter = v->data.eff_voter;
	winner = bd->all_data.voters[eff_voter];

	for_each_set_bit(i, &voter_mask, MAX_NUM_VOTERS)
		seq_printf(s, "           %-*s: %-*s: %d\n", MAX_NAME_LEN,
				v->name, MAX_NAME_LEN, bd->all_data.voters[i],
				v->data.votes[i]);
	seq_printf(s, "EFFECTIVE: %-*s: %-*s: %d\n", MAX_NAME_LEN, v->name,
			MAX_NAME_LEN, winner, winvote);

	mutex_unlock(&bd->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

#ifdef CONFIG_QTI_PMIC_GLINK_CLIENT_DEBUG
static int override_get(void *data, u64 *val)
{
	int rc;
	struct votable *v = data;
	struct battery_dbg_dev *bd = v->bd;
	unsigned long voter_mask;

	mutex_lock(&bd->lock);
	rc = battery_dbg_request_read_votable(bd, v->id);
	if (rc) {
		pr_err("Failed to read %s votable: %d\n", v->name, rc);
		mutex_unlock(&bd->lock);
		return rc;
	}

	voter_mask = v->data.active_voter_mask;

	/* Show override voter's vote only if override voter is active */
	if (bd->override_voter_id < bd->all_data.num_voters &&
	    test_bit(bd->override_voter_id, &voter_mask))
		*val = v->data.votes[bd->override_voter_id];
	else
		*val = 0;

	mutex_unlock(&bd->lock);
	return 0;
}

static int override_set(void *data, u64 val)
{
	struct votable *v = data;
	struct battery_dbg_dev *bd = v->bd;
	int rc;

	if (val > U32_MAX)
		return -ERANGE;
	mutex_lock(&bd->lock);
	rc = bd->override_voter_id >= bd->all_data.num_voters ? -EOPNOTSUPP :
		battery_dbg_request_override(bd, v->id, val);
	mutex_unlock(&bd->lock);
	return rc;
}
DEFINE_DEBUGFS_ATTRIBUTE(override_fops, override_get, override_set, "%llu\n");

static int battery_dbg_create_override_file(struct votable *v,
					    struct dentry *votable_dir)
{
	return battery_dbg_file_result(debugfs_create_file("override", 0600,
				votable_dir, v, &override_fops));

}
#else
static int battery_dbg_create_override_file(struct votable *v,
					    struct dentry *votable_dir)
{
	return 0;
}
#endif

static int battery_dbg_create_votable(struct battery_dbg_dev *bd,
				      struct dentry *votables_root_dir,
				      u32 id)
{
	int rc;
	char *v_name = bd->all_data.votables[id];
	struct dentry *votable_dir;

	votable_dir = debugfs_create_dir(v_name, votables_root_dir);
	if (IS_ERR_OR_NULL(votable_dir)) {
		pr_err("Failed to create %s debugfs directory\n", v_name);
		return votable_dir ? PTR_ERR(votable_dir) : -ENOMEM;
	}

	bd->votable[id].name = v_name;

	rc = battery_dbg_file_result(debugfs_create_file("active", 0400, votable_dir,
			&bd->votable[id], &active_fops));
	if (rc)
		goto error;

	rc = battery_dbg_file_result(debugfs_create_file("winvote", 0400, votable_dir,
			&bd->votable[id], &winvote_fops));
	if (rc)
		goto error;

	rc = battery_dbg_file_result(debugfs_create_file("winner", 0400, votable_dir,
			&bd->votable[id], &winner_fops));
	if (rc)
		goto error;

	rc = battery_dbg_file_result(debugfs_create_file("voters", 0400, votable_dir,
			&bd->votable[id], &voters_fops));
	if (rc)
		goto error;

	rc = battery_dbg_file_result(debugfs_create_file("votes", 0400, votable_dir,
			&bd->votable[id], &votes_fops));
	if (rc)
		goto error;

	rc = battery_dbg_file_result(debugfs_create_file("status", 0400, votable_dir,
			&bd->votable[id], &status_fops));
	if (rc)
		goto error;

	rc = battery_dbg_create_override_file(&bd->votable[id], votable_dir);
	if (rc)
		goto error;

	return 0;

error:
	pr_err("Failed to create debugfs file: %d\n", rc);
	return rc;
}

static int battery_dbg_get_votables_list(struct battery_dbg_dev *bd)
{
	struct votables_list_req_msg req_msg = { 0 };
	int rc;

	req_msg.hdr.owner = MSG_OWNER_BD;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BD_GET_AGGREGATOR_INFO_REQ;
	mutex_lock(&bd->lock);
	rc = battery_dbg_write(bd, &req_msg, sizeof(req_msg));
	mutex_unlock(&bd->lock);
	return rc;
}

static int battery_dbg_create_votables(struct battery_dbg_dev *bd,
				       struct dentry *bd_root_dir)
{
	int rc, id;
	u32 num_votables;
	struct dentry *votables_root_dir;

	votables_root_dir = debugfs_create_dir("votables", bd_root_dir);
	if (IS_ERR_OR_NULL(votables_root_dir)) {
		pr_err("Failed to create votables root directory\n");
		return votables_root_dir ? PTR_ERR(votables_root_dir) : -ENOMEM;
	}

	rc = battery_dbg_get_votables_list(bd);
	if (rc) {
		pr_err("Failed to get votables list: %d\n", rc);
		return rc;
	}

	num_votables = bd->all_data.num_votables;

	if (!num_votables)
		return 0;

	bd->votable = devm_kcalloc(bd->dev, num_votables,
				   sizeof(struct votable), GFP_KERNEL);
	if (!bd->votable)
		return -ENOMEM;

	for (id = 0; id < num_votables; id++) {
		bd->votable[id].bd = bd;
		bd->votable[id].id = id;
		rc = battery_dbg_create_votable(bd, votables_root_dir, id);
		if (rc)
			return rc;
	}

	return 0;
}

static int battery_dbg_add_debugfs(struct battery_dbg_dev *bd)
{
	struct dentry *dir;
	int rc;

	dir = debugfs_create_dir("battery_debug", NULL);
	if (IS_ERR_OR_NULL(dir))
		return dir ? PTR_ERR(dir) : -ENOMEM;
	rc = battery_dbg_create_votables(bd, dir);
	if (rc) {
		debugfs_remove_recursive(dir);
		return rc;
	}
	bd->debugfs_dir = dir;
	return 0;
}
#else
static int battery_dbg_add_debugfs(struct battery_dbg_dev *bd)
{
	return 0;
}
#endif

static int get_qbg_context_write(void *data, u64 val)
{
	struct battery_dbg_dev *bd = data;
	struct qbg_context_req_msg req_msg = { { 0 } };
	int rc;

	req_msg.hdr.owner = MSG_OWNER_BD;
	req_msg.hdr.type = MSG_TYPE_REQ_RESP;
	req_msg.hdr.opcode = BD_QBG_DUMP_REQ;
	mutex_lock(&bd->lock);
	req_msg.battery_cell_id = bd->battery_cell_id;
	rc = battery_dbg_write(bd, &req_msg, sizeof(req_msg));
	mutex_unlock(&bd->lock);
	return rc;
}

static ssize_t qbg_blob_write(struct file *filp, struct kobject *kobj,
				    const struct bin_attribute *attr, char *buf,
				    loff_t pos, size_t count)
{
	int rc;
	struct device *dev = kobj_to_dev(kobj);
	struct battery_dbg_dev *bd = dev_get_drvdata(dev);

	rc = get_qbg_context_write(bd, 0); /* second arg is ignored */
	if (rc < 0)
		return rc;

	return count;
}

static ssize_t qbg_blob_read(struct file *filp, struct kobject *kobj,
			    const struct bin_attribute *attr, char *buf,
			    loff_t pos, size_t count)
{
	struct battery_dbg_dev *bd = dev_get_drvdata(kobj_to_dev(kobj));
	ssize_t rc;

	mutex_lock(&bd->lock);
	rc = memory_read_from_buffer(buf, count, &pos, bd->qbg_blob.data, bd->qbg_blob.size);
	mutex_unlock(&bd->lock);
	return rc;
}

static struct bin_attribute qbg_blob = {
	.attr = {
		.name = "qbg_context",
		.mode = 0600,
	},
	.read = qbg_blob_read,
	.write = qbg_blob_write,
};


static ssize_t battery_cell_id_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct battery_dbg_dev *bd = dev_get_drvdata(dev);
	ssize_t rc;

	mutex_lock(&bd->lock);
	rc = sysfs_emit(buf, "%u\n", bd->battery_cell_id);
	mutex_unlock(&bd->lock);
	return rc;
}

static ssize_t battery_cell_id_store(struct device *dev,
				    struct device_attribute *attr, const char *buf,
				    size_t count)
{
	struct battery_dbg_dev *bd = dev_get_drvdata(dev);
	u32 value;

	if (kstrtou32(buf, 0, &value))
		return -EINVAL;
	mutex_lock(&bd->lock);
	bd->battery_cell_id = value;
	mutex_unlock(&bd->lock);
	return count;
}

static DEVICE_ATTR_RW(battery_cell_id);

static ssize_t votables_status_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct battery_dbg_dev *bd = dev_get_drvdata(dev);
	ssize_t rc;

	mutex_lock(&bd->lock);
	if (!bd->votables_error)
		rc = sysfs_emit(buf, "ready\n");
	else if (bd->votables_error == -EINPROGRESS)
		rc = sysfs_emit(buf, "initializing\n");
	else
		rc = sysfs_emit(buf, "unavailable (%d)\n", bd->votables_error);
	mutex_unlock(&bd->lock);
	return rc;
}

static DEVICE_ATTR_RO(votables_status);

static struct attribute *battery_dbg_attrs[] = {
	&dev_attr_battery_cell_id.attr,
	&dev_attr_votables_status.attr,
	NULL,
};

static const struct bin_attribute *battery_dbg_bin_attrs[] = {
	&qbg_blob,
	NULL,
};

static const struct attribute_group battery_dbg_group = {
	.attrs		= battery_dbg_attrs,
	.bin_attrs	= battery_dbg_bin_attrs,
};

static int battery_dbg_add_dev_attr(struct battery_dbg_dev *bd)
{
	int rc;

	bd->qbg_blob.data = bd->qbg_dump.buf;
	bd->qbg_blob.size = 0;

	rc = sysfs_create_group(&bd->dev->kobj, &battery_dbg_group);
	if (rc)
		dev_err(bd->dev, "Failed to create sysfs files for qbg_context: %d\n",
			rc);

	return rc;
}

static int battery_dbg_probe(struct platform_device *pdev)
{
	struct battery_dbg_dev *bd;
	struct pmic_glink_client_data client_data = { };
	int rc;

	bd = devm_kzalloc(&pdev->dev, sizeof(*bd), GFP_KERNEL);
	if (!bd)
		return -ENOMEM;
	bd->dev = &pdev->dev;
	bd->link_up = true;
	bd->override_voter_id = MAX_NUM_VOTERS;
	bd->votables_error = -EINPROGRESS;
	mutex_init(&bd->lock);
	spin_lock_init(&bd->response_lock);
	init_completion(&bd->ack);
	platform_set_drvdata(pdev, bd);
	client_data.id = MSG_OWNER_BD;
	client_data.name = "battery_debug";
	client_data.msg_cb = battery_dbg_callback;
	client_data.state_cb = battery_dbg_state_cb;
	client_data.priv = bd;
	bd->client = pmic_glink_register_client(bd->dev, &client_data);
	if (IS_ERR(bd->client))
		return PTR_ERR(bd->client);
	rc = battery_dbg_add_dev_attr(bd);
	if (rc)
		goto unregister;
	rc = battery_dbg_add_debugfs(bd);
	mutex_lock(&bd->lock);
	bd->votables_error = IS_ENABLED(CONFIG_DEBUG_FS) ? rc : -EOPNOTSUPP;
	mutex_unlock(&bd->lock);
	if (!rc)
		return 0;
	/* QBG uses an independent native command and remains useful on firmware
	 * that does not answer aggregator queries. Do not retry an unanswered
	 * query: its transport-owned fence remains authoritative.
	 */
	if (rc == -ETIMEDOUT || rc == -EAGAIN || rc == -EPROTO || rc == -ENOTCONN) {
		dev_warn(bd->dev, "Votables unavailable (%d); QBG interface retained\n", rc);
		return 0;
	}
	sysfs_remove_group(&bd->dev->kobj, &battery_dbg_group);
unregister:
	pmic_glink_unregister_client(bd->client);
	return rc;
}

static void battery_dbg_remove(struct platform_device *pdev)
{
	struct battery_dbg_dev *bd = platform_get_drvdata(pdev);
	unsigned long flags;
	bool wake;

	spin_lock_irqsave(&bd->response_lock, flags);
	bd->stopping = true;
	wake = bd->pending;
	bd->pending = false;
	bd->response_error = -ESHUTDOWN;
	spin_unlock_irqrestore(&bd->response_lock, flags);
	if (wake)
		complete(&bd->ack);
	sysfs_remove_group(&bd->dev->kobj, &battery_dbg_group);
	debugfs_remove_recursive(bd->debugfs_dir);
	mutex_lock(&bd->lock);
	pmic_glink_unregister_client(bd->client);
	mutex_unlock(&bd->lock);
}

static const struct of_device_id battery_dbg_match_table[] = {
	{ .compatible = "qcom,battery-debug" },
	{},
};

static struct platform_driver battery_dbg_driver = {
	.driver	= {
		.name = "qti_battery_debug",
		.of_match_table = battery_dbg_match_table,
	},
	.probe	= battery_dbg_probe,
	.remove	= battery_dbg_remove,
};
module_platform_driver(battery_dbg_driver);

MODULE_DESCRIPTION("QTI Glink battery debug driver");
MODULE_LICENSE("GPL");
