// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2024, Xiaomi Corporation, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/usb/ucsi_glink.h>
#include <linux/usb/typec.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include "typec_analog_acc.h"

/* Preserve the native unsupported-accessory bit in the jack report. */
#define JACK_TYPEC_ANALOG_MASK (SND_JACK_VIDEOOUT | SND_JACK_HEADSET)

struct typec_analog_acc_priv {
	int current_jack_type;
	struct notifier_block ucsi_nb;
	struct snd_soc_jack typec_analog_jack;
	struct snd_soc_component *component;
	/* Serialize the cached accessory state and its jack reports. */
	struct mutex jack_lock;
};

static DEFINE_MUTEX(acc_lock);
static struct typec_analog_acc_priv *acc_priv;

static int typec_analog_acc_event_changed(struct notifier_block *nb,
					unsigned long event, void *ptr)
{
	struct typec_analog_acc_priv *priv;
	enum typec_accessory acc;

	if (!ptr)
		return NOTIFY_DONE;
	priv = container_of(nb, struct typec_analog_acc_priv, ucsi_nb);
	acc = ((struct ucsi_glink_constat_info *)ptr)->acc;
	if (acc != TYPEC_ACCESSORY_AUDIO && acc != TYPEC_ACCESSORY_NONE)
		return NOTIFY_DONE;

	mutex_lock(&priv->jack_lock);
	if (priv->current_jack_type != acc) {
		snd_soc_jack_report(&priv->typec_analog_jack,
				    acc == TYPEC_ACCESSORY_AUDIO ?
				    JACK_TYPEC_ANALOG_MASK : 0,
				    JACK_TYPEC_ANALOG_MASK);
		priv->current_jack_type = acc;
	}
	mutex_unlock(&priv->jack_lock);
	return NOTIFY_DONE;
}

int typec_analog_acc_init(struct snd_soc_component *component)
{
	struct typec_analog_acc_priv *priv;
	int ret;

	if (!component || !component->card || !component->card->snd_card)
		return -EINVAL;
	mutex_lock(&acc_lock);
	if (acc_priv) {
		ret = -EBUSY;
		goto unlock;
	}
	if (!try_module_get(THIS_MODULE)) {
		ret = -ENODEV;
		goto unlock;
	}
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto put_module;
	}
	priv->component = component;
	mutex_init(&priv->jack_lock);
	priv->ucsi_nb.notifier_call = typec_analog_acc_event_changed;
	ret = snd_soc_card_jack_new(component->card, "Typec_analog Jack",
				    JACK_TYPEC_ANALOG_MASK,
				    &priv->typec_analog_jack);
	if (ret)
		goto free_priv;
	/* Registration can deliver events immediately; the jack is ready. */
	ret = register_ucsi_glink_notifier(&priv->ucsi_nb);
	if (ret) {
		snd_device_free(component->card->snd_card, priv->typec_analog_jack.jack);
		goto free_priv;
	}
	acc_priv = priv;
	mutex_unlock(&acc_lock);
	return 0;

free_priv:
	kfree(priv);
put_module:
	module_put(THIS_MODULE);
unlock:
	mutex_unlock(&acc_lock);
	return ret;
}
EXPORT_SYMBOL(typec_analog_acc_init);

/* Call before the component/card is destroyed, outside notifier callbacks. */
void typec_analog_acc_exit(struct snd_soc_component *component)
{
	struct typec_analog_acc_priv *priv;

	mutex_lock(&acc_lock);
	priv = acc_priv;
	if (!priv || priv->component != component)
		goto unlock;
	unregister_ucsi_glink_notifier(&priv->ucsi_nb);
	snd_device_free(component->card->snd_card, priv->typec_analog_jack.jack);
	acc_priv = NULL;
	kfree(priv);
	module_put(THIS_MODULE);
unlock:
	mutex_unlock(&acc_lock);
}
EXPORT_SYMBOL(typec_analog_acc_exit);

static int __init analog_acc_init(void)
{
	return 0;
}

static void __exit analog_acc_exit(void)
{
	/* Every live notifier holds a module reference until component cleanup. */
}

module_init(analog_acc_init);
module_exit(analog_acc_exit);

MODULE_DESCRIPTION("type analog accessory module");
MODULE_LICENSE("GPL v2");
