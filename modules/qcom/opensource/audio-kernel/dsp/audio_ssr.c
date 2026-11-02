// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, 2020 The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/remoteproc.h>
#include <linux/remoteproc/qcom_rproc.h>
#include "audio_ssr.h"


/**
 * audio_ssr_register -
 *        register to SSR framework
 *
 * @domain_id: Domain ID to register with
 * @nb: notifier block
 *
 * Returns handle pointer on success or error PTR on failure
 */
void *audio_ssr_register(const char *domain_name, struct notifier_block *nb)
{
	if (!domain_name || !domain_name[0] || !nb || !nb->notifier_call) {
		pr_err("%s: Invalid domain name\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	return qcom_register_ssr_notifier(domain_name, nb);
}
EXPORT_SYMBOL(audio_ssr_register);

/**
 * audio_ssr_deregister -
 *        Deregister handle from SSR framework
 *
 * @handle: SSR handle
 * @nb: notifier block
 *
 * Returns 0 on success or error on failure
 */
int audio_ssr_deregister(void *handle, struct notifier_block *nb)
{
	if (IS_ERR_OR_NULL(handle) || !nb)
		return -EINVAL;
	return qcom_unregister_ssr_notifier(handle, nb);
}
EXPORT_SYMBOL(audio_ssr_deregister);

