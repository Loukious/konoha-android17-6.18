// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2017, 2020-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/remoteproc.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <dsp/audio_notifier.h>
#include "audio_ssr.h"
#include "audio_pdr.h"

/* Audio states internal to notifier. Client */
/* used states defined in audio_notifier.h */
/* for AUDIO_NOTIFIER_SERVICE_DOWN & UP */
#define NO_SERVICE -2
#define UNINIT_SERVICE -1

static bool notifier_ready;
static struct platform_device *adsp_private;

struct adsp_notify_private {
	struct rproc *rproc_h;
	bool notifier_probe_complete;
};

/*
 * Used for each client registered with audio notifier
 */
struct client_data {
	struct list_head        list;
	/* Notifier block given by client */
	struct notifier_block   *nb;
	char                    client_name[20];
	int                     service;
	int                     domain;
};

/*
 * Used for each service and domain combination
 * Tracks information specific to the underlying
 * service.
 */
struct service_info {
	const char                      name[20];
	int                             domain_id;
	int                             state;
	void                            *handle;
	/* Hook registered to service */
	union {
		void (*cb)(int, char *, void *);
		struct notifier_block *nb;
	} hook;
	/* Used to determine when to register and deregister service */
	int                             num_of_clients;
	/* List of all clients registered to the service and domain */
	struct srcu_notifier_head       client_nb_list;
	/* Serialize provider events and the initial client snapshot. */
	struct mutex                   callback_mutex;
	bool                           early_down;
};

static int audio_notifier_ssr_adsp_cb(struct notifier_block *this,
				     unsigned long opcode, void *data);
static int audio_notifier_ssr_modem_cb(struct notifier_block *this,
				     unsigned long opcode, void *data);
static void audio_notifier_pdr_adsp_cb(int status, char *service_name, void *priv);

static struct notifier_block notifier_ssr_adsp_nb = {
	.notifier_call  = audio_notifier_ssr_adsp_cb,
	.priority = 0,
};

static struct notifier_block notifier_ssr_modem_nb = {
	.notifier_call  = audio_notifier_ssr_modem_cb,
	.priority = 0,
};

static struct service_info service_data[AUDIO_NOTIFIER_MAX_SERVICES]
				       [AUDIO_NOTIFIER_MAX_DOMAINS] = {

	{{
		.name = "SSR_ADSP",
		.domain_id = AUDIO_SSR_DOMAIN_ADSP,
		.state = AUDIO_NOTIFIER_SERVICE_DOWN,
		.hook.nb = &notifier_ssr_adsp_nb
	},
	{
		.name = "SSR_MODEM",
		.domain_id = AUDIO_SSR_DOMAIN_MODEM,
		.state = AUDIO_NOTIFIER_SERVICE_DOWN,
		.hook.nb = &notifier_ssr_modem_nb
	} },

	{{
		.name = "PDR_ADSP",
		.domain_id = AUDIO_PDR_DOMAIN_ADSP,
		.state = UNINIT_SERVICE,
		.hook.cb = &audio_notifier_pdr_adsp_cb
	},
	{	/* PDR MODEM service not enabled */
		.name = "INVALID",
		.state = NO_SERVICE,
		.hook.nb = NULL
	} }
};

/* Master list of all audio notifier clients */
LIST_HEAD(client_list);
static DEFINE_MUTEX(notifier_mutex);

static int audio_notifier_get_default_service(int domain)
{
	int service = NO_SERVICE;

	/* initial service to connect per domain */
	switch (domain) {
	case AUDIO_NOTIFIER_ADSP_DOMAIN:
		service = AUDIO_NOTIFIER_PDR_SERVICE;
		break;
	case AUDIO_NOTIFIER_MODEM_DOMAIN:
		service = AUDIO_NOTIFIER_SSR_SERVICE;
		break;
	}

	return service;
}

#ifdef CONFIG_MSM_QDSP6_PDR
static void audio_notifier_init_service(int service)
{
	int i;

	for (i = 0; i < AUDIO_NOTIFIER_MAX_DOMAINS; i++) {
		if (service_data[service][i].state == UNINIT_SERVICE)
			service_data[service][i].state =
				AUDIO_NOTIFIER_SERVICE_DOWN;
	}
}
#else
static void audio_notifier_init_service(int service)
{
	int i;

	for (i = 0; i < AUDIO_NOTIFIER_MAX_DOMAINS; i++)
		service_data[service][i].state = NO_SERVICE;
}
#endif

static int audio_notifier_reg_service(int service, int domain)
{
	struct service_info *info = &service_data[service][domain];
	struct adsp_notify_private *priv;
	void *handle;

	if (!notifier_ready || !adsp_private)
		return -EPROBE_DEFER;
	priv = platform_get_drvdata(adsp_private);
	if (!priv || !priv->rproc_h)
		return -EPROBE_DEFER;
	if (info->handle)
		return 0;

	/* Publish the initial state before the provider can call us. */
	mutex_lock(&info->callback_mutex);
	info->state = AUDIO_NOTIFIER_SERVICE_DOWN;
	info->early_down = false;
	mutex_unlock(&info->callback_mutex);
	switch (service) {
	case AUDIO_NOTIFIER_SSR_SERVICE:
		handle = audio_ssr_register(domain == AUDIO_NOTIFIER_MODEM_DOMAIN ?
					    "modem" : priv->rproc_h->name,
					    info->hook.nb);
		break;
	case AUDIO_NOTIFIER_PDR_SERVICE:
		handle = audio_pdr_service_register(info->domain_id, info->hook.cb);
		break;
	default:
		return -EINVAL;
	}
	if (IS_ERR_OR_NULL(handle))
		return handle ? PTR_ERR(handle) : -EINVAL;
	info->handle = handle;
	return 0;
}

static int audio_notifier_dereg_service(int service, int domain)
{
	struct service_info *info = &service_data[service][domain];
	int ret;

	if (!info->handle)
		return 0;
	/* Provider release drains callbacks; they never take notifier_mutex. */
	switch (service) {
	case AUDIO_NOTIFIER_SSR_SERVICE:
		ret = audio_ssr_deregister(info->handle, info->hook.nb);
		break;
	case AUDIO_NOTIFIER_PDR_SERVICE:
		ret = audio_pdr_service_deregister(info->domain_id);
		break;
	default:
		return -EINVAL;
	}
	if (ret)
		return ret;
	info->handle = NULL;
	mutex_lock(&info->callback_mutex);
	info->state = AUDIO_NOTIFIER_SERVICE_DOWN;
	info->early_down = false;
	mutex_unlock(&info->callback_mutex);
	return 0;
}

static int audio_notifier_reg_client_service(struct client_data *client_data,
					    int service)
{
	int domain = client_data->domain;
	struct service_info *info = &service_data[service][domain];
	struct audio_notifier_cb_data data;
	int ret;

	if (service != AUDIO_NOTIFIER_SSR_SERVICE &&
	    service != AUDIO_NOTIFIER_PDR_SERVICE)
		return -EINVAL;
	if (info->num_of_clients == INT_MAX)
		return -EOVERFLOW;
	if (!info->num_of_clients) {
		ret = audio_notifier_reg_service(service, domain);
		if (ret)
			return ret;
	}
	ret = srcu_notifier_chain_register(&info->client_nb_list, client_data->nb);
	if (ret) {
		if (!info->num_of_clients)
			audio_notifier_dereg_service(service, domain);
		return ret;
	}
	client_data->service = service;
	info->num_of_clients++;
	if (service == AUDIO_NOTIFIER_PDR_SERVICE) {
		data.service = service;
		data.domain = domain;
		/* Serialize the initial snapshot with provider callbacks. */
		mutex_lock(&info->callback_mutex);
		client_data->nb->notifier_call(client_data->nb, info->state, &data);
		mutex_unlock(&info->callback_mutex);
	}
	return 0;
}

static int audio_notifier_reg_client(struct client_data *client_data)
{
	int ret = 0;
	int service;
	int domain = client_data->domain;

	service = audio_notifier_get_default_service(domain);
	if (service < 0) {
		pr_err_ratelimited("%s: service %d is incorrect\n", __func__, service);
		ret = -EINVAL;
		goto done;
	}

	/* Search through services to find a valid one to register client on. */
	for (; service >= 0; service--) {
		/* If a service is not initialized, wait for it to come up. */
		if (READ_ONCE(service_data[service][domain].state) == UNINIT_SERVICE) {
			pr_err_ratelimited("%s: failed in client registration to PDR\n",
				 __func__);
			ret = -EINVAL;
			goto done;
		}
		/* Skip unsupported service and domain combinations. */
		if (READ_ONCE(service_data[service][domain].state) < 0)
			continue;
		/* Only register clients who have not acquired a service. */
		if (client_data->service != NO_SERVICE)
			continue;

		/*
		 * Only register clients, who have not acquired a service, on
		 * the best available service for their domain. Uninitialized
		 * services will try to register all of their clients after
		 * they initialize correctly or will disable their service and
		 * register clients on the next best avaialable service.
		 */
		pr_debug("%s: register client %s on service %s",
				__func__, client_data->client_name,
				service_data[service][domain].name);

		ret = audio_notifier_reg_client_service(client_data, service);
		if (ret < 0)
			pr_err_ratelimited("%s: client %s failed to register on service %s",
				__func__, client_data->client_name,
				service_data[service][domain].name);
	}

done:
	return ret;
}

static int audio_notifier_dereg_client(struct client_data *client_data)
{
	int service = client_data->service;
	int domain = client_data->domain;
	struct service_info *info;
	int ret;

	if (service == NO_SERVICE)
		return 0;
	if (service != AUDIO_NOTIFIER_SSR_SERVICE &&
	    service != AUDIO_NOTIFIER_PDR_SERVICE)
		return -EINVAL;
	info = &service_data[service][domain];
	ret = srcu_notifier_chain_unregister(&info->client_nb_list, client_data->nb);
	if (ret)
		return ret;
	client_data->service = NO_SERVICE;
	info->num_of_clients--;
	if (!info->num_of_clients)
		return audio_notifier_dereg_service(service, domain);
	return 0;
}

static int audio_notifier_convert_opcode(struct service_info *info,
					unsigned long opcode,
					unsigned long *notifier_opcode)
{
	int ret = 0;

	switch (opcode) {
	case QCOM_SSR_BEFORE_SHUTDOWN:
	case SERVREG_SERVICE_STATE_EARLY_DOWN:
		*notifier_opcode = AUDIO_NOTIFIER_SERVICE_DOWN;
		if (opcode == SERVREG_SERVICE_STATE_EARLY_DOWN)
			info->early_down = true;
		break;
	case QCOM_SSR_AFTER_POWERUP:
	case SERVREG_SERVICE_STATE_UP:
		info->early_down = false;
		*notifier_opcode = AUDIO_NOTIFIER_SERVICE_UP;
		break;
	case SERVREG_SERVICE_STATE_DOWN:
		if (!info->early_down) {
			*notifier_opcode = AUDIO_NOTIFIER_SERVICE_DOWN;
		} else {
			/* Reset info->early_down and nothing to do*/
			info->early_down = false;
			ret = -EINVAL;
			pr_info("%s: Early down aleady handled %d\n", __func__,
						info->early_down);
		}
		break;
	default:
		pr_debug("%s: Unused opcode 0x%lx\n", __func__, opcode);
		ret = -EINVAL;
	}

	return ret;
}

static int audio_notifier_service_cb(unsigned long opcode,
				    int service, int domain)
{
	struct service_info *info = &service_data[service][domain];
	unsigned long notifier_opcode;
	struct audio_notifier_cb_data data = {
		.service = service,
		.domain = domain,
	};

	mutex_lock(&info->callback_mutex);
	if (!audio_notifier_convert_opcode(info, opcode, &notifier_opcode)) {
		info->state = notifier_opcode;
		srcu_notifier_call_chain(&info->client_nb_list,
					 notifier_opcode, &data);
	}
	mutex_unlock(&info->callback_mutex);
	return NOTIFY_OK;
}

static void audio_notifier_pdr_adsp_cb(int status, char *service_name, void *priv)
{
	audio_notifier_service_cb(status, AUDIO_NOTIFIER_PDR_SERVICE, AUDIO_NOTIFIER_ADSP_DOMAIN);
}

static int audio_notifier_ssr_adsp_cb(struct notifier_block *this,
				     unsigned long opcode, void *data)
{
	return audio_notifier_service_cb(opcode,
					AUDIO_NOTIFIER_SSR_SERVICE,
					AUDIO_NOTIFIER_ADSP_DOMAIN);
}

static int audio_notifier_ssr_modem_cb(struct notifier_block *this,
				      unsigned long opcode, void *data)
{
	return audio_notifier_service_cb(opcode,
					AUDIO_NOTIFIER_SSR_SERVICE,
					AUDIO_NOTIFIER_MODEM_DOMAIN);
}
int audio_notifier_deregister(char *client_name)
{
	struct client_data *client, *next;
	int ret = 0, error;

	if (!client_name)
		return -EINVAL;
	mutex_lock(&notifier_mutex);
	list_for_each_entry_safe(client, next, &client_list, list) {
		if (strcmp(client_name, client->client_name))
			continue;
		error = audio_notifier_dereg_client(client);
		if (error)
			ret = error;
		if (client->service != NO_SERVICE)
			continue;
		list_del(&client->list);
		kfree(client);
	}
	mutex_unlock(&notifier_mutex);
	return ret;
}
EXPORT_SYMBOL(audio_notifier_deregister);
int audio_notifier_register(char *client_name, int domain,
			    struct notifier_block *nb)
{
	struct client_data *client, *existing;
	int ret;

	if (!client_name || !client_name[0] || !nb || !nb->notifier_call ||
	    domain < 0 || domain >= AUDIO_NOTIFIER_MAX_DOMAINS)
		return -EINVAL;
	if (strnlen(client_name, sizeof(client->client_name)) >=
	    sizeof(client->client_name))
		return -ENAMETOOLONG;
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	INIT_LIST_HEAD(&client->list);
	client->nb = nb;
	strscpy(client->client_name, client_name, sizeof(client->client_name));
	client->service = NO_SERVICE;
	client->domain = domain;
	mutex_lock(&notifier_mutex);
	if (!notifier_ready) {
		ret = -EPROBE_DEFER;
		goto unlock;
	}
	/* One notifier_block cannot belong to two linked notifier chains. */
	list_for_each_entry(existing, &client_list, list) {
		if (existing->nb == nb) {
			ret = -EALREADY;
			goto unlock;
		}
	}
	ret = audio_notifier_reg_client(client);
	if (!ret)
		list_add_tail(&client->list, &client_list);
unlock:
	mutex_unlock(&notifier_mutex);
	if (ret)
		kfree(client);
	return ret;
}
EXPORT_SYMBOL(audio_notifier_register);

static int audio_notifier_subsys_init(void)
{
	int i, j;

	for (i = 0; i < AUDIO_NOTIFIER_MAX_SERVICES; i++) {
		for (j = 0; j < AUDIO_NOTIFIER_MAX_DOMAINS; j++) {
			if (service_data[i][j].state <= NO_SERVICE)
				continue;

			mutex_init(&service_data[i][j].callback_mutex);
			srcu_init_notifier_head(
				&service_data[i][j].client_nb_list);
		}
	}

	return 0;
}

bool audio_notifier_probe_status(void)
{
	/* Pair with probe publication before consumers start registering. */
	return smp_load_acquire(&notifier_ready);
}
EXPORT_SYMBOL(audio_notifier_probe_status);
static int audio_notify_probe(struct platform_device *pdev)
{
	struct adsp_notify_private *priv;
	u32 rproc_phandle;
	int ret;

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,rproc-handle",
				   &rproc_phandle);
	if (ret)
		return ret;
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->rproc_h = rproc_get_by_phandle(rproc_phandle);
	if (IS_ERR_OR_NULL(priv->rproc_h))
		return priv->rproc_h ? PTR_ERR(priv->rproc_h) : -EPROBE_DEFER;
	mutex_lock(&notifier_mutex);
	if (adsp_private) {
		mutex_unlock(&notifier_mutex);
		rproc_put(priv->rproc_h);
		return -EBUSY;
	}
	platform_set_drvdata(pdev, priv);
	adsp_private = pdev;
	audio_notifier_init_service(AUDIO_NOTIFIER_PDR_SERVICE);
	priv->notifier_probe_complete = true;
	/* Publish the initialized provider only after its remoteproc is held. */
	smp_store_release(&notifier_ready, true);
	mutex_unlock(&notifier_mutex);
	return 0;
}

static void audio_notify_remove(struct platform_device *pdev)
{
	struct adsp_notify_private *priv = platform_get_drvdata(pdev);
	struct client_data *client, *next;
	int i, j;

	mutex_lock(&notifier_mutex);
	/* Withdraw readiness before callbacks and remoteproc ownership drain. */
	smp_store_release(&notifier_ready, false);
	for (i = 0; i < AUDIO_NOTIFIER_MAX_SERVICES; i++)
		for (j = 0; j < AUDIO_NOTIFIER_MAX_DOMAINS; j++)
			if (service_data[i][j].handle)
				audio_notifier_service_cb(QCOM_SSR_BEFORE_SHUTDOWN,
							  i, j);
	list_for_each_entry_safe(client, next, &client_list, list) {
		audio_notifier_dereg_client(client);
		list_del(&client->list);
		kfree(client);
	}
	for (i = 0; i < AUDIO_NOTIFIER_MAX_SERVICES; i++)
		for (j = 0; j < AUDIO_NOTIFIER_MAX_DOMAINS; j++)
			if (service_data[i][j].handle)
				audio_notifier_dereg_service(i, j);
	adsp_private = NULL;
	platform_set_drvdata(pdev, NULL);
	mutex_unlock(&notifier_mutex);
	rproc_put(priv->rproc_h);
}

static const struct of_device_id adsp_notify_dt_match[] = {
	{ .compatible = "qcom,adsp-notify" },
	{ }
};
MODULE_DEVICE_TABLE(of, adsp_notify_dt_match);

static struct platform_driver adsp_notify_driver = {
	.driver = {
		.name = "adsp-notify",
		.owner = THIS_MODULE,
		.of_match_table = adsp_notify_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = audio_notify_probe,
	.remove = audio_notify_remove,
};
static int __init audio_notifier_init(void)
{
	int ret, i, j;

	audio_notifier_subsys_init();
	ret = platform_driver_register(&adsp_notify_driver);
	if (ret)
		for (i = 0; i < AUDIO_NOTIFIER_MAX_SERVICES; i++)
			for (j = 0; j < AUDIO_NOTIFIER_MAX_DOMAINS; j++)
				if (service_data[i][j].state > NO_SERVICE)
					srcu_cleanup_notifier_head(
						&service_data[i][j].client_nb_list);
	return ret;
}

module_init(audio_notifier_init);

static void __exit audio_notifier_exit(void)
{
	int i, j;

	platform_driver_unregister(&adsp_notify_driver);
	for (i = 0; i < AUDIO_NOTIFIER_MAX_SERVICES; i++)
		for (j = 0; j < AUDIO_NOTIFIER_MAX_DOMAINS; j++)
			if (service_data[i][j].state > NO_SERVICE)
				srcu_cleanup_notifier_head(
					&service_data[i][j].client_nb_list);
}
module_exit(audio_notifier_exit);

MODULE_SOFTDEP("pre: qcom_q6v5_pas");
MODULE_DESCRIPTION("Audio notifier driver");
MODULE_LICENSE("GPL v2");
