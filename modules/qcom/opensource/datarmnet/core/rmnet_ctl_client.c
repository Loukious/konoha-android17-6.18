// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * RMNET_CTL client handlers
 *
 */

#include <linux/debugfs.h>
#include <linux/mutex.h>
#include <linux/ipc_logging.h>
#include <linux/version.h>
#include "rmnet_ctl.h"
#include "rmnet_ctl_client.h"

#define RMNET_CTL_LOG_PAGE 10
#define RMNET_CTL_LOG_NAME "rmnet_ctl"
#define RMNET_CTL_LOG_LVL  "ipc_log_lvl"

struct rmnet_ctl_client {
	struct rmnet_ctl_client_hooks hooks;
};

struct rmnet_ctl_endpoint {
	struct rmnet_ctl_dev __rcu *dev;
	struct rmnet_ctl_client __rcu *client;
	struct dentry *dbgfs_dir;
	struct dentry *dbgfs_loglvl;
	void __rcu *ipc_log;
};

#if defined(CONFIG_IPA_DEBUG) || defined(CONFIG_MHI_DEBUG)
#define CONFIG_RMNET_CTL_DEBUG 1
#endif

#ifdef CONFIG_RMNET_CTL_DEBUG
static u8 ipc_log_lvl = RMNET_CTL_LOG_DEBUG;
#else
static u8 ipc_log_lvl = RMNET_CTL_LOG_ERR;
#endif

static DEFINE_MUTEX(client_lock);
static struct rmnet_ctl_endpoint ctl_ep;

void rmnet_ctl_set_dbgfs(bool enable)
{
	if (enable) {
		if (IS_ERR_OR_NULL(ctl_ep.dbgfs_dir))
			ctl_ep.dbgfs_dir = debugfs_create_dir(
				RMNET_CTL_LOG_NAME, NULL);

		if (!IS_ERR_OR_NULL(ctl_ep.dbgfs_dir))
			debugfs_create_u8((const char *) RMNET_CTL_LOG_LVL,
					  (umode_t) 0644,
					  (struct dentry *) ctl_ep.dbgfs_dir,
					  (u8 *) &ipc_log_lvl);

		if (!rcu_access_pointer(ctl_ep.ipc_log)) {
			void *log;

			log = ipc_log_context_create(RMNET_CTL_LOG_PAGE,
						     RMNET_CTL_LOG_NAME, 0);

			rcu_assign_pointer(ctl_ep.ipc_log, log);
		}
	} else {
		void *log = rcu_access_pointer(ctl_ep.ipc_log);

		RCU_INIT_POINTER(ctl_ep.ipc_log, NULL);
		synchronize_rcu();
		debugfs_remove_recursive(ctl_ep.dbgfs_dir);
		ipc_log_context_destroy(log);
		ctl_ep.dbgfs_dir = NULL;
		ctl_ep.dbgfs_loglvl = NULL;
	}
}

void rmnet_ctl_endpoint_setdev(const struct rmnet_ctl_dev *dev)
{
	rcu_assign_pointer(ctl_ep.dev, dev);
}

void rmnet_ctl_endpoint_post(const void *data, size_t len)
{
	struct rmnet_ctl_client *client;
	struct sk_buff *skb;

	if (unlikely(!data || !len || len > U32_MAX))
		return;

	if (len == 0xFFFFFFFF) {
		skb = (struct sk_buff *)data;
		rmnet_ctl_log_info("RX", skb->data, skb->len);

		rcu_read_lock();

		client = rcu_dereference(ctl_ep.client);
		if (client && client->hooks.ctl_dl_client_hook) {
			skb->protocol = htons(ETH_P_MAP);
			client->hooks.ctl_dl_client_hook(skb);
		} else {
			kfree_skb(skb);
		}

		rcu_read_unlock();
	} else {
		rmnet_ctl_log_info("RX", data, len);

		rcu_read_lock();

		client = rcu_dereference(ctl_ep.client);
		if (client && client->hooks.ctl_dl_client_hook) {
			skb = alloc_skb(len, GFP_ATOMIC);
			if (skb) {
				skb_put_data(skb, data, len);
				skb->protocol = htons(ETH_P_MAP);
				client->hooks.ctl_dl_client_hook(skb);
			}
		}

		rcu_read_unlock();
	}
}

void *rmnet_ctl_register_client(struct rmnet_ctl_client_hooks *hook)
{
	struct rmnet_ctl_client *client;

	if (!hook)
		return NULL;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return NULL;
	client->hooks = *hook;

	mutex_lock(&client_lock);

	/* Only support one client for now */
	if (rcu_dereference(ctl_ep.client)) {
		mutex_unlock(&client_lock);
		kfree(client);
		return NULL;
	}

	rmnet_ctl_set_dbgfs(true);
	rcu_assign_pointer(ctl_ep.client, client);
	mutex_unlock(&client_lock);

	return client;
}
EXPORT_SYMBOL(rmnet_ctl_register_client);

int rmnet_ctl_unregister_client(void *handle)
{
	struct rmnet_ctl_client *client = (struct rmnet_ctl_client *)handle;

	mutex_lock(&client_lock);

	if (!client || rcu_access_pointer(ctl_ep.client) != client) {
		mutex_unlock(&client_lock);
		return -EINVAL;
	}

	RCU_INIT_POINTER(ctl_ep.client, NULL);
	synchronize_rcu();
	kfree(client);

	rmnet_ctl_set_dbgfs(false);
	mutex_unlock(&client_lock);
	return 0;
}
EXPORT_SYMBOL(rmnet_ctl_unregister_client);

int rmnet_ctl_send_client(void *handle, struct sk_buff *skb)
{
	struct rmnet_ctl_client *client = handle;
	struct rmnet_ctl_dev *dev;
	int rc = -EINVAL;

	if (!skb)
		return rc;
	rcu_read_lock();
	if (!client || client != rcu_dereference(ctl_ep.client)) {
		kfree_skb(skb);
		goto unlock;
	}
	rmnet_ctl_log_info("TX", skb->data, skb->len);
	dev = rcu_dereference(ctl_ep.dev);
	if (dev && dev->xmit)
		rc = dev->xmit(dev, skb);
	else
		kfree_skb(skb);
unlock:
	rcu_read_unlock();
	if (rc)
		rmnet_ctl_log_err("TXE", rc, NULL, 0);
	return rc;
}
EXPORT_SYMBOL(rmnet_ctl_send_client);

void rmnet_ctl_log(enum rmnet_ctl_log_lvl lvl, const char *msg,
		   int rc, const void *data, unsigned int len)
{
	void *log;

	rcu_read_lock();
	log = rcu_dereference(ctl_ep.ipc_log);
	if (lvl <= READ_ONCE(ipc_log_lvl) && log) {
		if (!data || !len)
			ipc_log_string(log, "%3s(%d): (null)\n", msg, rc);
		else
			ipc_log_string(log, "%3s(%d): %*ph\n",
				       msg, rc, len > 32 ? 32 : len, data);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(rmnet_ctl_log);

static struct rmnet_ctl_client_if client_if = {
	.reg = rmnet_ctl_register_client,
	.dereg = rmnet_ctl_unregister_client,
	.send = rmnet_ctl_send_client,
	.log = rmnet_ctl_log,
};

struct rmnet_ctl_client_if *rmnet_ctl_if(void)
{
	return &client_if;
}
EXPORT_SYMBOL(rmnet_ctl_if);

int rmnet_ctl_get_stats(u64 *s, int n)
{
	struct rmnet_ctl_dev *dev;

	if (n < 0 || (n && !s))
		return -EINVAL;
	rcu_read_lock();
	dev = rcu_dereference(ctl_ep.dev);
	if (dev && n > 0) {
		n = min(n, (int)(sizeof(dev->stats) / sizeof(u64)));
		memcpy(s, &dev->stats, n * sizeof(u64));
	} else {
		n = 0;
	}
	rcu_read_unlock();

	return n;
}
EXPORT_SYMBOL(rmnet_ctl_get_stats);
