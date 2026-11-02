// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017, 2019, 2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "cnss_utils: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/etherdevice.h>
#include <linux/debugfs.h>
#include <linux/of.h>
#include <linux/limits.h>
#include <linux/srcu.h>
#include <linux/atomic.h>
#ifdef CONFIG_CNSS_OUT_OF_TREE
#include "cnss_utils.h"
#else
#include <net/cnss_utils.h>
#endif

#ifdef CONFIG_FEATURE_SMEM_MAILBOX
#include <smem-mailbox.h>
#endif

#define CNSS_MAX_CH_NUM 157
struct cnss_unsafe_channel_list {
	u16 unsafe_ch_count;
	u16 unsafe_ch_list[CNSS_MAX_CH_NUM];
};

struct cnss_dfs_nol_info {
	void *dfs_nol_info;
	u16 dfs_nol_info_len;
};

#define MAX_NO_OF_MAC_ADDR 4
#define MAC_PREFIX_LEN 2
struct cnss_wlan_mac_addr {
	u8 mac_addr[MAX_NO_OF_MAC_ADDR][ETH_ALEN];
	u32 no_of_mac_addr_set;
};

enum mac_type {
	CNSS_MAC_PROVISIONED,
	CNSS_MAC_DERIVED,
};

#define CNSS_UTILS_NOTIFIER_MAX_USER	2
struct cnss_utils_status_registration {
	cnss_utils_status_update callback;
	void *context;
};

struct cnss_utils_status_notifier {
	struct cnss_utils_status_registration __rcu
		*users[CNSS_UTILS_NOTIFIER_MAX_USER];
};

static DEFINE_MUTEX(utils_notifier_lock);
DEFINE_STATIC_SRCU(utils_notifier_srcu);


static struct cnss_utils_priv {
	struct cnss_unsafe_channel_list unsafe_channel_list;
	struct cnss_dfs_nol_info dfs_nol_info;
	/* generic mutex for unsafe channel */
	struct mutex unsafe_channel_list_lock;
	/* generic spin-lock for dfs_nol info */
	spinlock_t dfs_nol_info_lock;
	atomic_t driver_load_cnt;
	struct cnss_wlan_mac_addr wlan_mac_addr;
	struct cnss_wlan_mac_addr wlan_der_mac_addr;
	/* Serializes the first immutable MAC publication. */
	struct mutex mac_lock;
	enum cnss_utils_cc_src cc_source;
	struct dentry *root_dentry;
	/* generic mutex for device_id */
	struct mutex cnss_device_id_lock;
	struct cnss_utils_status_notifier
			notifier_ctx[CNSS_UTILS_MAX_STATUS_TYPE];
	enum cnss_utils_device_type cnss_device_type;
#ifdef CONFIG_FEATURE_SMEM_MAILBOX
	/* Serializes mailbox start, writes and provider retry state. */
	struct mutex mailbox_lock;
	bool smem_mailbox_initialized;
	int smem_mailbox_id;
#endif
} *cnss_utils_priv;

int cnss_utils_set_wlan_unsafe_channel(struct device *dev,
				       u16 *unsafe_ch_list, u16 ch_count)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;

	if (!priv)
		return -EINVAL;

	mutex_lock(&priv->unsafe_channel_list_lock);
	if (!unsafe_ch_list || ch_count > CNSS_MAX_CH_NUM) {
		mutex_unlock(&priv->unsafe_channel_list_lock);
		return -EINVAL;
	}

	priv->unsafe_channel_list.unsafe_ch_count = ch_count;

	if (ch_count == 0)
		goto end;

	memcpy(priv->unsafe_channel_list.unsafe_ch_list,
	       unsafe_ch_list, ch_count * sizeof(u16));

end:
	mutex_unlock(&priv->unsafe_channel_list_lock);

	return 0;
}
EXPORT_SYMBOL(cnss_utils_set_wlan_unsafe_channel);

int cnss_utils_get_wlan_unsafe_channel(struct device *dev,
				       u16 *unsafe_ch_list,
				       u16 *ch_count, u16 buf_len)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;

	if (!priv)
		return -EINVAL;

	mutex_lock(&priv->unsafe_channel_list_lock);
	if (!unsafe_ch_list || !ch_count) {
		mutex_unlock(&priv->unsafe_channel_list_lock);
		return -EINVAL;
	}

	if (buf_len <
	    (priv->unsafe_channel_list.unsafe_ch_count * sizeof(u16))) {
		mutex_unlock(&priv->unsafe_channel_list_lock);
		return -ENOMEM;
	}

	*ch_count = priv->unsafe_channel_list.unsafe_ch_count;
	memcpy(unsafe_ch_list, priv->unsafe_channel_list.unsafe_ch_list,
	       priv->unsafe_channel_list.unsafe_ch_count * sizeof(u16));
	mutex_unlock(&priv->unsafe_channel_list_lock);

	return 0;
}
EXPORT_SYMBOL(cnss_utils_get_wlan_unsafe_channel);

enum cnss_utils_device_type cnss_utils_update_device_type(
			enum cnss_utils_device_type  device_type)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;
	enum cnss_utils_device_type result;

	if (!priv)
		return -EINVAL;

	mutex_lock(&priv->cnss_device_id_lock);
	pr_info("cnss_utils: device type:%d\n", device_type);
	if (priv->cnss_device_type == CNSS_UNSUPPORETD_DEVICE_TYPE) {
		priv->cnss_device_type = device_type;
		pr_info("cnss_utils: set device type:%d\n",
			priv->cnss_device_type);
	} else {
		pr_info("cnss_utils: device type already set :%d\n",
			priv->cnss_device_type);
	}
	result = priv->cnss_device_type;
	mutex_unlock(&priv->cnss_device_id_lock);
	return result;
}
EXPORT_SYMBOL(cnss_utils_update_device_type);
static void cnss_utils_status_update_user(enum cnss_status_type status_type,
					  bool status)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;
	struct cnss_utils_status_registration *entry;
	int i, idx;

	if (!priv || status_type < 0 || status_type >= CNSS_UTILS_MAX_STATUS_TYPE)
		return;
	idx = srcu_read_lock(&utils_notifier_srcu);
	for (i = 0; i < CNSS_UTILS_NOTIFIER_MAX_USER; i++) {
		entry = srcu_dereference(priv->notifier_ctx[status_type].users[i],
					 &utils_notifier_srcu);
		if (entry)
			entry->callback(entry->context, status);
	}
	srcu_read_unlock(&utils_notifier_srcu, idx);
}

int cnss_utils_fmd_status(int is_enabled)
{
	pr_info("cnss_utils: FMD status:%d\n", is_enabled);

	if (is_enabled)
		cnss_utils_status_update_user(CNSS_UTILS_FMD_STATUS,
					      true);
	else
		cnss_utils_status_update_user(CNSS_UTILS_FMD_STATUS,
					      false);

	return 0;
}
EXPORT_SYMBOL(cnss_utils_fmd_status);
int cnss_utils_register_status_notifier(enum cnss_status_type status_type,
					cnss_utils_status_update status_update_cb,
				       void *cb_ctx)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;
	struct cnss_utils_status_registration *entry, *new_entry;
	struct cnss_utils_status_notifier *notifier;
	int i, slot = -1, ret = 0;

	if (!priv || !status_update_cb || status_type < 0 ||
	    status_type >= CNSS_UTILS_MAX_STATUS_TYPE)
		return -EINVAL;
	new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry)
		return -ENOMEM;
	new_entry->callback = status_update_cb;
	new_entry->context = cb_ctx;
	notifier = &priv->notifier_ctx[status_type];
	mutex_lock(&utils_notifier_lock);
	for (i = 0; i < CNSS_UTILS_NOTIFIER_MAX_USER; i++) {
		entry = rcu_dereference_protected(notifier->users[i],
						  lockdep_is_held(&utils_notifier_lock));
		if (!entry) {
			if (slot < 0)
				slot = i;
		} else if (entry->callback == status_update_cb && entry->context == cb_ctx) {
			ret = -EEXIST;
			goto out;
		}
	}
	if (slot < 0) {
		ret = -ENOSPC;
		goto out;
	}
	rcu_assign_pointer(notifier->users[slot], new_entry);
	new_entry = NULL;
out:
	mutex_unlock(&utils_notifier_lock);
	kfree(new_entry);
	return ret;
}
EXPORT_SYMBOL(cnss_utils_register_status_notifier);

int cnss_utils_unregister_status_notifier(enum cnss_status_type status_type,
					  cnss_utils_status_update callback,
					 void *context)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;
	struct cnss_utils_status_registration *entry = NULL, *candidate;
	int i;

	if (!priv || !callback || status_type < 0 ||
	    status_type >= CNSS_UTILS_MAX_STATUS_TYPE)
		return -EINVAL;
	mutex_lock(&utils_notifier_lock);
	for (i = 0; i < CNSS_UTILS_NOTIFIER_MAX_USER; i++) {
		candidate = rcu_dereference_protected(priv->notifier_ctx[status_type].users[i],
						      lockdep_is_held(&utils_notifier_lock));
		if (candidate && candidate->callback == callback && candidate->context == context) {
			entry = candidate;
			RCU_INIT_POINTER(priv->notifier_ctx[status_type].users[i], NULL);
			break;
		}
	}
	mutex_unlock(&utils_notifier_lock);
	if (!entry)
		return -ENOENT;
	synchronize_srcu(&utils_notifier_srcu);
	kfree(entry);
	return 0;
}
EXPORT_SYMBOL(cnss_utils_unregister_status_notifier);

int cnss_utils_wlan_set_dfs_nol(struct device *dev,
				const void *info, u16 info_len)
{
	void *temp;
	void *old_nol_info;
	struct cnss_dfs_nol_info *dfs_info;
	struct cnss_utils_priv *priv = cnss_utils_priv;

	if (!priv)
		return -EINVAL;

	if (!info || !info_len)
		return -EINVAL;

	temp = kmemdup(info, info_len, GFP_ATOMIC);
	if (!temp)
		return -ENOMEM;

	spin_lock_bh(&priv->dfs_nol_info_lock);
	dfs_info = &priv->dfs_nol_info;
	old_nol_info = dfs_info->dfs_nol_info;
	dfs_info->dfs_nol_info = temp;
	dfs_info->dfs_nol_info_len = info_len;
	spin_unlock_bh(&priv->dfs_nol_info_lock);
	kfree(old_nol_info);

	return 0;
}
EXPORT_SYMBOL(cnss_utils_wlan_set_dfs_nol);

int cnss_utils_wlan_get_dfs_nol(struct device *dev,
				void *info, u16 info_len)
{
	int len;
	struct cnss_dfs_nol_info *dfs_info;
	struct cnss_utils_priv *priv = cnss_utils_priv;

	if (!priv)
		return -EINVAL;

	if (!info || !info_len)
		return -EINVAL;

	spin_lock_bh(&priv->dfs_nol_info_lock);

	dfs_info = &priv->dfs_nol_info;
	if (!dfs_info->dfs_nol_info ||
	    dfs_info->dfs_nol_info_len == 0) {
		spin_unlock_bh(&priv->dfs_nol_info_lock);
		return -ENOENT;
	}

	len = min(info_len, dfs_info->dfs_nol_info_len);
	memcpy(info, dfs_info->dfs_nol_info, len);
	spin_unlock_bh(&priv->dfs_nol_info_lock);

	return len;
}
EXPORT_SYMBOL(cnss_utils_wlan_get_dfs_nol);

void cnss_utils_increment_driver_load_cnt(struct device *dev)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;

	if (!priv)
		return;

	atomic_inc(&priv->driver_load_cnt);
}
EXPORT_SYMBOL(cnss_utils_increment_driver_load_cnt);

int cnss_utils_get_driver_load_cnt(struct device *dev)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;

	if (!priv)
		return -EINVAL;

	return atomic_read(&priv->driver_load_cnt);
}
EXPORT_SYMBOL(cnss_utils_get_driver_load_cnt);

static int set_wlan_mac_address(const u8 *mac_list, const uint32_t len,
				enum mac_type type)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;
	u32 no_of_mac_addr;
	struct cnss_wlan_mac_addr *addr = NULL;
	int iter;
	u8 *temp = NULL;

	if (!priv)
		return -EINVAL;

	if (!mac_list || len == 0 || (len % ETH_ALEN) != 0) {
		pr_err("Invalid length %d\n", len);
		return -EINVAL;
	}

	no_of_mac_addr = len / ETH_ALEN;
	if (no_of_mac_addr > MAX_NO_OF_MAC_ADDR) {
		pr_err("Exceed maximum supported MAC address %u %u\n",
		       MAX_NO_OF_MAC_ADDR, no_of_mac_addr);
		return -EINVAL;
	}

	if (type != CNSS_MAC_PROVISIONED && type != CNSS_MAC_DERIVED)
		return -EINVAL;

	mutex_lock(&priv->mac_lock);
	if (type == CNSS_MAC_PROVISIONED)
		addr = &priv->wlan_mac_addr;
	else
		addr = &priv->wlan_der_mac_addr;

	/* Pair with the first publisher before reading immutable MAC bytes. */
	if (smp_load_acquire(&addr->no_of_mac_addr_set)) {
		pr_err("WLAN MAC address is already set, num %d type %d\n",
		       addr->no_of_mac_addr_set, type);
		mutex_unlock(&priv->mac_lock);
		return 0;
	}

	temp = &addr->mac_addr[0][0];

	for (iter = 0; iter < no_of_mac_addr;
	     ++iter, temp += ETH_ALEN, mac_list += ETH_ALEN) {
		ether_addr_copy(temp, mac_list);
		pr_debug("MAC_ADDR:%02x:%02x:%02x:%02x:%02x:%02x\n",
			 temp[0], temp[1], temp[2],
			 temp[3], temp[4], temp[5]);
	}
	/* Publish all MAC bytes before readers observe a nonzero count. */
	smp_store_release(&addr->no_of_mac_addr_set, no_of_mac_addr);
	mutex_unlock(&priv->mac_lock);
	return 0;
}

int cnss_utils_set_wlan_mac_address(const u8 *mac_list, const uint32_t len)
{
	return set_wlan_mac_address(mac_list, len, CNSS_MAC_PROVISIONED);
}
EXPORT_SYMBOL(cnss_utils_set_wlan_mac_address);

int cnss_utils_set_wlan_derived_mac_address(const u8 *mac_list,
					    const uint32_t len)
{
	return set_wlan_mac_address(mac_list, len, CNSS_MAC_DERIVED);
}
EXPORT_SYMBOL(cnss_utils_set_wlan_derived_mac_address);

static u8 *get_wlan_mac_address(struct device *dev,
				u32 *num, enum mac_type type)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;
	struct cnss_wlan_mac_addr *addr = NULL;

	if (!num)
		return NULL;
	if (!priv)
		goto out;

	if (type == CNSS_MAC_PROVISIONED)
		addr = &priv->wlan_mac_addr;
	else
		addr = &priv->wlan_der_mac_addr;

	/* Pair with the one-time MAC publication. */
	*num = smp_load_acquire(&addr->no_of_mac_addr_set);
	if (!*num) {
		pr_err("WLAN MAC address is not set, type %d\n", type);
		goto out;
	}
	return &addr->mac_addr[0][0];

out:
	*num = 0;
	return NULL;
}

u8 *cnss_utils_get_wlan_mac_address(struct device *dev, uint32_t *num)
{
	return get_wlan_mac_address(dev, num, CNSS_MAC_PROVISIONED);
}
EXPORT_SYMBOL(cnss_utils_get_wlan_mac_address);

u8 *cnss_utils_get_wlan_derived_mac_address(struct device *dev,
					    uint32_t *num)
{
	return get_wlan_mac_address(dev, num, CNSS_MAC_DERIVED);
}
EXPORT_SYMBOL(cnss_utils_get_wlan_derived_mac_address);

void cnss_utils_set_cc_source(struct device *dev,
			      enum cnss_utils_cc_src cc_source)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;

	if (!priv)
		return;

	WRITE_ONCE(priv->cc_source, cc_source);
}
EXPORT_SYMBOL(cnss_utils_set_cc_source);

enum cnss_utils_cc_src cnss_utils_get_cc_source(struct device *dev)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;

	if (!priv)
		return -EINVAL;
	return READ_ONCE(priv->cc_source);
}
EXPORT_SYMBOL(cnss_utils_get_cc_source);

#ifdef CONFIG_FEATURE_SMEM_MAILBOX
int cnss_utils_smem_mailbox_write(struct device *dev, int flags,
				  const __u8 *data, uint32_t len)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;
	int ret;

	if (!priv)
		return -EINVAL;
	if ((!data && len) || len > U16_MAX)
		return -EMSGSIZE;
	mutex_lock(&priv->mailbox_lock);
	if (!priv->smem_mailbox_initialized) {
		ret = smem_mailbox_start(priv->smem_mailbox_id, NULL);
		if (ret != 1)
			goto out;
		priv->smem_mailbox_initialized = true;
	}
	ret = smem_mailbox_write(priv->smem_mailbox_id, flags, (__u8 *)data, len);
	if (ret == -ENODEV || ret == -EAGAIN || ret == -EPERM)
		priv->smem_mailbox_initialized = false;
out:
	mutex_unlock(&priv->mailbox_lock);
	return ret;
}
EXPORT_SYMBOL(cnss_utils_smem_mailbox_write);
#endif

static ssize_t cnss_utils_mac_write(struct file *fp,
				    const char __user *user_buf,
				    size_t count, loff_t *off)
{
	char buf[128];
	char *input, *mac_type, *mac_address;
	u8 parsed[MAX_NO_OF_MAC_ADDR * ETH_ALEN];
	u8 *dest_mac = parsed;
	u8 val;
	enum mac_type type;
	int ret;
	const char *delim = "\n";
	size_t len = 0;
	char temp[3] = "";

	if (!count || count >= sizeof(buf))
		return -EINVAL;
	len = count;
	if (copy_from_user(buf, user_buf, len))
		return -EFAULT;
	buf[len] = '\0';

	input = buf;

	mac_type = strsep(&input, delim);
	if (!mac_type)
		return -EINVAL;
	if (!input)
		return -EINVAL;

	mac_address = strsep(&input, delim);
	if (!mac_address)
		return -EINVAL;
	if (strncmp("0x", mac_address, MAC_PREFIX_LEN)) {
		pr_err("Invalid MAC prefix\n");
		return -EINVAL;
	}

	len = strlen(mac_address);
	mac_address += MAC_PREFIX_LEN;
	len -= MAC_PREFIX_LEN;
	if (len < ETH_ALEN * 2 || len > ETH_ALEN * 2 * MAX_NO_OF_MAC_ADDR ||
	    len % (ETH_ALEN * 2) != 0) {
		pr_err("Invalid MAC address length %zu\n", len);
		return -EINVAL;
	}

	if (!strcmp("provisioned", mac_type)) {
		type = CNSS_MAC_PROVISIONED;
	} else if (!strcmp("derived", mac_type)) {
		type = CNSS_MAC_DERIVED;
	} else {
		pr_err("Invalid MAC address type %s\n", mac_type);
		return -EINVAL;
	}

	len /= 2;
	for (size_t i = 0; i < len; i++) {
		temp[0] = *mac_address++;
		temp[1] = *mac_address++;
		if (kstrtou8(temp, 16, &val))
			return -EINVAL;
		*dest_mac++ = val;
	}
	ret = set_wlan_mac_address(parsed, len, type);
	if (ret)
		return ret;
	return count;
}

static int cnss_utils_mac_show(struct seq_file *s, void *data)
{
	u8 mac[6];
	u32 count;
	int i;
	struct cnss_utils_priv *priv = s->private;
	struct cnss_wlan_mac_addr *addr = NULL;

	addr = &priv->wlan_mac_addr;
	/* Pair with the one-time MAC publication. */
	count = smp_load_acquire(&addr->no_of_mac_addr_set);
	if (count) {
		seq_puts(s, "\nProvisioned MAC addresseses\n");
		for (i = 0; i < count; i++) {
			ether_addr_copy(mac, addr->mac_addr[i]);
			seq_printf(s, "MAC_ADDR:%02x:%02x:%02x:%02x:%02x:%02x\n",
				   mac[0], mac[1], mac[2],
				   mac[3], mac[4], mac[5]);
		}
	}

	addr = &priv->wlan_der_mac_addr;
	/* Pair with the one-time MAC publication. */
	count = smp_load_acquire(&addr->no_of_mac_addr_set);
	if (count) {
		seq_puts(s, "\nDerived MAC addresseses\n");
		for (i = 0; i < count; i++) {
			ether_addr_copy(mac, addr->mac_addr[i]);
			seq_printf(s, "MAC_ADDR:%02x:%02x:%02x:%02x:%02x:%02x\n",
				   mac[0], mac[1], mac[2],
				   mac[3], mac[4], mac[5]);
		}
	}

	return 0;
}

static int cnss_utils_mac_open(struct inode *inode, struct file *file)
{
	return single_open(file, cnss_utils_mac_show, inode->i_private);
}

static const struct file_operations cnss_utils_mac_fops = {
	.read		= seq_read,
	.write		= cnss_utils_mac_write,
	.release	= single_release,
	.open		= cnss_utils_mac_open,
	.owner		= THIS_MODULE,
	.llseek		= seq_lseek,
};

static int cnss_utils_debugfs_create(struct cnss_utils_priv *priv)
{
	int ret = 0;
	struct dentry *root_dentry;

	root_dentry = debugfs_create_dir("cnss_utils", NULL);

	if (IS_ERR(root_dentry)) {
		ret = PTR_ERR(root_dentry);
		pr_err("Unable to create debugfs %d\n", ret);
		goto out;
	}
	priv->root_dentry = root_dentry;
	debugfs_create_file("mac_address", 0600, root_dentry, priv,
			    &cnss_utils_mac_fops);
out:
	return ret;
}

/**
 * cnss_utils_is_valid_dt_node_found - Check if valid device tree node present
 *
 * Valid device tree node means a node with "qcom,wlan" property present and
 * "status" property not disabled.
 *
 * Return: true if valid device tree node found, false if not found
 */
static bool cnss_utils_is_valid_dt_node_found(void)
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

#ifdef CONFIG_FEATURE_SMEM_MAILBOX
static void cnss_utils_smem_mailbox_init(void)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;

	mutex_init(&priv->mailbox_lock);
	priv->smem_mailbox_id = 0;
	priv->smem_mailbox_initialized = false;
}

static void cnss_utils_smem_mailbox_deinit(void)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;

	smem_mailbox_stop(priv->smem_mailbox_id);
}
#else
static void cnss_utils_smem_mailbox_init(void)
{
}

static void cnss_utils_smem_mailbox_deinit(void)
{
}
#endif

static int __init cnss_utils_init(void)
{
	struct cnss_utils_priv *priv = NULL;

	if (!cnss_utils_is_valid_dt_node_found()) {
		pr_err("device node not found!\n");
		return -ENODEV;
	}
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->cc_source = CNSS_UTILS_SOURCE_CORE;
	priv->cnss_device_type = CNSS_UNSUPPORETD_DEVICE_TYPE;

	mutex_init(&priv->unsafe_channel_list_lock);
	mutex_init(&priv->cnss_device_id_lock);
	mutex_init(&priv->mac_lock);
	spin_lock_init(&priv->dfs_nol_info_lock);
	atomic_set(&priv->driver_load_cnt, 0);
	cnss_utils_priv = priv;
	cnss_utils_smem_mailbox_init();
	/* Native getters publish raw MAC storage; retain it for this boot. */
	__module_get(THIS_MODULE);
	cnss_utils_debugfs_create(priv);
	return 0;
}

static void __exit cnss_utils_exit(void)
{
	struct cnss_utils_priv *priv = cnss_utils_priv;

	debugfs_remove_recursive(priv->root_dentry);
	cnss_utils_smem_mailbox_deinit();
	cnss_utils_priv = NULL;
	kfree(priv->dfs_nol_info.dfs_nol_info);
	kfree(priv);
}

module_init(cnss_utils_init);
module_exit(cnss_utils_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("CNSS Utilities Driver");
