// SPDX-License-Identifier: GPL-2.0
/*
 *mca_strategy_fg_class.c
 *
 * mca fuelgauge strategy class driver
 *
 * Copyright (c) 2023-2023 Xiaomi Technologies Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/srcu.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <mca/strategy/strategy_fg_class.h>

static struct strategy_fg_class_info __rcu *g_stg_fg_class_info;
static DEFINE_MUTEX(fg_ops_lock);
DEFINE_STATIC_SRCU(fg_ops_srcu);

#define is_invalid_ops(name) (!entry || !entry->ops || !(entry->ops->name))
#define strategy_fg_class_ops_no_para(name) (entry->ops->name(entry->data))
#define strategy_fg_class_ops_with_one_para(name, para1) \
	(entry->ops->name(entry->data, para1))

int strategy_class_fg_ops_register(void *data,
				   struct strategy_fg_class_ops *ops)
{
	struct strategy_fg_class_info *entry;

	if (!ops || !data)
		return -EINVAL;
	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->ops = ops;
	entry->data = data;
	mutex_lock(&fg_ops_lock);
	if (rcu_access_pointer(g_stg_fg_class_info)) {
		mutex_unlock(&fg_ops_lock);
		kfree(entry);
		return -EEXIST;
	}
	rcu_assign_pointer(g_stg_fg_class_info, entry);
	mutex_unlock(&fg_ops_lock);
	return 0;
}
EXPORT_SYMBOL(strategy_class_fg_ops_register);

int strategy_class_fg_ops_is_init_ok(void)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (is_invalid_ops(strategy_fg_is_init_ok))
		return -ENODEV;

	return strategy_fg_class_ops_no_para(strategy_fg_is_init_ok);
}
EXPORT_SYMBOL(strategy_class_fg_ops_is_init_ok);

int strategy_class_fg_ops_get_rsoc(int *rsoc)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!rsoc)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_rsoc))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_rsoc, rsoc);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_rsoc);

int strategy_class_fg_ops_get_soc(void)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (is_invalid_ops(strategy_fg_get_soc))
		return -ENODEV;

	return strategy_fg_class_ops_no_para(strategy_fg_get_soc);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_soc);

int strategy_class_fg_ops_get_temperature(int *temp)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!temp)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_temp))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_temp, temp);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_temperature);

int strategy_class_fg_ops_get_current(int *curr)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!curr)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_current))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_current,
						   curr);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_current);

int strategy_class_fg_ops_get_voltage(int *volt)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!volt)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_voltage))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_voltage,
						   volt);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_voltage);

int strategy_class_fg_ops_get_cyclecount(int *cycle)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!cycle)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_cycle))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_cycle,
						   cycle);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_cyclecount);

int strategy_class_fg_get_voltage_mean(int *vol_mean)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!vol_mean)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_voltage_mean))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_voltage_mean,
						   vol_mean);
}
EXPORT_SYMBOL(strategy_class_fg_get_voltage_mean);

int strategy_class_fg_ops_get_soc_decimal(int *soc_decimal, int *rate)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!soc_decimal || !rate)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_soc_decimal_info))
		return -ENODEV;

	return entry->ops->strategy_fg_get_soc_decimal_info(entry->data,
							    soc_decimal, rate);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_soc_decimal);

bool strategy_class_fg_ops_get_charging_done(void)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (is_invalid_ops(strategy_fg_get_charging_done))
		return false;

	return strategy_fg_class_ops_no_para(strategy_fg_get_charging_done);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_charging_done);

int strategy_class_fg_ops_set_charging_done(bool charging_done)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (is_invalid_ops(strategy_fg_set_charging_done))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(
		strategy_fg_set_charging_done, charging_done);
}
EXPORT_SYMBOL(strategy_class_fg_ops_set_charging_done);

int strategy_class_fg_get_model_name(const char **model_name)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!model_name)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_model_name))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_model_name,
						   model_name);
}
EXPORT_SYMBOL(strategy_class_fg_get_model_name);

int strategy_class_fg_set_fastcharge(bool en)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (is_invalid_ops(strategy_fg_set_fastcharge))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_set_fastcharge,
						   en);
}
EXPORT_SYMBOL(strategy_class_fg_set_fastcharge);

int strategy_class_fg_get_fastcharge(void)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (is_invalid_ops(strategy_fg_get_fastcharge))
		return -ENODEV;

	return strategy_fg_class_ops_no_para(strategy_fg_get_fastcharge);
}
EXPORT_SYMBOL(strategy_class_fg_get_fastcharge);

int strategy_class_fg_get_authentic(bool *authentic)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!authentic)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_authentic))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_authentic,
						   authentic);
}
EXPORT_SYMBOL(strategy_class_fg_get_authentic);

int strategy_class_fg_get_dc(int *dc)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!dc)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_dc))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_dc, dc);
}
EXPORT_SYMBOL(strategy_class_fg_get_dc);

int strategy_class_fg_get_rm(int *rm)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!rm)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_rm))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_rm, rm);
}
EXPORT_SYMBOL(strategy_class_fg_get_rm);

int strategy_class_fg_get_fcc(int *fcc)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!fcc)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_fcc))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_fcc, fcc);
}
EXPORT_SYMBOL(strategy_class_fg_get_fcc);

int strategy_class_fg_is_chip_ok(void)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (is_invalid_ops(strategy_fg_is_chip_ok))
		return -ENODEV;

	return strategy_fg_class_ops_no_para(strategy_fg_is_chip_ok);
}
EXPORT_SYMBOL(strategy_class_fg_is_chip_ok);

int strategy_class_fg_get_health(int *health)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!health)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_health))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_health,
						   health);
}
EXPORT_SYMBOL(strategy_class_fg_get_health);

int strategy_class_fg_get_first_termination(int *value)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!value)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_first_termination))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(
		strategy_fg_get_first_termination, value);
}
EXPORT_SYMBOL(strategy_class_fg_get_first_termination);

int strategy_class_fg_get_pack_vendor_id(int *vendor_id)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!vendor_id)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_pack_vendor_id))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(
		strategy_fg_get_pack_vendor_id, vendor_id);
}
EXPORT_SYMBOL(strategy_class_fg_get_pack_vendor_id);

int strategy_class_fg_dual_is_chip_ok(int index)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (is_invalid_ops(strategy_fg_dual_is_chip_ok))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_dual_is_chip_ok,
						   index);
}
EXPORT_SYMBOL(strategy_class_fg_dual_is_chip_ok);

int strategy_class_fg_ops_get_thermal_temperature(int *temp)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!temp)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_thermal_temperature))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(
		strategy_fg_get_thermal_temperature, temp);
}
EXPORT_SYMBOL(strategy_class_fg_ops_get_thermal_temperature);

int strategy_class_fg_get_soh(int *soh)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!soh)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_soh))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(strategy_fg_get_soh, soh);
}
EXPORT_SYMBOL(strategy_class_fg_get_soh);

int strategy_class_fg_get_temp_offset_flag(int *flag)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);

	if (!flag)
		return -EINVAL;

	if (is_invalid_ops(strategy_fg_get_temp_offset_flag))
		return -ENODEV;

	return strategy_fg_class_ops_with_one_para(
		strategy_fg_get_temp_offset_flag, flag);
}
EXPORT_SYMBOL(strategy_class_fg_get_temp_offset_flag);

void strategy_class_fg_ops_unregister(void *data)
{
	struct strategy_fg_class_info *entry;

	mutex_lock(&fg_ops_lock);
	entry = rcu_dereference_protected(g_stg_fg_class_info,
					  lockdep_is_held(&fg_ops_lock));
	if (!entry || entry->data != data) {
		mutex_unlock(&fg_ops_lock);
		return;
	}
	rcu_assign_pointer(g_stg_fg_class_info, NULL);
	mutex_unlock(&fg_ops_lock);
	synchronize_srcu(&fg_ops_srcu);
	kfree(entry);
}
EXPORT_SYMBOL(strategy_class_fg_ops_unregister);

int strategy_class_fg_copy_model_name(char *buf, size_t len)
{
	guard(srcu)(&fg_ops_srcu);
	struct strategy_fg_class_info *entry =
		srcu_dereference(g_stg_fg_class_info, &fg_ops_srcu);
	const char *name;
	int ret;

	if (!buf || !len)
		return -EINVAL;
	if (is_invalid_ops(strategy_fg_get_model_name))
		return -ENODEV;
	ret = entry->ops->strategy_fg_get_model_name(entry->data, &name);
	if (ret)
		return ret;
	if (!name)
		return -ENODATA;
	ret = strscpy(buf, name, len);
	return ret < 0 ? ret : 0;
}
EXPORT_SYMBOL(strategy_class_fg_copy_model_name);

static struct platform_driver strategy_fg_class_driver = {
	.driver	= {
		.name = "strategy_fg_class",
	},
};

module_platform_driver(strategy_fg_class_driver);
MODULE_DESCRIPTION("platform fg class");
MODULE_AUTHOR("liyuze1@xiaomi.com");
MODULE_LICENSE("GPL v2");
