// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2017, 2020 The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include "audio_pdr.h"

struct audio_pdr_service {
	void *pdr_handle;
	bool releasing;
	char service_name[SERVREG_NAME_LENGTH + 1];
	char service_path[SERVREG_NAME_LENGTH + 1];
};

static DEFINE_MUTEX(audio_pdr_mutex);

static struct audio_pdr_service audio_pdr_services[AUDIO_PDR_DOMAIN_MAX] = {
	{	/* AUDIO_PDR_DOMAIN_ADSP */
		.service_name = "avs/audio",
		.service_path = "msm/adsp/audio_pd",
	}
};

void *audio_pdr_service_register(int domain_id, void (*cb)(int, char *, void *))
{
	struct audio_pdr_service *service;
	void *handle, *lookup;

	if ((domain_id < 0) ||
	    (domain_id >= AUDIO_PDR_DOMAIN_MAX) || !cb) {
		pr_err("%s: Invalid service ID %d\n", __func__, domain_id);
		return ERR_PTR(-EINVAL);
	}

	mutex_lock(&audio_pdr_mutex);
	service = &audio_pdr_services[domain_id];
	if (service->pdr_handle || service->releasing) {
		lookup = ERR_PTR(-EBUSY);
		goto unlock;
	}
	handle = pdr_handle_alloc(cb, NULL);
	if (IS_ERR_OR_NULL(handle)) {
		lookup = handle ?: ERR_PTR(-ENOMEM);
		goto unlock;
	}
	lookup = pdr_add_lookup(handle, service->service_name,
				service->service_path);
	if (IS_ERR_OR_NULL(lookup)) {
		lookup = lookup ?: ERR_PTR(-ENOMEM);
		/* Drain the provider without holding a lock needed by clients. */
		service->releasing = true;
		mutex_unlock(&audio_pdr_mutex);
		pdr_handle_release(handle);
		mutex_lock(&audio_pdr_mutex);
		service->releasing = false;
	} else {
		service->pdr_handle = handle;
	}

unlock:
	mutex_unlock(&audio_pdr_mutex);
	return lookup;
}
EXPORT_SYMBOL(audio_pdr_service_register);

int audio_pdr_service_deregister(int domain_id)
{
	struct audio_pdr_service *service;
	void *handle;

	if ((domain_id < 0) ||
	    (domain_id >= AUDIO_PDR_DOMAIN_MAX)) {
		pr_err("%s: Invalid service ID %d\n", __func__, domain_id);
		return -EINVAL;
	}
	mutex_lock(&audio_pdr_mutex);
	service = &audio_pdr_services[domain_id];
	if (service->releasing) {
		mutex_unlock(&audio_pdr_mutex);
		return -EBUSY;
	}
	handle = service->pdr_handle;
	service->pdr_handle = NULL;
	service->releasing = true;
	mutex_unlock(&audio_pdr_mutex);
	if (handle)
		pdr_handle_release(handle);
	mutex_lock(&audio_pdr_mutex);
	service->releasing = false;
	mutex_unlock(&audio_pdr_mutex);

	return 0;
}
EXPORT_SYMBOL(audio_pdr_service_deregister);

static int __init audio_pdr_late_init(void)
{
	return 0;
}
module_init(audio_pdr_late_init);

static void __exit audio_pdr_late_exit(void)
{
	int i;

	for (i = 0; i < AUDIO_PDR_DOMAIN_MAX; i++)
		audio_pdr_service_deregister(i);
}
module_exit(audio_pdr_late_exit);

MODULE_DESCRIPTION("PDR framework driver");
MODULE_LICENSE("GPL v2");
