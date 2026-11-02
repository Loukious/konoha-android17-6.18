// SPDX-License-Identifier: GPL-2.0
/*
 *mca_strategy_class.c
 *
 * mca strategy class driver
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
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/ctype.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <mca/strategy/strategy_class.h>

struct mca_strategy_func_data {
	mca_strategy_func func;
	mca_strategy_get_status get_func;
	mca_strategy_set_config set_config;
	void *data;
};

static struct mca_strategy_func_data __rcu *g_mca_stg_func[STRATEGY_FUNC_TYPE_MAX];
static DEFINE_MUTEX(mca_ops_lock);
DEFINE_STATIC_SRCU(mca_ops_srcu);

static struct mca_strategy_func_data *mca_strategy_get_func_data(unsigned int type)
{
	if (type >= STRATEGY_FUNC_TYPE_MAX)
		return NULL;

	return srcu_dereference(g_mca_stg_func[type], &mca_ops_srcu);
}

int mca_strategy_func_get_status(int type, int status, void *value)
{
	guard(srcu)(&mca_ops_srcu);

	struct mca_strategy_func_data *temp_data = mca_strategy_get_func_data(type);

	if (!temp_data || !temp_data->get_func)
		return -1;

	return temp_data->get_func(status, value, temp_data->data);
}
EXPORT_SYMBOL(mca_strategy_func_get_status);

int mca_strategy_func_process(unsigned int type, int event, int value)
{
	guard(srcu)(&mca_ops_srcu);

	struct mca_strategy_func_data *temp_data = mca_strategy_get_func_data(type);

	if (!temp_data || !temp_data->func)
		return -1;

	return temp_data->func(event, value, temp_data->data);
}
EXPORT_SYMBOL(mca_strategy_func_process);

int mca_strategy_func_set_config(int type, int conifg, int value)
{
	guard(srcu)(&mca_ops_srcu);

	struct mca_strategy_func_data *temp_data = mca_strategy_get_func_data(type);

	if (!temp_data || !temp_data->set_config)
		return -1;

	return temp_data->set_config(conifg, value, temp_data->data);
}
EXPORT_SYMBOL(mca_strategy_func_set_config);

int mca_strategy_ops_register(unsigned int type, mca_strategy_func func,
	mca_strategy_get_status get_func, mca_strategy_set_config set_config, void *data)
{
	struct mca_strategy_func_data *entry;

	if (type >= STRATEGY_FUNC_TYPE_MAX || !data || (!func && !get_func && !set_config))
		return -EINVAL;
	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	*entry = (struct mca_strategy_func_data) {
		.func = func, .get_func = get_func, .set_config = set_config, .data = data,
	};
	mutex_lock(&mca_ops_lock);
	if (rcu_access_pointer(g_mca_stg_func[type])) {
		mutex_unlock(&mca_ops_lock);
		kfree(entry);
		return -EEXIST;
	}
	rcu_assign_pointer(g_mca_stg_func[type], entry);
	mutex_unlock(&mca_ops_lock);
	return 0;
}
EXPORT_SYMBOL(mca_strategy_ops_register);

void mca_strategy_ops_unregister(unsigned int type, void *data)
{
	struct mca_strategy_func_data *entry;

	if (type >= STRATEGY_FUNC_TYPE_MAX)
		return;
	mutex_lock(&mca_ops_lock);
	entry = rcu_dereference_protected(g_mca_stg_func[type], lockdep_is_held(&mca_ops_lock));
	if (!entry || entry->data != data) {
		mutex_unlock(&mca_ops_lock);
		return;
	}
	rcu_assign_pointer(g_mca_stg_func[type], NULL);
	mutex_unlock(&mca_ops_lock);
	synchronize_srcu(&mca_ops_srcu);
	kfree(entry);
}
EXPORT_SYMBOL(mca_strategy_ops_unregister);


static struct platform_driver mca_strategy_class_driver = {
	.driver	= {
		.name = "mca_strategy_class",
	},
};


module_platform_driver(mca_strategy_class_driver);


MODULE_DESCRIPTION("mca strategy class");
MODULE_AUTHOR("getian@xiaomi.com");
MODULE_LICENSE("GPL v2");

