// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2011-2017, The Linux Foundation
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/idr.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/slimbus.h>
#include <linux/srcu.h>
#include "slimbus.h"

DEFINE_STATIC_SRCU(slim_creation_srcu);
DEFINE_STATIC_SRCU(slim_status_srcu);

static DEFINE_IDA(ctrl_ida);

static const struct slim_device_id *slim_match(const struct slim_device_id *id,
					       const struct slim_device *sbdev)
{
	if (!id)
		return NULL;
	while (id->manf_id != 0 || id->prod_code != 0) {
		if (id->manf_id == sbdev->e_addr.manf_id &&
		    id->prod_code == sbdev->e_addr.prod_code &&
		    id->dev_index == sbdev->e_addr.dev_index &&
		    id->instance == sbdev->e_addr.instance)
			return id;
		id++;
	}
	return NULL;
}

static int slim_device_match(struct device *dev, const struct device_driver *drv)
{
	struct slim_device *sbdev = to_slim_device(dev);
	const struct slim_driver *sbdrv = to_slim_driver(drv);

	/* Attempt an OF style match first */
	if (of_driver_match_device(dev, drv))
		return 1;

	return !!slim_match(sbdrv->id_table, sbdev);
}

static void slim_device_update_status(struct slim_device *sbdev,
				      enum slim_device_status status)
{
	struct slim_driver *driver;
	int idx;

	if (READ_ONCE(sbdev->status) == status)
		return;
	WRITE_ONCE(sbdev->status, status);
	idx = srcu_read_lock(&slim_status_srcu);
	driver = srcu_dereference(sbdev->status_driver, &slim_status_srcu);
	if (driver && driver->device_status)
		driver->device_status(sbdev, READ_ONCE(sbdev->status));
	srcu_read_unlock(&slim_status_srcu, idx);
}

static int slim_device_probe(struct device *dev)
{
	struct slim_device	*sbdev = to_slim_device(dev);
	struct slim_driver	*sbdrv = to_slim_driver(dev->driver);
	int ret;

	ret = sbdrv->probe(sbdev);
	if (ret)
		return ret;

	/* try getting the logical address after probe */
	ret = slim_get_logical_addr(sbdev);
	if (!ret) {
		rcu_assign_pointer(sbdev->status_driver, sbdrv);
		if (sbdrv->device_status)
			sbdrv->device_status(sbdev, READ_ONCE(sbdev->status));
	} else {
		dev_err(&sbdev->dev, "Failed to get logical address\n");
		if (sbdrv->remove)
			sbdrv->remove(sbdev);
		ret = -EPROBE_DEFER;
	}

	return ret;
}

static void slim_device_remove(struct device *dev)
{
	struct slim_device *sbdev = to_slim_device(dev);
	struct slim_driver *sbdrv;

	RCU_INIT_POINTER(sbdev->status_driver, NULL);
	synchronize_srcu(&slim_status_srcu);
	if (dev->driver) {
		sbdrv = to_slim_driver(dev->driver);
		if (sbdrv->remove)
			sbdrv->remove(sbdev);
	}
}

static int slim_device_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct slim_device *sbdev = to_slim_device(dev);

	return add_uevent_var(env, "MODALIAS=slim:%s", dev_name(&sbdev->dev));
}

struct bus_type slimbus_bus = {
	.name		= "slimbus",
	.match		= slim_device_match,
	.probe		= slim_device_probe,
	.remove		= slim_device_remove,
	.uevent		= slim_device_uevent,
};
EXPORT_SYMBOL_GPL(slimbus_bus);

/*
 * __slim_driver_register() - Client driver registration with SLIMbus
 *
 * @drv:Client driver to be associated with client-device.
 * @owner: owning module/driver
 *
 * This API will register the client driver with the SLIMbus
 * It is called from the driver's module-init function.
 */
int __slim_driver_register(struct slim_driver *drv, struct module *owner)
{
	/* ID table and probe are mandatory */
	if (!(drv->driver.of_match_table || drv->id_table) || !drv->probe)
		return -EINVAL;

	drv->driver.bus = &slimbus_bus;
	drv->driver.owner = owner;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(__slim_driver_register);

/*
 * slim_driver_unregister() - Undo effect of slim_driver_register
 *
 * @drv: Client driver to be unregistered
 */
void slim_driver_unregister(struct slim_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(slim_driver_unregister);

static void slim_dev_release(struct device *dev)
{
	struct slim_device *sbdev = to_slim_device(dev);
	struct slim_controller *ctrl = sbdev->ctrl;

	of_node_put(sbdev->dev.of_node);
	kfree(sbdev);
	if (refcount_dec_and_test(&ctrl->device_refs))
		complete(&ctrl->devices_gone);
}

static int slim_add_device(struct slim_controller *ctrl,
			   struct slim_device *sbdev,
			   struct device_node *node)
{
	int ret;

	refcount_inc(&ctrl->device_refs);
	device_initialize(&sbdev->dev);
	sbdev->dev.bus = &slimbus_bus;
	sbdev->dev.parent = ctrl->dev;
	sbdev->dev.release = slim_dev_release;
	sbdev->dev.driver = NULL;
	sbdev->ctrl = ctrl;
	INIT_LIST_HEAD(&sbdev->stream_list);
	spin_lock_init(&sbdev->stream_list_lock);
	sbdev->dev.of_node = of_node_get(node);
	sbdev->dev.fwnode = of_fwnode_handle(node);

	ret = dev_set_name(&sbdev->dev, "%x:%x:%x:%x%s",
			   sbdev->e_addr.manf_id,
				  sbdev->e_addr.prod_code,
				  sbdev->e_addr.dev_index,
				  sbdev->e_addr.instance, EXTRA_CHAR);

	if (ret)
		return ret;
	return device_add(&sbdev->dev);
}

static struct slim_device *slim_alloc_device(struct slim_controller *ctrl,
					     struct slim_eaddr *eaddr,
					     struct device_node *node)
{
	struct slim_device *sbdev;
	int ret;

	sbdev = kzalloc(sizeof(*sbdev), GFP_KERNEL);
	if (!sbdev)
		return NULL;

	sbdev->e_addr = *eaddr;
	ret = slim_add_device(ctrl, sbdev, node);
	if (ret) {
		put_device(&sbdev->dev);
		return NULL;
	}

	return sbdev;
}

static void of_register_slim_devices(struct slim_controller *ctrl)
{
	struct device *dev = ctrl->dev;
	struct device_node *node;

	if (!ctrl->dev->of_node)
		return;

	for_each_available_child_of_node(ctrl->dev->of_node, node) {
		struct slim_device *sbdev;
		struct slim_eaddr e_addr;
		const char *compat = NULL;
		int reg[2], ret;
		int manf_id, prod_code;

		compat = of_get_property(node, "compatible", NULL);
		if (!compat)
			continue;

		ret = sscanf(compat, "slim%x,%x", &manf_id, &prod_code);
		if (ret != 2) {
			dev_err(dev, "Manf ID & Product code not found %s\n",
				compat);
			continue;
		}

		ret = of_property_read_u32_array(node, "reg", reg, 2);
		if (ret) {
			dev_err(dev, "Device and Instance id not found:%d\n",
				ret);
			continue;
		}

		if (reg[0] > U8_MAX || reg[1] > U8_MAX ||
		    manf_id > U16_MAX || prod_code > U16_MAX)
			continue;

		e_addr.dev_index = reg[0];
		e_addr.instance = reg[1];
		e_addr.manf_id = manf_id;
		e_addr.prod_code = prod_code;

		sbdev = slim_alloc_device(ctrl, &e_addr, node);
		if (!sbdev)
			continue;
	}
}

/*
 * slim_register_controller() - Controller bring-up and registration.
 *
 * @ctrl: Controller to be registered.
 *
 * A controller is registered with the framework using this API.
 * If devices on a controller were registered before controller,
 * this will make sure that they get probed when controller is up
 */
int slim_register_controller(struct slim_controller *ctrl)
{
	int id;

	id = ida_alloc(&ctrl_ida, GFP_KERNEL);
	if (id < 0)
		return id;

	ctrl->id = id;
	mutex_init(&ctrl->devices_lock);
	refcount_set(&ctrl->device_refs, 1);
	init_completion(&ctrl->devices_gone);
	WRITE_ONCE(ctrl->removing, false);

	if (!ctrl->min_cg)
		ctrl->min_cg = SLIM_MIN_CLK_GEAR;
	if (!ctrl->max_cg)
		ctrl->max_cg = SLIM_MAX_CLK_GEAR;

	ida_init(&ctrl->laddr_ida);
	idr_init(&ctrl->tid_idr);
	mutex_init(&ctrl->lock);
	mutex_init(&ctrl->stream_lock);
	mutex_init(&ctrl->sched.m_reconf);
	init_completion(&ctrl->sched.pause_comp);
	spin_lock_init(&ctrl->txn_lock);

	dev_dbg(ctrl->dev, "Bus [%s] registered:dev:%p\n",
		ctrl->name, ctrl->dev);

	of_register_slim_devices(ctrl);

	return 0;
}
EXPORT_SYMBOL_GPL(slim_register_controller);

/* slim_remove_device: Remove the effect of slim_add_device() */
static void slim_remove_device(struct slim_device *sbdev)
{
	device_unregister(&sbdev->dev);
}

static int slim_ctrl_remove_device(struct device *dev, void *null)
{
	slim_remove_device(to_slim_device(dev));
	return 0;
}

/**
 * slim_unregister_controller() - Controller tear-down.
 *
 * @ctrl: Controller to tear-down.
 */
int slim_unregister_controller(struct slim_controller *ctrl)
{
	int tid;
	unsigned long flags;

	mutex_lock(&ctrl->devices_lock);
	spin_lock_irqsave(&ctrl->txn_lock, flags);
	WRITE_ONCE(ctrl->removing, true);
	spin_unlock_irqrestore(&ctrl->txn_lock, flags);
	mutex_unlock(&ctrl->devices_lock);
	synchronize_srcu(&slim_creation_srcu);
	/* The controller must stop its notification and DMA producers first. */
	for (tid = 1; tid < SLIM_MAX_TIDS; tid++)
		slim_msg_response(ctrl, NULL, tid, 0);
	device_for_each_child(ctrl->dev, NULL, slim_ctrl_remove_device);
	if (refcount_dec_and_test(&ctrl->device_refs))
		complete(&ctrl->devices_gone);
	/* Lookup and stream users must drop their retained device references. */
	wait_for_completion(&ctrl->devices_gone);
	idr_destroy(&ctrl->tid_idr);
	ida_destroy(&ctrl->laddr_ida);
	ida_free(&ctrl_ida, ctrl->id);
	return 0;
}
EXPORT_SYMBOL_GPL(slim_unregister_controller);

/**
 * slim_report_absent() - Controller calls this function when a device
 *	reports absent, OR when the device cannot be communicated with
 *
 * @sbdev: Device that cannot be reached, or sent report absent
 */
void slim_report_absent(struct slim_device *sbdev)
{
	struct slim_controller *ctrl = sbdev->ctrl;

	if (!ctrl)
		return;

	/* invalidate logical addresses */
	mutex_lock(&ctrl->lock);
	if (!sbdev->is_laddr_valid) {
		mutex_unlock(&ctrl->lock);
		return;
	}
	sbdev->is_laddr_valid = false;
	if (!ctrl->get_laddr)
		ida_free(&ctrl->laddr_ida, sbdev->laddr);
	mutex_unlock(&ctrl->lock);
	slim_device_update_status(sbdev, SLIM_DEVICE_STATUS_DOWN);
}
EXPORT_SYMBOL_GPL(slim_report_absent);

static bool slim_eaddr_equal(const struct slim_eaddr *a, const struct slim_eaddr *b)
{
	return (a->manf_id == b->manf_id &&
		a->prod_code == b->prod_code &&
		a->dev_index == b->dev_index &&
		a->instance == b->instance);
}

static int slim_match_dev(struct device *dev, const void *data)
{
	const struct slim_eaddr *e_addr = data;
	struct slim_device *sbdev = to_slim_device(dev);

	return slim_eaddr_equal(&sbdev->e_addr, e_addr);
}

static struct slim_device *find_slim_device(struct slim_controller *ctrl,
					    struct slim_eaddr *eaddr)
{
	struct slim_device *sbdev;
	struct device *dev;

	dev = device_find_child(ctrl->dev, eaddr, slim_match_dev);
	if (dev) {
		sbdev = to_slim_device(dev);
		return sbdev;
	}

	return NULL;
}

/**
 * slim_get_device() - get handle to a device.
 *
 * @ctrl: Controller on which this device will be added/queried
 * @e_addr: Enumeration address of the device to be queried
 *
 * Return: pointer to a device if it has already reported. Creates a new
 * device and returns pointer to it if the device has not yet enumerated.
 */
struct slim_device *slim_get_device(struct slim_controller *ctrl,
				    struct slim_eaddr *e_addr)
{
	struct slim_device *sbdev;
	int idx = srcu_read_lock(&slim_creation_srcu);

	mutex_lock(&ctrl->devices_lock);
	if (ctrl->removing) {
		sbdev = ERR_PTR(-ESHUTDOWN);
		mutex_unlock(&ctrl->devices_lock);
		goto out;
	}
	sbdev = find_slim_device(ctrl, e_addr);
	mutex_unlock(&ctrl->devices_lock);
	if (!sbdev) {
		/* device_add can synchronously probe and recursively look up peers. */
		sbdev = slim_alloc_device(ctrl, e_addr, NULL);
		if (sbdev)
			get_device(&sbdev->dev);
		else
			sbdev = ERR_PTR(-ENOMEM);
	}
out:
	srcu_read_unlock(&slim_creation_srcu, idx);
	return sbdev;
}
EXPORT_SYMBOL_GPL(slim_get_device);

static int of_slim_match_dev(struct device *dev, const void *data)
{
	const struct device_node *np = data;
	struct slim_device *sbdev = to_slim_device(dev);

	return (sbdev->dev.of_node == np);
}

static struct slim_device *of_find_slim_device(struct slim_controller *ctrl,
					       struct device_node *np)
{
	struct slim_device *sbdev;
	struct device *dev;

	dev = device_find_child(ctrl->dev, np, of_slim_match_dev);
	if (dev) {
		sbdev = to_slim_device(dev);
		return sbdev;
	}

	return NULL;
}

/**
 * of_slim_get_device() - get handle to a device using dt node.
 *
 * @ctrl: Controller on which this device will be added/queried
 * @np: node pointer to device
 *
 * Return: pointer to a device if it has already reported. Creates a new
 * device and returns pointer to it if the device has not yet enumerated.
 */
struct slim_device *of_slim_get_device(struct slim_controller *ctrl,
				       struct device_node *np)
{
	struct slim_device *sbdev = NULL;

	mutex_lock(&ctrl->devices_lock);
	if (!ctrl->removing)
		sbdev = of_find_slim_device(ctrl, np);
	mutex_unlock(&ctrl->devices_lock);
	return sbdev;
}
EXPORT_SYMBOL_GPL(of_slim_get_device);

static int slim_device_alloc_laddr(struct slim_device *sbdev,
				   bool report_present)
{
	struct slim_controller *ctrl;
	u8 laddr;
	int ret;

	ctrl = sbdev->ctrl;
	if (!ctrl) {
		pr_err("%s: slim_controller is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&ctrl->lock);
	if (sbdev->is_laddr_valid) {
		mutex_unlock(&ctrl->lock);
		return 0;
	}
	if (ctrl->get_laddr) {
		ret = ctrl->get_laddr(ctrl, &sbdev->e_addr, &laddr);
		if (ret < 0)
			goto err;
	} else if (report_present) {
		ret = ida_alloc_max(&ctrl->laddr_ida,
				    SLIM_LA_MANAGER - 1, GFP_KERNEL);
		if (ret < 0)
			goto err;

		laddr = ret;
	} else {
		ret = -EINVAL;
		goto err;
	}

	if (ctrl->set_laddr) {
		ret = ctrl->set_laddr(ctrl, &sbdev->e_addr, laddr);
		if (ret) {
			if (!ctrl->get_laddr)
				ida_free(&ctrl->laddr_ida, laddr);
			goto err;
		}
	}

	sbdev->laddr = laddr;
	sbdev->is_laddr_valid = true;
	mutex_unlock(&ctrl->lock);

	slim_device_update_status(sbdev, SLIM_DEVICE_STATUS_UP);

	dev_dbg(ctrl->dev, "setting slimbus l-addr:%x, ea:%x,%x,%x,%x\n",
		laddr, sbdev->e_addr.manf_id, sbdev->e_addr.prod_code,
		sbdev->e_addr.dev_index, sbdev->e_addr.instance);

	return 0;

err:
	mutex_unlock(&ctrl->lock);
	return ret;

}

/**
 * slim_device_report_present() - Report enumerated device.
 *
 * @ctrl: Controller with which device is enumerated.
 * @e_addr: Enumeration address of the device.
 * @laddr: Return logical address (if valid flag is false)
 *
 * Called by controller in response to REPORT_PRESENT. Framework will assign
 * a logical address to this enumeration address.
 * Function returns -EXFULL to indicate that all logical addresses are already
 * taken.
 */
int slim_device_report_present(struct slim_controller *ctrl,
			       struct slim_eaddr *e_addr, u8 *laddr)
{
	struct slim_device *sbdev;
	int ret;

	ret = pm_runtime_get_sync(ctrl->dev);
	if (ret < 0) {
		dev_err(ctrl->dev, "slim %s: PM get_sync failed ret :%d\n",
			__func__, ret);
		pm_runtime_put_noidle(ctrl->dev);
		/* Set device in suspended since resume failed */
		pm_runtime_set_suspended(ctrl->dev);
		return ret;
	}

	if (ctrl->sched.clk_state != SLIM_CLK_ACTIVE) {
		dev_err(ctrl->dev, "slim ctrl not active,state:%d, ret:%d\n",
				    ctrl->sched.clk_state, ret);
		ret = -EHOSTDOWN;
		goto out_put_rpm;
	}

	sbdev = slim_get_device(ctrl, e_addr);
	if (IS_ERR(sbdev)) {
		ret = -ENODEV;
		goto out_put_rpm;
	}

	if (sbdev->is_laddr_valid) {
		*laddr = sbdev->laddr;
		ret = 0;
	} else {
		ret = slim_device_alloc_laddr(sbdev, true);
		if (!ret)
			*laddr = sbdev->laddr;
	}

	put_device(&sbdev->dev);
out_put_rpm:
	pm_runtime_mark_last_busy(ctrl->dev);
	pm_runtime_put_autosuspend(ctrl->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(slim_device_report_present);

/**
 * slim_get_logical_addr() - get/allocate logical address of a SLIMbus device.
 *
 * @sbdev: client handle requesting the address.
 *
 * Return: zero if a logical address is valid or a new logical address
 * has been assigned. error code in case of error.
 */
int slim_get_logical_addr(struct slim_device *sbdev)
{
	if (READ_ONCE(sbdev->ctrl->removing))
		return -ESHUTDOWN;
	if (!sbdev->is_laddr_valid)
		return slim_device_alloc_laddr(sbdev, false);

	return 0;
}
EXPORT_SYMBOL_GPL(slim_get_logical_addr);

static void __exit slimbus_exit(void)
{
	bus_unregister(&slimbus_bus);
}
module_exit(slimbus_exit);

static int __init slimbus_init(void)
{
	return bus_register(&slimbus_bus);
}
postcore_initcall(slimbus_init);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("SLIMbus core");
