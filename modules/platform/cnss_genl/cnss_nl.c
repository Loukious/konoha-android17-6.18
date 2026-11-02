// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023,2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <net/genetlink.h>
#ifdef CONFIG_CNSS_OUT_OF_TREE
#include "cnss_nl.h"
#else
#include <net/cnss_nl.h>
#endif
#include <linux/module.h>
#include <linux/of.h>
#include <linux/version.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/srcu.h>

#define CLD80211_GENL_NAME "cld80211"

#define CLD80211_MULTICAST_GROUP_SVC_MSGS       "svc_msgs"
#define CLD80211_MULTICAST_GROUP_HOST_LOGS      "host_logs"
#define CLD80211_MULTICAST_GROUP_FW_LOGS        "fw_logs"
#define CLD80211_MULTICAST_GROUP_PER_PKT_STATS  "per_pkt_stats"
#define CLD80211_MULTICAST_GROUP_DIAG_EVENTS    "diag_events"
#define CLD80211_MULTICAST_GROUP_FATAL_EVENTS   "fatal_events"
#define CLD80211_MULTICAST_GROUP_OEM_MSGS       "oem_msgs"
#define CLD80211_MULTICAST_GROUP_OPT_DP_LOGS    "opt_dp_logs"

static const struct genl_multicast_group nl_mcgrps[] = {
	[CLD80211_MCGRP_SVC_MSGS] = { .name =
			CLD80211_MULTICAST_GROUP_SVC_MSGS},
	[CLD80211_MCGRP_HOST_LOGS] = { .name =
			CLD80211_MULTICAST_GROUP_HOST_LOGS},
	[CLD80211_MCGRP_FW_LOGS] = { .name =
			CLD80211_MULTICAST_GROUP_FW_LOGS},
	[CLD80211_MCGRP_PER_PKT_STATS] = { .name =
			CLD80211_MULTICAST_GROUP_PER_PKT_STATS},
	[CLD80211_MCGRP_DIAG_EVENTS] = { .name =
			CLD80211_MULTICAST_GROUP_DIAG_EVENTS},
	[CLD80211_MCGRP_FATAL_EVENTS] = { .name =
			CLD80211_MULTICAST_GROUP_FATAL_EVENTS},
	[CLD80211_MCGRP_OEM_MSGS] = { .name =
			CLD80211_MULTICAST_GROUP_OEM_MSGS},
	[CLD80211_MCGRP_OPT_DP_LOGS] = { .name =
			CLD80211_MULTICAST_GROUP_OPT_DP_LOGS},
};

struct cld_ops {
	cld80211_cb cb;
	void *cb_ctx;
};

struct cld80211_nl_data {
	struct cld_ops __rcu *cld_ops[CLD80211_MAX_COMMANDS];
};

static struct cld80211_nl_data nl_data;
static DEFINE_MUTEX(cld_callback_lock);
DEFINE_STATIC_SRCU(cld_callback_srcu);
static bool cld_ready;

static struct genl_ops nl_ops[CLD80211_MAX_COMMANDS];

/* policy for the attributes */
static const struct nla_policy cld80211_policy[CLD80211_ATTR_MAX + 1] = {
	[CLD80211_ATTR_VENDOR_DATA] = { .type = NLA_NESTED },
	[CLD80211_ATTR_DATA] = { .type = NLA_BINARY,
				 .len = CLD80211_MAX_NL_DATA },
	[CLD80211_ATTR_META_DATA] = { .type = NLA_BINARY,
				 .len = CLD80211_MAX_NL_DATA },
	[CLD80211_ATTR_CMD] = { .type = NLA_U32 },
	[CLD80211_ATTR_CMD_TAG_DATA] = { .type = NLA_NESTED },
};

static int cld80211_pre_doit(const struct genl_split_ops *ops,
			     struct sk_buff *skb,
			     struct genl_info *info)
{
	if (!ops->cmd || ops->cmd > CLD80211_MAX_COMMANDS)
		return -EOPNOTSUPP;
	return 0;
}

/* The netlink family */
static struct genl_family cld80211_fam __ro_after_init = {
	.name = CLD80211_GENL_NAME,
	.hdrsize = 0,			/* no private header */
	.version = 1,			/* no particular meaning now */
	.maxattr = CLD80211_ATTR_MAX,
	.policy = cld80211_policy,
	.netnsok = true,
	.pre_doit = cld80211_pre_doit,
	.post_doit = NULL,
	.module = THIS_MODULE,
	.ops = nl_ops,
	.n_ops = ARRAY_SIZE(nl_ops),
	.mcgrps = nl_mcgrps,
	.n_mcgrps = ARRAY_SIZE(nl_mcgrps),
};

int register_cld_cmd_cb(u8 cmd_id, cld80211_cb func, void *cb_ctx)
{
	struct cld_ops *entry;
	int ret = 0;

	if (!cmd_id || cmd_id > CLD80211_MAX_COMMANDS || !func)
		return -EINVAL;
	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->cb = func;
	entry->cb_ctx = cb_ctx;
	mutex_lock(&cld_callback_lock);
	/* Observe the initialized netlink family before publishing callbacks. */
	if (!smp_load_acquire(&cld_ready))
		ret = -ENODEV;
	else if (rcu_access_pointer(nl_data.cld_ops[cmd_id - 1]))
		ret = -EEXIST;
	else
		rcu_assign_pointer(nl_data.cld_ops[cmd_id - 1], entry);
	mutex_unlock(&cld_callback_lock);
	if (ret)
		kfree(entry);
	return ret;
}

EXPORT_SYMBOL(register_cld_cmd_cb);

int deregister_cld_cmd_cb(u8 cmd_id)
{
	struct cld_ops *entry;

	if (!cmd_id || cmd_id > CLD80211_MAX_COMMANDS)
		return -EINVAL;
	mutex_lock(&cld_callback_lock);
	entry = rcu_dereference_protected(nl_data.cld_ops[cmd_id - 1],
					  lockdep_is_held(&cld_callback_lock));
	RCU_INIT_POINTER(nl_data.cld_ops[cmd_id - 1], NULL);
	mutex_unlock(&cld_callback_lock);
	/* Callers withdraw outside their own command callback. */
	synchronize_srcu(&cld_callback_srcu);
	kfree(entry);
	return 0;
}

EXPORT_SYMBOL(deregister_cld_cmd_cb);

struct genl_family *cld80211_get_genl_family(void)
{
	return &cld80211_fam;
}
EXPORT_SYMBOL(cld80211_get_genl_family);

static int cld80211_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct cld_ops *entry;
	u8 cmd_id = info->genlhdr->cmd;
	int idx, ret = -EOPNOTSUPP;

	if (!cmd_id || cmd_id > CLD80211_MAX_COMMANDS)
		return -EOPNOTSUPP;
	if (!info->attrs[CLD80211_ATTR_VENDOR_DATA])
		return -EINVAL;
	idx = srcu_read_lock(&cld_callback_srcu);
	entry = srcu_dereference(nl_data.cld_ops[cmd_id - 1],
				 &cld_callback_srcu);
	if (entry) {
		entry->cb(nla_data(info->attrs[CLD80211_ATTR_VENDOR_DATA]),
			  nla_len(info->attrs[CLD80211_ATTR_VENDOR_DATA]),
			  entry->cb_ctx, info->snd_portid);
		ret = 0;
	}
	srcu_read_unlock(&cld_callback_srcu, idx);
	return ret;
}

static int __cld80211_init(void)
{
	int err, i;

	memset(&nl_ops[0], 0, sizeof(nl_ops));

	pr_info("CLD80211: Initializing\n");
	for (i = 0; i < CLD80211_MAX_COMMANDS; i++) {
		nl_ops[i].cmd = i + 1;
		nl_ops[i].doit = cld80211_doit;
		nl_ops[i].flags = GENL_ADMIN_PERM;
	}

	err = genl_register_family(&cld80211_fam);
	if (err) {
		pr_err("CLD80211: Failed to register cld80211 family: %d\n",
		       err);
	}

	if (!err) {
		/* Publish initialized family state to callback registrants. */
		smp_store_release(&cld_ready, true);
	}
	return err;
}

static void __cld80211_exit(void)
{
	int i;

	mutex_lock(&cld_callback_lock);
	/* Withdraw registration access before family and callback teardown. */
	smp_store_release(&cld_ready, false);
	mutex_unlock(&cld_callback_lock);
	genl_unregister_family(&cld80211_fam);
	for (i = 1; i <= CLD80211_MAX_COMMANDS; i++)
		deregister_cld_cmd_cb(i);
}

/**
 * cld80211_is_valid_dt_node_found - Check if valid device tree node present
 *
 * Valid device tree node means a node with "qcom,wlan" property present and
 * "status" property not disabled.
 *
 * Return: true if valid device tree node found, false if not found
 */
static bool cld80211_is_valid_dt_node_found(void)
{
	struct device_node *dn = NULL;

	for_each_node_with_property(dn, "qcom,wlan") {
		if (of_device_is_available(dn))
			break;
	}

	if (dn) {
		of_node_put(dn);
		return true;
	}

	return false;
}

static int __init cld80211_init(void)
{
	if (!cld80211_is_valid_dt_node_found())
		return -ENODEV;

	return __cld80211_init();
}

static void __exit cld80211_exit(void)
{
	__cld80211_exit();
}

module_init(cld80211_init);
module_exit(cld80211_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CNSS generic netlink module");
