// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/scmi_protocol.h>
#include <linux/qcom_scmi_vendor.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/platform_device.h>

static struct scmi_device *scmi_dev;

struct scmi_device *get_qcom_scmi_device(void)
{
	struct scmi_device *sdev;

	/* Paired with publication of the initialized SCMI client. */
	sdev = smp_load_acquire(&scmi_dev);
	if (!sdev)
		return ERR_PTR(-EPROBE_DEFER);

	return sdev;
}
EXPORT_SYMBOL_GPL(get_qcom_scmi_device);

static int scmi_client_probe(struct scmi_device *sdev)
{
	if (!sdev->handle)
		return -ENODEV;
	if (READ_ONCE(scmi_dev))
		return -EBUSY;

	/* Publish the handle only after the SCMI core has initialized it. */
	smp_store_release(&scmi_dev, sdev);
	return 0;
}

static const struct scmi_device_id scmi_id_table[] = {
	{ .protocol_id = QCOM_SCMI_VENDOR_PROTOCOL, .name = "qcom_scmi_vendor_protocol" },
	{ },
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver qcom_scmi_client_drv = {
	.name		= "qcom-scmi-driver",
	.probe		= scmi_client_probe,
	.id_table	= scmi_id_table,
	.driver.suppress_bind_attrs = true,
};

/* Consumers retain the platform's SCMI handle for their lifetime. */
static int __init qcom_scmi_client_init(void)
{
	return scmi_register(&qcom_scmi_client_drv);
}
module_init(qcom_scmi_client_init);

MODULE_SOFTDEP("pre: qcom_scmi_vendor");
MODULE_DESCRIPTION("QCOM SCMI client driver");
MODULE_LICENSE("GPL");
