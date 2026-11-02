// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/srcu.h>
#include <linux/usb/typec.h>
#include <linux/usb/ucsi_glink.h>
#include <linux/soc/qcom/fsa4480-i2c.h>
#include <linux/qti-regmap-debugfs.h>

#define FSA4480_I2C_NAME	"fsa4480-driver"

#define FSA4480_SWITCH_SETTINGS 0x04
#define FSA4480_SWITCH_CONTROL  0x05
#define FSA4480_SWITCH_STATUS1  0x07
#define FSA4480_SLOW_L          0x08
#define FSA4480_SLOW_R          0x09
#define FSA4480_SLOW_MIC        0x0A
#define FSA4480_SLOW_SENSE      0x0B
#define FSA4480_SLOW_GND        0x0C
#define FSA4480_DELAY_L_R       0x0D
#define FSA4480_DELAY_L_MIC     0x0E
#define FSA4480_DELAY_L_SENSE   0x0F
#define FSA4480_DELAY_L_AGND    0x10
#define FSA4480_RESET			0x21

/* DIO4485 REG */
#define DIO4485_DEVICE_ID 		0x00
#define DIO4485_FUNCTION_ENABLE 0x12
#define DIO4485_TIMING_DELAY_SETTING 0x21
#define DIO4485_POWER_UP_4E 	0x4E
#define DIO4485_POWER_UP_50 	0x50
#define DIO4485_POWER_UP_51 	0x51

struct fsa4480_priv {
	struct list_head registry_node;
	bool removing;
	spinlock_t work_lock;
	bool work_active;
	int last_error;
	struct regmap *regmap;
	struct device *dev;
	struct notifier_block ucsi_nb;
	atomic_t usbc_mode;
	struct work_struct usbc_analog_work;
	struct blocking_notifier_head fsa4480_notifier;
	struct mutex notification_lock;
};

struct fsa4480_reg_val {
	u16 reg;
	u8 val;
};

static const struct regmap_config fsa4480_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = DIO4485_POWER_UP_51,
};

static const struct fsa4480_reg_val fsa_reg_i2c_defaults[] = {
	{FSA4480_SLOW_L, 0x00},
	{FSA4480_SLOW_R, 0x00},
	{FSA4480_SLOW_MIC, 0x00},
	{FSA4480_SLOW_SENSE, 0x00},
	{FSA4480_SLOW_GND, 0x00},
	{FSA4480_DELAY_L_R, 0x00},
	{FSA4480_DELAY_L_MIC, 0x00},
	{FSA4480_DELAY_L_SENSE, 0x00},
	{FSA4480_DELAY_L_AGND, 0x09},
	{FSA4480_SWITCH_SETTINGS, 0x98},
};

static const struct fsa4480_reg_val dio_reg_i2c_defaults[] = {
	{ DIO4485_POWER_UP_4E, 0x8F },
	{ DIO4485_POWER_UP_4E, 0x5A },
	{ DIO4485_POWER_UP_51, 0x90 },
	{ DIO4485_POWER_UP_50, 0x45 },
	{ FSA4480_DELAY_L_SENSE, 0x00 },
	{ FSA4480_DELAY_L_AGND, 0x00 },
	{ FSA4480_DELAY_L_MIC, 0x0B },
	{ FSA4480_DELAY_L_R, 0x0F },
	{ DIO4485_TIMING_DELAY_SETTING, 0x0F },
	{ DIO4485_FUNCTION_ENABLE, 0x08 },
};

static LIST_HEAD(fsa4480_devices);
static DEFINE_MUTEX(fsa4480_registry_lock);
DEFINE_STATIC_SRCU(fsa4480_ops_srcu);

/* The caller holds fsa4480_ops_srcu until it stops using this instance. */
static struct fsa4480_priv *fsa4480_find(struct device_node *node)
{
	struct fsa4480_priv *priv, *found = NULL;

	mutex_lock(&fsa4480_registry_lock);
	list_for_each_entry(priv, &fsa4480_devices, registry_node) {
		if (priv->dev->of_node == node && !priv->removing) {
			found = priv;
			break;
		}
	}
	mutex_unlock(&fsa4480_registry_lock);
	return found;
}

/* notification_lock serializes the read/disable/control/enable sequence. */
static int fsa4480_usbc_update_settings(struct fsa4480_priv *fsa_priv,
		u32 switch_control, u32 switch_enable)
{
	u32 prev_control, prev_enable;
	int rc;

	rc = regmap_read(fsa_priv->regmap, FSA4480_SWITCH_CONTROL, &prev_control);
	if (rc)
		return rc;
	rc = regmap_read(fsa_priv->regmap, FSA4480_SWITCH_SETTINGS, &prev_enable);
	if (rc)
		return rc;
	if (prev_control == switch_control && prev_enable == switch_enable)
		return 0;
	rc = regmap_write(fsa_priv->regmap, FSA4480_SWITCH_SETTINGS, 0x80);
	if (rc)
		return rc;
	rc = regmap_write(fsa_priv->regmap, FSA4480_SWITCH_CONTROL, switch_control);
	if (rc)
		return rc;
	/* FSA4480 chip hardware requirement */
	usleep_range(50, 55);
	return regmap_write(fsa_priv->regmap, FSA4480_SWITCH_SETTINGS, switch_enable);
}

static int fsa4480_usbc_event_changed(struct notifier_block *nb,
				      unsigned long evt, void *ptr)
{
	struct fsa4480_priv *priv = container_of(nb, struct fsa4480_priv, ucsi_nb);
	enum typec_accessory acc;
	unsigned long flags;

	if (!ptr)
		return NOTIFY_BAD;
	acc = ((struct ucsi_glink_constat_info *)ptr)->acc;
	if (acc != TYPEC_ACCESSORY_AUDIO && acc != TYPEC_ACCESSORY_NONE)
		return NOTIFY_DONE;
	spin_lock_irqsave(&priv->work_lock, flags);
	if (READ_ONCE(priv->removing))
		goto unlock;
	if (atomic_read(&priv->usbc_mode) == acc && !priv->last_error)
		goto unlock;
	atomic_set(&priv->usbc_mode, acc);
	if (!priv->work_active) {
		priv->work_active = true;
		pm_stay_awake(priv->dev);
		queue_work(system_freezable_wq, &priv->usbc_analog_work);
	}
unlock:
	spin_unlock_irqrestore(&priv->work_lock, flags);
	return NOTIFY_OK;
}

static int fsa4480_usbc_analog_setup_switches(struct fsa4480_priv *priv)
{
	int mode = atomic_read(&priv->usbc_mode);
	int rc;

	if (READ_ONCE(priv->removing))
		return -ENODEV;
	/* Notifiers may configure the switch themselves: never call them while
	 * holding notification_lock. Preserve disconnect-before-disable ordering.
	 */
	if (mode == TYPEC_ACCESSORY_NONE)
		blocking_notifier_call_chain(&priv->fsa4480_notifier, mode, NULL);
	mutex_lock(&priv->notification_lock);
	if (READ_ONCE(priv->removing))
		rc = -ENODEV;
	else if (mode == TYPEC_ACCESSORY_AUDIO)
		rc = fsa4480_usbc_update_settings(priv, 0x00, 0x9F);
	else if (mode == TYPEC_ACCESSORY_NONE)
		rc = fsa4480_usbc_update_settings(priv, 0x18, 0x98);
	else
		rc = -EINVAL;
	mutex_unlock(&priv->notification_lock);
	if (!rc && mode == TYPEC_ACCESSORY_AUDIO)
		blocking_notifier_call_chain(&priv->fsa4480_notifier, mode, NULL);
	return rc;
}

/*
 * fsa4480_reg_notifier - register notifier block with fsa driver
 *
 * @nb - notifier block of fsa4480
 * @node - phandle node to fsa4480 device
 *
 * Returns 0 on success, or error code
 */
int fsa4480_reg_notifier(struct notifier_block *nb, struct device_node *node)
{
	struct fsa4480_priv *priv;
	int idx, rc;

	if (!nb || !node)
		return -EINVAL;
	idx = srcu_read_lock(&fsa4480_ops_srcu);
	priv = fsa4480_find(node);
	if (!priv) {
		rc = -EPROBE_DEFER;
		goto unlock;
	}
	rc = blocking_notifier_chain_register(&priv->fsa4480_notifier, nb);
	if (rc)
		goto unlock;
	if (atomic_read(&priv->usbc_mode) == TYPEC_ACCESSORY_AUDIO) {
		rc = fsa4480_usbc_analog_setup_switches(priv);
		if (rc)
			blocking_notifier_chain_unregister(&priv->fsa4480_notifier, nb);
	}
unlock:
	srcu_read_unlock(&fsa4480_ops_srcu, idx);
	return rc;
}
EXPORT_SYMBOL_GPL(fsa4480_reg_notifier);

/*
 * fsa4480_unreg_notifier - unregister notifier block with fsa driver
 *
 * @nb - notifier block of fsa4480
 * @node - phandle node to fsa4480 device
 *
 * Returns 0 on pass, or error code
 */
int fsa4480_unreg_notifier(struct notifier_block *nb, struct device_node *node)
{
	struct fsa4480_priv *priv;
	int idx, rc;

	if (!nb || !node)
		return -EINVAL;
	idx = srcu_read_lock(&fsa4480_ops_srcu);
	priv = fsa4480_find(node);
	if (!priv) {
		rc = -ENODEV;
		goto unlock;
	}
	rc = blocking_notifier_chain_unregister(&priv->fsa4480_notifier, nb);
	if (!rc) {
		mutex_lock(&priv->notification_lock);
		if (!priv->removing)
			rc = fsa4480_usbc_update_settings(priv, 0x18, 0x98);
		mutex_unlock(&priv->notification_lock);
	}
unlock:
	srcu_read_unlock(&fsa4480_ops_srcu, idx);
	return rc;
}
EXPORT_SYMBOL_GPL(fsa4480_unreg_notifier);

static int fsa4480_validate_display_port_settings(struct fsa4480_priv *fsa_priv)
{
	u32 switch_status = 0;

	int rc = regmap_read(fsa_priv->regmap, FSA4480_SWITCH_STATUS1, &switch_status);

	if (rc)
		return rc;

	if ((switch_status != 0x23) && (switch_status != 0x1C)) {
		pr_err("AUX SBU1/2 switch status is invalid = %u\n",
				switch_status);
		return -EIO;
	}

	return 0;
}
/*
 * fsa4480_switch_event - configure FSA switch position based on event
 *
 * @node - phandle node to fsa4480 device
 * @event - fsa_function enum
 *
 * Returns int on whether the switch happened or not
 */
int fsa4480_switch_event(struct device_node *node, enum fsa_function event)
{
	struct fsa4480_priv *priv;
	u32 control;
	int idx, rc;

	if (!node || event < 0 || event >= FSA_EVENT_MAX)
		return -EINVAL;
	idx = srcu_read_lock(&fsa4480_ops_srcu);
	priv = fsa4480_find(node);
	if (!priv) {
		rc = -ENODEV;
		goto unlock_srcu;
	}
	mutex_lock(&priv->notification_lock);
	if (READ_ONCE(priv->removing)) {
		rc = -ENODEV;
		goto unlock;
	}
	switch (event) {
	case FSA_MIC_GND_SWAP:
		rc = regmap_read(priv->regmap, FSA4480_SWITCH_CONTROL, &control);
		if (!rc)
			rc = fsa4480_usbc_update_settings(priv,
				(control & 0x07) == 0x07 ? 0x00 : 0x07, 0x9F);
		break;
	case FSA_USBC_ORIENTATION_CC1:
	case FSA_USBC_ORIENTATION_CC2:
		rc = fsa4480_usbc_update_settings(priv,
			event == FSA_USBC_ORIENTATION_CC1 ? 0x18 : 0x78, 0xF8);
		if (!rc)
			rc = fsa4480_validate_display_port_settings(priv);
		break;
	case FSA_USBC_DISPLAYPORT_DISCONNECTED:
		rc = fsa4480_usbc_update_settings(priv, 0x18, 0x98);
		break;
	case FSA_USBC_SBU_LPD_SENSE:
		rc = fsa4480_usbc_update_settings(priv, 0x19, 0xD9);
		break;
	case FSA_USBC_SBU_LPD_ISOLATE:
		rc = fsa4480_usbc_update_settings(priv, 0x78, 0xD9);
		break;
	default:
		rc = -EINVAL;
	}
unlock:
	mutex_unlock(&priv->notification_lock);
unlock_srcu:
	srcu_read_unlock(&fsa4480_ops_srcu, idx);
	return rc;
}
EXPORT_SYMBOL_GPL(fsa4480_switch_event);

static void fsa4480_usbc_analog_work_fn(struct work_struct *work)
{
	struct fsa4480_priv *priv = container_of(work,
			struct fsa4480_priv, usbc_analog_work);
	unsigned long flags;
	int mode, rc;

	for (;;) {
		mode = atomic_read(&priv->usbc_mode);
		rc = fsa4480_usbc_analog_setup_switches(priv);
		spin_lock_irqsave(&priv->work_lock, flags);
		priv->last_error = rc;
		if (READ_ONCE(priv->removing) || mode == atomic_read(&priv->usbc_mode)) {
			priv->work_active = false;
			pm_relax(priv->dev);
			spin_unlock_irqrestore(&priv->work_lock, flags);
			return;
		}
		spin_unlock_irqrestore(&priv->work_lock, flags);
	}
}

static int fsa4480_update_reg_defaults(struct regmap *regmap)
{
	const struct fsa4480_reg_val *table;
	u32 type;
	size_t count, i;
	int rc;

	rc = regmap_read(regmap, DIO4485_DEVICE_ID, &type);
	if (rc)
		return rc;
	if (type == 0xF6) {
		table = dio_reg_i2c_defaults;
		count = ARRAY_SIZE(dio_reg_i2c_defaults);
	} else {
		table = fsa_reg_i2c_defaults;
		count = ARRAY_SIZE(fsa_reg_i2c_defaults);
	}
	for (i = 0; i < count; i++) {
		rc = regmap_write(regmap, table[i].reg, table[i].val);
		if (rc)
			return rc;
	}
	return 0;
}

static int fsa4480_probe(struct i2c_client *i2c)
{
	struct fsa4480_priv *priv;
	int rc;

	priv = devm_kzalloc(&i2c->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = &i2c->dev;
	mutex_init(&priv->notification_lock);
	spin_lock_init(&priv->work_lock);
	INIT_LIST_HEAD(&priv->registry_node);
	INIT_WORK(&priv->usbc_analog_work, fsa4480_usbc_analog_work_fn);
	BLOCKING_INIT_NOTIFIER_HEAD(&priv->fsa4480_notifier);
	priv->regmap = devm_regmap_init_i2c(i2c, &fsa4480_regmap_config);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);
	rc = fsa4480_update_reg_defaults(priv->regmap);
	if (rc)
		return rc;
	rc = devm_regmap_qti_debugfs_register(priv->dev, priv->regmap);
	if (rc)
		return rc;
	rc = device_init_wakeup(priv->dev, true);
	if (rc)
		return rc;
	priv->ucsi_nb.notifier_call = fsa4480_usbc_event_changed;
	rc = register_ucsi_glink_notifier(&priv->ucsi_nb);
	if (rc) {
		WRITE_ONCE(priv->removing, true);
		cancel_work_sync(&priv->usbc_analog_work);
		pm_relax(priv->dev);
		device_init_wakeup(priv->dev, false);
		return rc;
	}
	i2c_set_clientdata(i2c, priv);
	mutex_lock(&fsa4480_registry_lock);
	list_add_tail(&priv->registry_node, &fsa4480_devices);
	mutex_unlock(&fsa4480_registry_lock);
	return 0;
}

static void fsa4480_remove(struct i2c_client *i2c)
{
	struct fsa4480_priv *priv = i2c_get_clientdata(i2c);
	unsigned long flags;

	mutex_lock(&fsa4480_registry_lock);
	WRITE_ONCE(priv->removing, true);
	list_del_init(&priv->registry_node);
	mutex_unlock(&fsa4480_registry_lock);
	unregister_ucsi_glink_notifier(&priv->ucsi_nb);
	cancel_work_sync(&priv->usbc_analog_work);
	synchronize_srcu(&fsa4480_ops_srcu);
	mutex_lock(&priv->notification_lock);
	fsa4480_usbc_update_settings(priv, 0x18, 0x98);
	mutex_unlock(&priv->notification_lock);
	spin_lock_irqsave(&priv->work_lock, flags);
	priv->work_active = false;
	pm_relax(priv->dev);
	spin_unlock_irqrestore(&priv->work_lock, flags);
	device_init_wakeup(priv->dev, false);
	i2c_set_clientdata(i2c, NULL);
}

static const struct of_device_id fsa4480_i2c_dt_match[] = {
	{
		.compatible = "qcom,fsa4480-i2c",
	},
	{}
};

static struct i2c_driver fsa4480_i2c_driver = {
	.driver = {
		.name = FSA4480_I2C_NAME,
		.of_match_table = fsa4480_i2c_dt_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = fsa4480_probe,
	.remove = fsa4480_remove,
};

static int __init fsa4480_init(void)
{
	int rc;

	rc = i2c_add_driver(&fsa4480_i2c_driver);
	if (rc)
		pr_err("fsa4480: Failed to register I2C driver: %d\n", rc);

	return rc;
}
module_init(fsa4480_init);

static void __exit fsa4480_exit(void)
{
	i2c_del_driver(&fsa4480_i2c_driver);
}
module_exit(fsa4480_exit);

MODULE_DESCRIPTION("FSA4480 I2C driver");
MODULE_LICENSE("GPL");
