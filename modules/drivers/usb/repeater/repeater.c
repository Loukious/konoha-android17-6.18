// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/usb/repeater.h>

static LIST_HEAD(repeater_list);
static DEFINE_SPINLOCK(repeater_lock);

struct usb_repeater_ref {
	struct device *dev;
	struct module *owner;
};

void usb_put_repeater(struct usb_repeater *r)
{
	if (r) {
		struct module *owner = r->dev->driver->owner;

		put_device(r->dev);
		module_put(owner);
	}
}
EXPORT_SYMBOL_GPL(usb_put_repeater);

static struct usb_repeater *of_usb_find_repeater(struct device_node *node)
{
	struct usb_repeater *r;

	if (!of_device_is_available(node))
		return ERR_PTR(-ENODEV);

	list_for_each_entry(r, &repeater_list, head) {
		if (node != r->dev->of_node)
			continue;
		return r;
	}

	return ERR_PTR(-EPROBE_DEFER);
}

static void devm_usb_repeater_release_by_node(struct device *dev, void *res)
{
	struct usb_repeater_ref *ref = res;

	put_device(ref->dev);
	module_put(ref->owner);
}

struct usb_repeater *devm_usb_get_repeater_by_node(struct device *dev,
			struct device_node *node)
{
	struct usb_repeater *r = ERR_PTR(-ENOMEM);
	struct usb_repeater_ref *r_devm;
	struct device_link *link;
	unsigned long flags;
	int ret;

	r_devm = devres_alloc(devm_usb_repeater_release_by_node,
					sizeof(*r_devm), GFP_KERNEL);
	if (!r_devm) {
		dev_dbg(dev, "failed to allocate memory for r_devm\n");
		return r;
	}

	spin_lock_irqsave(&repeater_lock, flags);
	r = of_usb_find_repeater(node);
	if (IS_ERR(r)) {
		devres_free(r_devm);
		goto err0;
	}

	if (!try_module_get(r->dev->driver->owner)) {
		r = ERR_PTR(-ENODEV);
		devres_free(r_devm);
		goto err0;
	}

	r_devm->dev = get_device(r->dev);
	r_devm->owner = r->dev->driver->owner;
	spin_unlock_irqrestore(&repeater_lock, flags);
	/* Unbind this consumer before releasing its supplier's managed storage. */
	link = device_link_add(dev, r_devm->dev, DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		ret = -ENOMEM;
		goto release;
	}
	/* A supplier that is still probing or already unbinding cannot be used. */
	if (READ_ONCE(link->status) != DL_STATE_CONSUMER_PROBE &&
	    READ_ONCE(link->status) != DL_STATE_ACTIVE) {
		ret = -EPROBE_DEFER;
		goto release;
	}
	/* Recheck after linking in case the supplier rebound during setup. */
	spin_lock_irqsave(&repeater_lock, flags);
	r = of_usb_find_repeater(node);
	ret = IS_ERR(r) ? PTR_ERR(r) :
		(r->dev != r_devm->dev || r->dev->driver->owner != r_devm->owner) ?
		-EPROBE_DEFER : 0;
	spin_unlock_irqrestore(&repeater_lock, flags);
	if (ret)
		goto release;
	devres_add(dev, r_devm);
	return r;
release:
	devm_usb_repeater_release_by_node(dev, r_devm);
	devres_free(r_devm);
	return ERR_PTR(ret);
err0:
	spin_unlock_irqrestore(&repeater_lock, flags);
	return r;
}
EXPORT_SYMBOL_GPL(devm_usb_get_repeater_by_node);

struct usb_repeater *devm_usb_get_repeater_by_phandle(struct device *dev,
	const char *phandle, u8 index)
{
	struct device_node *node;
	struct usb_repeater *r;

	if (!dev->of_node) {
		dev_dbg(dev, "device does not have a device node entry\n");
		return ERR_PTR(-EINVAL);
	}

	node = of_parse_phandle(dev->of_node, phandle, index);
	if (!node) {
		dev_dbg(dev, "failed to get %s phandle in %pOF node\n", phandle,
			dev->of_node);
		return ERR_PTR(-ENODEV);
	}

	r = devm_usb_get_repeater_by_node(dev, node);
	of_node_put(node);
	return r;
}
EXPORT_SYMBOL_GPL(devm_usb_get_repeater_by_phandle);

struct usb_repeater *usb_get_repeater_by_node(struct device_node *node)
{
	struct usb_repeater *r = ERR_PTR(-ENODEV);
	unsigned long flags;

	spin_lock_irqsave(&repeater_lock, flags);
	r = of_usb_find_repeater(node);
	spin_unlock_irqrestore(&repeater_lock, flags);

	return r;
}
EXPORT_SYMBOL_GPL(usb_get_repeater_by_node);

struct usb_repeater *usb_get_repeater_by_phandle(struct device *dev,
	const char *phandle, u8 index)
{
	struct device_node *node;
	struct usb_repeater *r;

	if (!dev->of_node) {
		dev_dbg(dev, "device does not have a device node entry\n");
		return ERR_PTR(-EINVAL);
	}

	node = of_parse_phandle(dev->of_node, phandle, index);
	if (!node) {
		dev_dbg(dev, "failed to get %s phandle in %pOF node\n", phandle,
			dev->of_node);
		return ERR_PTR(-ENODEV);
	}

	r = usb_get_repeater_by_node(node);
	of_node_put(node);
	return r;
}
EXPORT_SYMBOL_GPL(usb_get_repeater_by_phandle);

/**
 * usb_add_repeater_dev - Add repeater device
 * @r: repeater device available
 *
 * It shall used by any USB repeater driver to register
 * device to allow USB PHY driver to get access to it
 * for invoking repeater specific operations.
 */
int usb_add_repeater_dev(struct usb_repeater *r)
{
	struct usb_repeater *entry;
	unsigned long flags;

	if (!r || !r->dev || !r->dev->driver) {
		pr_err("no repeater device\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&repeater_lock, flags);
	list_for_each_entry(entry, &repeater_list, head) {
		if (entry->dev == r->dev) {
			spin_unlock_irqrestore(&repeater_lock, flags);
			return -EEXIST;
		}
	}
	INIT_LIST_HEAD(&r->head);
	list_add_tail(&r->head, &repeater_list);
	spin_unlock_irqrestore(&repeater_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_add_repeater_dev);

/**
 * usb_remove_repeater_dev - remove repeater device
 * @r: USB repeater device to remove from repeater list
 */
void usb_remove_repeater_dev(struct usb_repeater *r)
{
	unsigned long flags;

	spin_lock_irqsave(&repeater_lock, flags);
	if (r)
		list_del(&r->head);
	spin_unlock_irqrestore(&repeater_lock, flags);
}
EXPORT_SYMBOL_GPL(usb_remove_repeater_dev);

MODULE_DESCRIPTION("USB repeater framework");
MODULE_LICENSE("GPL");
