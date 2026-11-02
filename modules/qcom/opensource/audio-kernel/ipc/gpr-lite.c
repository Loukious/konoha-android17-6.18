/* Copyright (c) 2011-2017, 2019-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2018, Linaro Limited
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/of_device.h>
#include <ipc/gpr-lite.h>
#include <linux/rpmsg.h>
#include <linux/of.h>
#include <linux/kref.h>

#include <soc/snd_event.h>
#include <dsp/audio_notifier.h>

#define APM_EVENT_MODULE_TO_CLIENT	0x03001000
#define WAKELOCK_TIMEOUT 200

struct gpr {
	struct rpmsg_endpoint *ch;
	struct device *dev;
	spinlock_t gpr_lock;
	bool is_initial_boot;

	spinlock_t svcs_lock;
	struct idr svcs_idr;
	int dest_domain_id;
	struct kref ref;
	struct notifier_block service_nb;
	const char *notifier_name;
	bool stopping;
	bool notifier_registered;
	bool snd_registered;
	struct wakeup_source *wsource;
};

static struct gpr_q6 q6;

/* Serialize drvdata publication with reference acquisition by RX and TX. */
static DEFINE_SPINLOCK(gpr_instances_lock);
static struct gpr *gpr_domains[GPR_DOMAIN_MAX];

static void gpr_release(struct kref *ref)
{
	struct gpr *gpr = container_of(ref, struct gpr, ref);

	idr_destroy(&gpr->svcs_idr);
	kfree(gpr);
}

static struct gpr *gpr_get(struct device *dev)
{
	struct gpr *gpr;
	unsigned long flags;

	spin_lock_irqsave(&gpr_instances_lock, flags);
	gpr = dev_get_drvdata(dev);
	if (gpr)
		kref_get(&gpr->ref);
	spin_unlock_irqrestore(&gpr_instances_lock, flags);
	return gpr;
}

static void gpr_withdraw(struct gpr *gpr)
{
	unsigned long flags;

	spin_lock_irqsave(&gpr_instances_lock, flags);
	if (dev_get_drvdata(gpr->dev) == gpr)
		dev_set_drvdata(gpr->dev, NULL);
	spin_lock(&gpr->gpr_lock);
	gpr->stopping = true;
	gpr->ch = NULL;
	spin_unlock(&gpr->gpr_lock);
	spin_unlock_irqrestore(&gpr_instances_lock, flags);
}

static void gpr_unreserve(struct gpr *gpr)
{
	unsigned long flags;

	/* Retain the domain until deregistration by client name has drained. */
	spin_lock_irqsave(&gpr_instances_lock, flags);
	if (gpr_domains[gpr->dest_domain_id] == gpr)
		gpr_domains[gpr->dest_domain_id] = NULL;
	spin_unlock_irqrestore(&gpr_instances_lock, flags);
}

static bool gpr_packet_layout_valid(u32 header)
{
	u32 header_size = GPR_PKT_GET_HEADER_BYTE_SIZE(header);
	u32 packet_size = GPR_PKT_GET_PACKET_BYTE_SIZE(header);

	return GPR_GET_FIELD(GPR_PKT_VERSION, header) == GPR_PKT_VER &&
	       header_size >= GPR_HDR_SIZE && header_size <= packet_size;
}

static int gpr_remove_device(struct device *dev, void *data);

enum gpr_subsys_state gpr_get_q6_state(void)
{
	return atomic_read(&q6.q6_state);
}
EXPORT_SYMBOL(gpr_get_q6_state);

enum gpr_subsys_state gpr_get_modem_state(void)
{
	return atomic_read(&q6.modem_state);
}
EXPORT_SYMBOL(gpr_get_modem_state);

/**
 * gpr_send_pkt() - Send a packet from a registered service device
 * @adev: Service device owned by the client
 * @pkt: Complete packet allocation described by its wire header
 *
 * Return: Packet size on success or a negative error.
 */
int gpr_send_pkt(struct gpr_device *adev, struct gpr_pkt *pkt)
{
	struct gpr *gpr;
	unsigned long flags;
	u32 packet_size;
	int ret;

	if (!adev || !pkt || !adev->dev.parent ||
	    !gpr_packet_layout_valid(pkt->hdr.header))
		return -EINVAL;
	gpr = gpr_get(adev->dev.parent);
	if (!gpr)
		return -ENETRESET;
	spin_lock_irqsave(&gpr->gpr_lock, flags);
	if (gpr->stopping || !gpr->ch ||
	    (adev->domain_id == GPR_DOMAIN_ADSP &&
	     gpr_get_q6_state() != GPR_SUBSYS_LOADED) ||
	    (adev->domain_id == GPR_DOMAIN_MODEM &&
	     gpr_get_modem_state() == GPR_SUBSYS_DOWN)) {
		ret = -ENETRESET;
		goto unlock;
	}
	packet_size = GPR_PKT_GET_PACKET_BYTE_SIZE(pkt->hdr.header);
	pkt->hdr.dst_domain_id = adev->domain_id;
	/* Caller owns the packet allocation described by its wire size. */
	ret = rpmsg_trysend(gpr->ch, pkt, packet_size);
	if (!ret)
		ret = packet_size;
unlock:
	spin_unlock_irqrestore(&gpr->gpr_lock, flags);
	kref_put(&gpr->ref, gpr_release);
	return ret;
}
EXPORT_SYMBOL_GPL(gpr_send_pkt);

 /**
  * apr_set_modem_state - Update modem load status.
  *
  * @state: State to update modem load status
  *
 */
void gpr_set_modem_state(enum gpr_subsys_state state)
{
	if (state >= GPR_SUBSYS_DOWN && state <= GPR_SUBSYS_LOADED)
		atomic_set(&q6.modem_state, state);
}
EXPORT_SYMBOL(gpr_set_modem_state);

int gpr_set_q6_state(enum gpr_subsys_state state)
{
	if (state < GPR_SUBSYS_DOWN || state > GPR_SUBSYS_LOADED)
		return -EINVAL;
	atomic_set(&q6.q6_state, state);
	return 0;
}
EXPORT_SYMBOL(gpr_set_q6_state);
static void gpr_ssr_disable(struct device *dev, void *data)
{
	struct gpr *gpr = data;

	if (gpr->dest_domain_id == GPR_DOMAIN_ADSP)
		gpr_set_q6_state(GPR_SUBSYS_DOWN);
	else
		gpr_set_modem_state(GPR_SUBSYS_DOWN);
}

static const struct snd_event_ops gpr_ssr_ops = {
	.disable = gpr_ssr_disable,
};
static int gpr_notifier_service_cb(struct notifier_block *this,
				 unsigned long opcode, void *data)
{
	struct gpr *gpr = container_of(this, struct gpr, service_nb);
	struct audio_notifier_cb_data *cb_data = data;
	unsigned long flags;
	bool up;
	int domain = gpr->dest_domain_id == GPR_DOMAIN_ADSP ?
		     AUDIO_NOTIFIER_ADSP_DOMAIN : AUDIO_NOTIFIER_MODEM_DOMAIN;

	if (!cb_data || cb_data->domain != domain ||
	    (opcode != AUDIO_NOTIFIER_SERVICE_DOWN &&
	     opcode != AUDIO_NOTIFIER_SERVICE_UP))
		return NOTIFY_OK;
	up = opcode == AUDIO_NOTIFIER_SERVICE_UP;
	spin_lock_irqsave(&gpr->gpr_lock, flags);
	if (gpr->stopping || (!up && gpr->is_initial_boot)) {
		spin_unlock_irqrestore(&gpr->gpr_lock, flags);
		return NOTIFY_OK;
	}
	if (up)
		gpr->is_initial_boot = false;
	spin_unlock_irqrestore(&gpr->gpr_lock, flags);
	if (gpr->dest_domain_id == GPR_DOMAIN_ADSP)
		gpr_set_q6_state(up ? GPR_SUBSYS_LOADED : GPR_SUBSYS_DOWN);
	else
		gpr_set_modem_state(up ? GPR_SUBSYS_UP : GPR_SUBSYS_DOWN);
	snd_event_notify(gpr->dev, up ? SND_EVENT_UP : SND_EVENT_DOWN);
	return NOTIFY_OK;
}

static void gpr_dev_release(struct device *dev)
{
	struct gpr_device *adev = to_gpr_device(dev);

	of_node_put(adev->dev.of_node);
	kfree(adev);
}

static int gpr_dispatch(struct gpr *gpr, u32 svc_id, void *buf)
{
	struct gpr_device *svc;
	struct gpr_driver *driver;
	unsigned long flags;
	int ret = -ENODEV;

	spin_lock_irqsave(&gpr->svcs_lock, flags);
	svc = idr_find(&gpr->svcs_idr, svc_id);
	if (svc)
		get_device(&svc->dev);
	spin_unlock_irqrestore(&gpr->svcs_lock, flags);
	if (!svc)
		return ret;
	/* Driver removal uses the same lock; never invoke a withdrawn callback. */
	device_lock(&svc->dev);
	if (svc->dev.driver) {
		driver = to_gpr_driver(svc->dev.driver);
		if (driver->callback) {
			driver->callback(svc, buf);
			ret = 0;
		}
	}
	device_unlock(&svc->dev);
	put_device(&svc->dev);
	return ret;
}

static int gpr_callback(struct rpmsg_device *rpdev, void *buf,
			int len, void *priv, u32 addr)
{
	struct gpr *gpr;
	struct gpr_hdr *hdr = buf;
	unsigned long flags;
	int ret;

	if (!buf || len < (int)GPR_HDR_SIZE ||
	    !gpr_packet_layout_valid(hdr->header) ||
	    GPR_PKT_GET_PACKET_BYTE_SIZE(hdr->header) != len)
		return -EINVAL;
	gpr = gpr_get(&rpdev->dev);
	if (!gpr)
		return -ENETRESET;
	spin_lock_irqsave(&gpr->gpr_lock, flags);
	if (gpr->stopping) {
		spin_unlock_irqrestore(&gpr->gpr_lock, flags);
		ret = -ENETRESET;
		goto put;
	}
	if (hdr->opcode == APM_EVENT_MODULE_TO_CLIENT)
		pm_wakeup_ws_event(gpr->wsource, WAKELOCK_TIMEOUT, true);
	spin_unlock_irqrestore(&gpr->gpr_lock, flags);
	ret = gpr_dispatch(gpr, hdr->dst_port, buf);
	if (ret == -ENODEV && hdr->dst_port != GPR_SVC_MAX)
		ret = gpr_dispatch(gpr, GPR_SVC_MAX, buf);
	if (ret && hdr->opcode == APM_EVENT_MODULE_TO_CLIENT) {
		spin_lock_irqsave(&gpr->gpr_lock, flags);
		if (!gpr->stopping)
			__pm_relax(gpr->wsource);
		spin_unlock_irqrestore(&gpr->gpr_lock, flags);
	}
put:
	kref_put(&gpr->ref, gpr_release);
	return ret;
}

static int gpr_device_match(struct device *dev, const struct device_driver *drv)
{
	struct gpr_device *adev = to_gpr_device(dev);
	const struct gpr_driver *adrv =
		container_of_const(drv, struct gpr_driver, driver);
	const struct gpr_device_id *id = adrv->id_table;

	/* Attempt an OF style match first */
	if (of_driver_match_device(dev, drv))
		return 1;

	if (!id)
		return 0;

	while (id->domain_id != 0 || id->svc_id != 0) {
		if (id->domain_id == adev->domain_id &&
		    id->svc_id == adev->svc_id)
			return 1;
		id++;
	}

	return 0;
}

static int gpr_device_probe(struct device *dev)
{
	struct gpr_device *adev = to_gpr_device(dev);
	struct gpr_driver *adrv = to_gpr_driver(dev->driver);

	return adrv->probe ? adrv->probe(adev) : 0;
}

static void gpr_device_remove(struct device *dev)
{
	struct gpr_device *adev = to_gpr_device(dev);
	struct gpr_driver *driver = to_gpr_driver(dev->driver);

	/* Keep the routing entry while this service device can bind again. */
	if (driver->remove)
		driver->remove(adev);
}

static int gpr_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct gpr_device *adev =
		container_of_const(dev, struct gpr_device, dev);
	int ret;

	ret = of_device_uevent_modalias(dev, env);
	if (ret != -ENODEV)
		return ret;

	return add_uevent_var(env, "MODALIAS=gpr:%s", adev->name);
}

struct bus_type gprbus = {
	.name		= "gprbus",
	.match		= gpr_device_match,
	.probe		= gpr_device_probe,
	.uevent		= gpr_uevent,
	.remove		= gpr_device_remove,
};
EXPORT_SYMBOL_GPL(gprbus);
static int gpr_add_device(struct device *dev, struct device_node *np,
			  const struct gpr_device_id *id)
{
	struct gpr *gpr = dev_get_drvdata(dev);
	struct gpr_device *adev;
	unsigned long flags;
	u32 svc_id = id->svc_id;
	int ret;

	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return -ENOMEM;
	spin_lock_init(&adev->lock);
	adev->svc_id = id->svc_id;
	adev->domain_id = id->domain_id;
	adev->version = id->svc_version;
	strscpy(adev->name, np ? np->name : id->name, GPR_NAME_SIZE);
	adev->dev.bus = &gprbus;
	adev->dev.parent = dev;
	adev->dev.of_node = of_node_get(np);
	adev->dev.release = gpr_dev_release;
	device_initialize(&adev->dev);
	ret = dev_set_name(&adev->dev, "gprsvc:%s:%x:%x", adev->name,
			   id->domain_id, id->svc_id);
	if (ret)
		goto put;
	spin_lock_irqsave(&gpr->svcs_lock, flags);
	ret = idr_alloc_u32(&gpr->svcs_idr, adev, &svc_id, svc_id, GFP_ATOMIC);
	spin_unlock_irqrestore(&gpr->svcs_lock, flags);
	if (ret)
		goto put;
	ret = device_add(&adev->dev);
	if (!ret)
		return 0;
	spin_lock_irqsave(&gpr->svcs_lock, flags);
	idr_remove(&gpr->svcs_idr, svc_id);
	spin_unlock_irqrestore(&gpr->svcs_lock, flags);
put:
	put_device(&adev->dev);
	return ret;
}

static int of_register_gpr_devices(struct device *dev)
{
	struct gpr *gpr = dev_get_drvdata(dev);
	struct device_node *node;
	int ret;

	for_each_available_child_of_node(dev->of_node, node) {
		struct gpr_device_id id = { 0 };

		if (of_property_read_u32(node, "reg", &id.svc_id))
			continue;
		id.domain_id = gpr->dest_domain_id;
		ret = gpr_add_device(dev, node, &id);
		if (ret) {
			of_node_put(node);
			return ret;
		}
	}
	return 0;
}

static int gpr_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct gpr *gpr;
	unsigned long flags;
	u32 domain;
	int ret;

	if (!audio_notifier_probe_status())
		return -EPROBE_DEFER;
	ret = of_property_read_u32(dev->of_node, "reg", &domain);
	if (ret)
		return ret;
	if (domain != GPR_DOMAIN_ADSP && domain != GPR_DOMAIN_MODEM)
		return -EINVAL;
	gpr = kzalloc(sizeof(*gpr), GFP_KERNEL);
	if (!gpr)
		return -ENOMEM;
	kref_init(&gpr->ref);
	spin_lock_init(&gpr->gpr_lock);
	spin_lock_init(&gpr->svcs_lock);
	idr_init(&gpr->svcs_idr);
	gpr->is_initial_boot = true;
	gpr->dev = dev;
	gpr->ch = rpdev->ept;
	gpr->dest_domain_id = domain;
	gpr->service_nb.notifier_call = gpr_notifier_service_cb;
	gpr->notifier_name = domain == GPR_DOMAIN_ADSP ? "gpr_adsp" : "gpr_modem";
	gpr->wsource = wakeup_source_register(dev, "audio-gpr");
	if (!gpr->wsource) {
		ret = -ENOMEM;
		goto put;
	}
	spin_lock_irqsave(&gpr_instances_lock, flags);
	if (gpr_domains[domain]) {
		spin_unlock_irqrestore(&gpr_instances_lock, flags);
		ret = -EBUSY;
		goto wakeup;
	}
	gpr_domains[domain] = gpr;
	dev_set_drvdata(dev, gpr);
	spin_unlock_irqrestore(&gpr_instances_lock, flags);
	ret = snd_event_client_register(dev, &gpr_ssr_ops, gpr);
	if (ret)
		goto withdraw;
	gpr->snd_registered = true;
	ret = audio_notifier_register((char *)gpr->notifier_name,
				      domain == GPR_DOMAIN_ADSP ?
				      AUDIO_NOTIFIER_ADSP_DOMAIN : AUDIO_NOTIFIER_MODEM_DOMAIN,
				      &gpr->service_nb);
	if (ret)
		goto withdraw;
	gpr->notifier_registered = true;
	ret = of_register_gpr_devices(dev);
	if (!ret)
		return 0;
withdraw:
	gpr_withdraw(gpr);
	if (gpr->notifier_registered)
		audio_notifier_deregister((char *)gpr->notifier_name);
	device_for_each_child(dev, gpr, gpr_remove_device);
	if (gpr->snd_registered)
		snd_event_client_deregister(dev);
	if (domain == GPR_DOMAIN_ADSP)
		gpr_set_q6_state(GPR_SUBSYS_DOWN);
	else
		gpr_set_modem_state(GPR_SUBSYS_DOWN);
wakeup:
	wakeup_source_unregister(gpr->wsource);
put:
	gpr_unreserve(gpr);
	kref_put(&gpr->ref, gpr_release);
	return ret;
}

static int gpr_remove_device(struct device *dev, void *data)
{
	struct gpr_device *adev = to_gpr_device(dev);
	struct gpr *gpr = data;
	unsigned long flags;

	spin_lock_irqsave(&gpr->svcs_lock, flags);
	idr_remove(&gpr->svcs_idr, adev->svc_id);
	spin_unlock_irqrestore(&gpr->svcs_lock, flags);
	device_unregister(dev);
	return 0;
}

static void gpr_remove(struct rpmsg_device *rpdev)
{
	struct gpr *gpr = dev_get_drvdata(&rpdev->dev);

	gpr_withdraw(gpr);
	if (gpr->notifier_registered)
		audio_notifier_deregister((char *)gpr->notifier_name);
	device_for_each_child(gpr->dev, gpr, gpr_remove_device);
	if (gpr->snd_registered)
		snd_event_client_deregister(gpr->dev);
	if (gpr->dest_domain_id == GPR_DOMAIN_ADSP)
		gpr_set_q6_state(GPR_SUBSYS_DOWN);
	else
		gpr_set_modem_state(GPR_SUBSYS_DOWN);
	wakeup_source_unregister(gpr->wsource);
	gpr_unreserve(gpr);
	kref_put(&gpr->ref, gpr_release);
}

/*
 * __gpr_driver_register() - Client driver registration with gprbus
 *
 * @drv:Client driver to be associated with client-device.
 * @owner: owning module/driver
 *
 * This API will register the client driver with the gprbus
 * It is called from the driver's module-init function.
 */
int __gpr_driver_register(struct gpr_driver *drv, struct module *owner)
{
	drv->driver.bus = &gprbus;
	drv->driver.owner = owner;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(__gpr_driver_register);

/*
 * gpr_driver_unregister() - Undo effect of gpr_driver_register
 *
 * @drv: Client driver to be unregistered
 */
void gpr_driver_unregister(struct gpr_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(gpr_driver_unregister);

static const struct of_device_id gpr_of_match[] = {
	{ .compatible = "qcom,gpr"},
	{}
};
MODULE_DEVICE_TABLE(of, gpr_of_match);

static struct rpmsg_driver gpr_driver = {
	.probe = gpr_probe,
	.remove = gpr_remove,
	.callback = gpr_callback,
	.drv = {
		.name = "qcom,gpr",
		.of_match_table = gpr_of_match,
	},
};
static int __init gpr_init(void)
{
	int ret;

	ret = bus_register(&gprbus);
	if (ret)
		return ret;
	ret = register_rpmsg_driver(&gpr_driver);
	if (ret)
		bus_unregister(&gprbus);
	return ret;
}

static void __exit gpr_exit(void)
{
	unregister_rpmsg_driver(&gpr_driver);
	bus_unregister(&gprbus);
}

subsys_initcall(gpr_init);
module_exit(gpr_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QTI GPR Bus");
