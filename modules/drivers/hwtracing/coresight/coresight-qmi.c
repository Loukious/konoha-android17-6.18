// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/io.h>
#include <linux/err.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/coresight.h>
#define CORESIGHT_QMI_DEFINE_TABLES
#include "coresight-qmi.h"
#include "coresight-priv.h"

DEFINE_CORESIGHT_DEVLIST(qmi_devs, "qmi");

static int service_coresight_qmi_new_server(struct qmi_handle *qmi,
		struct qmi_service *svc)
{
	struct qmi_drvdata *d = container_of(qmi, struct qmi_drvdata, handle);
	unsigned long flags;

	raw_spin_lock_irqsave(&d->server_lock, flags);
	if (!d->removing) {
		d->s_addr.sq_family = AF_QIPCRTR;
		d->s_addr.sq_node = svc->node;
		d->s_addr.sq_port = svc->port;
		d->service_connected = true;
	}
	raw_spin_unlock_irqrestore(&d->server_lock, flags);
	return 0;
}

static void service_coresight_qmi_del_server(struct qmi_handle *qmi,
		struct qmi_service *svc)
{
	struct qmi_drvdata *d = container_of(qmi, struct qmi_drvdata, handle);
	unsigned long flags;

	raw_spin_lock_irqsave(&d->server_lock, flags);
	if (d->s_addr.sq_node == svc->node && d->s_addr.sq_port == svc->port)
		d->service_connected = false;
	raw_spin_unlock_irqrestore(&d->server_lock, flags);
}

static struct qmi_ops server_ops = {
	.new_server = service_coresight_qmi_new_server,
	.del_server = service_coresight_qmi_del_server,
};

/* Serialize transactions without blocking QMI receive/server callbacks. */
static int coresight_qmi_request(struct qmi_drvdata *d, unsigned int msg_id,
		size_t max_len, const struct qmi_elem_info *req_ei, const void *req,
		const struct qmi_elem_info *resp_ei, void *resp,
		struct qmi_response_type_v01 *result)
{
	struct sockaddr_qrtr address;
	struct qmi_txn txn;
	unsigned long flags;
	int ret;

	mutex_lock(&d->mutex);
	raw_spin_lock_irqsave(&d->server_lock, flags);
	ret = d->removing ? -ENODEV : d->service_connected ? 0 : -ENOTCONN;
	address = d->s_addr;
	raw_spin_unlock_irqrestore(&d->server_lock, flags);
	if (ret)
		goto out;
	ret = qmi_txn_init(&d->handle, &txn, resp_ei, resp);
	if (ret)
		goto out;
	ret = qmi_send_request(&d->handle, &address, &txn, msg_id,
			       max_len, req_ei, req);
	if (ret) {
		qmi_txn_cancel(&txn);
		goto out;
	}
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(TIMEOUT_MS));
	if (!ret && result->result != QMI_RESULT_SUCCESS_V01)
		ret = -EREMOTEIO;
out:
	mutex_unlock(&d->mutex);
	return ret;
}

int coresight_qmi_remote_etm_enable(struct coresight_device *csdev)
{
	struct qmi_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	struct coresight_set_etm_req_msg_v01 req = { .state = CORESIGHT_ETM_STATE_ENABLED_V01 };
	struct coresight_set_etm_resp_msg_v01 resp = {};

	return coresight_qmi_request(d, CORESIGHT_QMI_SET_ETM_REQ_V01,
		CORESIGHT_QMI_SET_ETM_REQ_MAX_LEN, coresight_set_etm_req_msg_v01_ei,
		&req, coresight_set_etm_resp_msg_v01_ei, &resp, &resp.resp);
}
EXPORT_SYMBOL_GPL(coresight_qmi_remote_etm_enable);

int coresight_qmi_remote_etm_disable(struct coresight_device *csdev)
{
	struct qmi_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	struct coresight_set_etm_req_msg_v01 req = { .state = CORESIGHT_ETM_STATE_DISABLED_V01 };
	struct coresight_set_etm_resp_msg_v01 resp = {};

	return coresight_qmi_request(d, CORESIGHT_QMI_SET_ETM_REQ_V01,
		CORESIGHT_QMI_SET_ETM_REQ_MAX_LEN, coresight_set_etm_req_msg_v01_ei,
		&req, coresight_set_etm_resp_msg_v01_ei, &resp, &resp.resp);
}
EXPORT_SYMBOL_GPL(coresight_qmi_remote_etm_disable);

/*
 * remote_etm_etr_assign - reassign the ownership of an ETR instance to specified
 * subsystem.
 */
int coresight_qmi_etr_assign(struct coresight_device *csdev,
		struct coresight_etr_assign_req_msg_v01 *req)
{
	struct qmi_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	struct coresight_etr_assign_resp_msg_v01 resp = {};

	if (!req || !req->buffer_size || req->buffer_base > U64_MAX - req->buffer_size)
		return -EINVAL;
	return coresight_qmi_request(d, CORESIGHT_QMI_ETR_ASSIGN_REQ_V01,
		CORESIGHT_QMI_ETR_ASSIGN_REQ_MAX_LEN, coresight_etr_assign_req_msg_v01_ei,
		req, coresight_etr_assign_resp_msg_v01_ei, &resp, &resp.resp);
}
EXPORT_SYMBOL_GPL(coresight_qmi_etr_assign);

int coresight_qmi_assign_atid(struct coresight_device *csdev,
		struct coresight_atid_assign_req_msg_v01 *req)
{
	struct qmi_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	struct coresight_atid_assign_resp_msg_v01 resp = {};
	unsigned int i, j;

	if (!req || !req->num_atids || req->num_atids > ARRAY_SIZE(req->atids) ||
	    !req->name[0] || strnlen(req->name, sizeof(req->name)) == sizeof(req->name))
		return -EINVAL;
	for (i = 0; i < req->num_atids; i++) {
		if (!req->atids[i] || req->atids[i] >= 0x70)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (req->atids[j] == req->atids[i])
				return -EINVAL;
	}
	return coresight_qmi_request(d, CORESIGHT_QMI_ATID_ASSIGN_V01,
		CORESIGHT_QMI_ATID_ASSIGN_REQ_MAX_LEN, coresight_atid_assign_req_msg_v01_ei,
		req, coresight_atid_assign_resp_msg_v01_ei, &resp, &resp.resp);
}
EXPORT_SYMBOL_GPL(coresight_qmi_assign_atid);

static int qmi_helper_enable(struct coresight_device *csdev, enum cs_mode mode, void *data)
{
	struct qmi_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&d->server_lock, flags);
	if (d->removing)
		ret = -ENODEV;
	else if (!d->service_connected)
		ret = -ENOTCONN;
	else if (d->path_users == UINT_MAX)
		ret = -EOVERFLOW;
	else
		d->path_users++;
	raw_spin_unlock_irqrestore(&d->server_lock, flags);
	return ret;
}

static int qmi_helper_disable(struct coresight_device *csdev, void *data)
{
	struct qmi_drvdata *d = dev_get_drvdata(csdev->dev.parent);
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&d->server_lock, flags);
	if (!d->path_users)
		ret = -EINVAL;
	else
		d->path_users--;
	raw_spin_unlock_irqrestore(&d->server_lock, flags);
	return ret;
}

static const struct coresight_ops_helper qmi_helper_ops = {
	.enable = qmi_helper_enable,
	.disable = qmi_helper_disable,
};

static const struct coresight_ops qmi_ops = {
	.helper_ops = &qmi_helper_ops,
};


static int coresight_qmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct coresight_platform_data *pdata;
	struct qmi_drvdata *drvdata;
	struct coresight_desc desc = {0 };
	int ret;

	if (device_property_read_string(dev, "coresight-name", &desc.name))
		desc.name = coresight_alloc_device_name(&qmi_devs, dev);
	if (!desc.name)
		return -ENOMEM;
	pdata = coresight_get_platform_data(dev);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);
	pdev->dev.platform_data = pdata;

	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = &pdev->dev;
	platform_set_drvdata(pdev, drvdata);

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,inst-id",
			&drvdata->inst_id);
	if (ret)
		return ret;

	mutex_init(&drvdata->mutex);
	raw_spin_lock_init(&drvdata->server_lock);

	ret = qmi_handle_init(&drvdata->handle,
			CORESIGHT_QMI_SET_ETM_RESP_MAX_LEN,
			&server_ops, NULL);
	if (ret < 0) {
		dev_err(dev, "qmi client init failed ret:%d\n", ret);
		return ret;
	}

	ret = qmi_add_lookup(&drvdata->handle,
			CORESIGHT_QMI_SVC_ID,
			CORESIGHT_QMI_VERSION,
			drvdata->inst_id);
	if (ret)
		goto err;

	desc.type = CORESIGHT_DEV_TYPE_HELPER;
	desc.subtype.helper_subtype = CORESIGHT_DEV_SUBTYPE_HELPER_QCOM_QMI;
	desc.pdata = pdev->dev.platform_data;
	desc.dev = &pdev->dev;
	desc.ops = &qmi_ops;
	drvdata->csdev = coresight_register(&desc);
	if (IS_ERR(drvdata->csdev)) {
		ret = PTR_ERR(drvdata->csdev);
		goto err;
	}
	dev_info(dev, "qmi initialized\n");

	return 0;
err:
	qmi_handle_release(&drvdata->handle);
	return ret;
}

static void coresight_qmi_remove(struct platform_device *pdev)
{
	struct qmi_drvdata *d = platform_get_drvdata(pdev);
	unsigned long flags;

	mutex_lock(&d->mutex);
	raw_spin_lock_irqsave(&d->server_lock, flags);
	d->removing = true;
	d->service_connected = false;
	raw_spin_unlock_irqrestore(&d->server_lock, flags);
	mutex_unlock(&d->mutex);
	coresight_unregister(d->csdev);
	qmi_handle_release(&d->handle);
}

static const struct of_device_id coresight_qmi_match[] = {
	{.compatible = "qcom,coresight-qmi"},
	{}
};
MODULE_DEVICE_TABLE(of, coresight_qmi_match);

static struct platform_driver coresight_qmi_driver = {
	.probe          = coresight_qmi_probe,
	.remove         = coresight_qmi_remove,
	.driver         = {
		.name   = "coresight-qmi",
		.of_match_table = coresight_qmi_match,
		.suppress_bind_attrs = true,
	},
};

int __init coresight_qmi_init(void)
{
	return platform_driver_register(&coresight_qmi_driver);
}
module_init(coresight_qmi_init);

void __exit coresight_qmi_exit(void)
{
	platform_driver_unregister(&coresight_qmi_driver);
}
module_exit(coresight_qmi_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CoreSight QMI driver");

