// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/net.h>

#include "thermal_mitigation_device_service_v01.h"

#define QMI_CDEV_DRIVER		"qmi-cooling-device"
#define QMI_TMD_RESP_TOUT	msecs_to_jiffies(100)

struct qmi_cooling_device {
	struct device_node *np;
	char cdev_name[QMI_TMD_MITIGATION_DEV_ID_LENGTH_MAX_V01];
	char qmi_name[QMI_TMD_MITIGATION_DEV_ID_LENGTH_MAX_V01];
	bool connection_active;
	bool pending;
	struct list_head qmi_node;
	struct thermal_cooling_device *cdev;
	unsigned int mtgn_state;
	unsigned int max_level;
	struct qmi_tmd_instance *tmd;
};

struct qmi_tmd_instance {
	struct device *dev;
	struct qmi_handle handle;
	/* Serializes requests without blocking receive callbacks. */
	struct mutex mutex;
	/* Protects connection generations, desired state and callback queueing. */
	spinlock_t state_lock;
	struct sockaddr_qrtr server;
	u64 generation;
	bool online;
	bool stopping;
	bool handle_ready;
	u32 inst_id;
	struct list_head tmd_cdev_list;
	struct work_struct svc_arrive_work;
};

struct qmi_tmd_controller {
	struct qmi_tmd_instance *instances;
	unsigned int count;
};

static char  device_clients[][QMI_TMD_MITIGATION_DEV_ID_LENGTH_MAX_V01] = {
	{"pa"},
	{"pa_fr1"},
	{"cx_vdd_limit"},
	{"modem"},
	{"modem_current"},
	{"modem_skin"},
	{"modem_bw"},
	{"modem_bw_backoff"},
	{"vbatt_low"},
	{"charge_state"},
	{"mmw0"},
	{"mmw1"},
	{"mmw2"},
	{"mmw3"},
	{"mmw_skin0"},
	{"mmw_skin1"},
	{"mmw_skin2"},
	{"mmw_skin3"},
	{"wlan"},
	{"wlan_bw"},
	{"mmw_skin0_dsc"},
	{"mmw_skin1_dsc"},
	{"mmw_skin2_dsc"},
	{"mmw_skin3_dsc"},
	{"modem_skin_lte_dsc"},
	{"modem_skin_nr_dsc"},
	{"pa_dsc"},
	{"pa_fr1_dsc"},
	{"cdsp_sw"},
	{"cdsp_sw_hvx"},
	{"cdsp_sw_hmx"},
	{"cdsp_hw"},
	{"cpuv_restriction_cold"},
	{"cpr_cold"},
	{"modem_lte_dsc"},
	{"modem_nr_dsc"},
	{"modem_nr_scg_dsc"},
	{"sdr0_lte_dsc"},
	{"sdr1_lte_dsc"},
	{"sdr0_nr_dsc"},
	{"sdr1_nr_dsc"},
	{"sdr0_nr_scg_dsc"},
	{"sdr1_nr_scg_dsc"},
	{"pa_lte_sdr0_dsc"},
	{"pa_lte_sdr1_dsc"},
	{"pa_nr_sdr0_dsc"},
	{"pa_nr_sdr1_dsc"},
	{"pa_nr_sdr0_scg_dsc"},
	{"pa_nr_sdr1_scg_dsc"},
	{"mmw0_dsc"},
	{"mmw1_dsc"},
	{"mmw2_dsc"},
	{"mmw3_dsc"},
	{"mmw_ific_dsc"},
	{"modem_lte_sub1_dsc"},
	{"modem_nr_sub1_dsc"},
	{"modem_nr_scg_sub1_dsc"},
	{"sdr0_lte_sub1_dsc"},
	{"sdr1_lte_sub1_dsc"},
	{"sdr0_nr_sub1_dsc"},
	{"sdr1_nr_sub1_dsc"},
	{"pa_lte_sdr0_sub1_dsc"},
	{"pa_lte_sdr1_sub1_dsc"},
	{"pa_nr_sdr0_sub1_dsc"},
	{"pa_nr_sdr1_sub1_dsc"},
	{"pa_nr_sdr0_scg_sub1_dsc"},
	{"pa_nr_sdr1_scg_sub1_dsc"},
	{"mmw0_sub1_dsc"},
	{"mmw1_sub1_dsc"},
	{"mmw2_sub1_dsc"},
	{"mmw3_sub1_dsc"},
	{"mmw_ific_sub1_dsc"},
	{"bcl"},
};

static int qmi_get_max_state(struct thermal_cooling_device *cdev,
			     unsigned long *state)
{
	struct qmi_cooling_device *data = cdev->devdata;

	*state = READ_ONCE(data->max_level);
	return 0;
}

static int qmi_get_cur_state(struct thermal_cooling_device *cdev,
			     unsigned long *state)
{
	struct qmi_cooling_device *data = cdev->devdata;

	*state = READ_ONCE(data->mtgn_state);
	return 0;
}

/* Caller holds mutex; receive callbacks only take state_lock. */
static int qmi_tmd_request(struct qmi_tmd_instance *tmd, int id, size_t len,
			   struct qmi_elem_info *req_ei, void *req,
			   struct qmi_elem_info *resp_ei, void *resp,
			   struct qmi_response_type_v01 *result, u64 generation)
{
	struct sockaddr_qrtr server;
	struct qmi_txn txn;
	int ret;

	spin_lock(&tmd->state_lock);
	ret = tmd->stopping ? -ENODEV :
		(!tmd->online || generation != tmd->generation ? -ENETRESET : 0);
	server = tmd->server;
	spin_unlock(&tmd->state_lock);
	if (ret)
		return ret;
	ret = qmi_txn_init(&tmd->handle, &txn, resp_ei, resp);
	if (ret < 0)
		return ret;
	ret = qmi_send_request(&tmd->handle, &server, &txn, id, len, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		return ret;
	}
	ret = qmi_txn_wait(&txn, QMI_TMD_RESP_TOUT);
	if (ret < 0)
		return ret;
	if (result->result != QMI_RESULT_SUCCESS_V01)
		return -EREMOTEIO;
	spin_lock(&tmd->state_lock);
	ret = tmd->stopping ? -ENODEV :
		(!tmd->online || generation != tmd->generation ? -ENETRESET : 0);
	spin_unlock(&tmd->state_lock);
	return ret;
}

static int qmi_tmd_send_state_request(struct qmi_cooling_device *data)
{
	struct tmd_set_mitigation_level_req_msg_v01 req = {};
	struct tmd_set_mitigation_level_resp_msg_v01 resp = {};
	struct qmi_tmd_instance *tmd = data->tmd;
	u64 generation;
	int ret;

	spin_lock(&tmd->state_lock);
	generation = tmd->generation;
	req.mitigation_level = data->mtgn_state;
	spin_unlock(&tmd->state_lock);
	strscpy(req.mitigation_dev_id.mitigation_dev_id, data->qmi_name,
		sizeof(req.mitigation_dev_id.mitigation_dev_id));
	ret = qmi_tmd_request(tmd, QMI_TMD_SET_MITIGATION_LEVEL_REQ_V01,
			      TMD_SET_MITIGATION_LEVEL_REQ_MSG_V01_MAX_MSG_LEN,
		tmd_set_mitigation_level_req_msg_v01_ei, &req,
		tmd_set_mitigation_level_resp_msg_v01_ei, &resp, &resp.resp,
		generation);
	spin_lock(&tmd->state_lock);
	data->pending = ret || generation != tmd->generation || !tmd->online;
	spin_unlock(&tmd->state_lock);
	return ret;
}

static int qmi_set_cur_state(struct thermal_cooling_device *cdev,
			     unsigned long state)
{
	struct qmi_cooling_device *data = cdev->devdata;
	struct qmi_tmd_instance *tmd = data->tmd;
	bool send = false;
	int ret = 0;

	mutex_lock(&tmd->mutex);
	spin_lock(&tmd->state_lock);
	if (tmd->stopping) {
		ret = -ENODEV;
	} else if (state > data->max_level) {
		ret = -EINVAL;
	} else if (state != data->mtgn_state || data->pending) {
		/* Retain the native desired state even when delivery fails. */
		data->mtgn_state = state;
		data->pending = true;
		send = data->connection_active && tmd->online;
	}
	spin_unlock(&tmd->state_lock);
	if (send)
		ret = qmi_tmd_send_state_request(data);
	mutex_unlock(&tmd->mutex);
	return ret;
}

static struct thermal_cooling_device_ops qmi_device_ops = {
	.get_max_state = qmi_get_max_state,
	.get_cur_state = qmi_get_cur_state,
	.set_cur_state = qmi_set_cur_state,
};

static int qmi_register_cooling_device(struct qmi_cooling_device *data)
{
	struct thermal_cooling_device *cdev;

	cdev = thermal_of_cooling_device_register(data->np, data->cdev_name,
						  data, &qmi_device_ops);
	if (IS_ERR(cdev))
		return PTR_ERR(cdev);
	data->cdev = cdev;
	return 0;
}

static int qmi_tmd_accept_device(struct qmi_tmd_instance *tmd,
				 struct tmd_mitigation_dev_list_type_v01 *remote,
				u64 generation)
{
	struct qmi_cooling_device *data;
	bool changed;
	int ret = 0;

	if (strnlen(remote->mitigation_dev_id.mitigation_dev_id,
		    sizeof(remote->mitigation_dev_id.mitigation_dev_id)) ==
	    sizeof(remote->mitigation_dev_id.mitigation_dev_id))
		return -EPROTO;
	list_for_each_entry(data, &tmd->tmd_cdev_list, qmi_node) {
		if (strcasecmp(data->qmi_name, remote->mitigation_dev_id.mitigation_dev_id))
			continue;
		mutex_lock(&tmd->mutex);
		spin_lock(&tmd->state_lock);
		if (tmd->stopping || !tmd->online || generation != tmd->generation) {
			spin_unlock(&tmd->state_lock);
			mutex_unlock(&tmd->mutex);
			return -ENETRESET;
		}
		data->connection_active = true;
		changed = data->max_level != remote->max_mitigation_level;
		data->max_level = remote->max_mitigation_level;
		data->mtgn_state = min(data->mtgn_state, data->max_level);
		data->pending = true;
		spin_unlock(&tmd->state_lock);
		ret = qmi_tmd_send_state_request(data);
		mutex_unlock(&tmd->mutex);
		/* Registration may call thermal operations: hold no transaction lock. */
		if (!data->cdev)
			ret = qmi_register_cooling_device(data);
		else if (changed)
			thermal_cooling_device_update(data->cdev);
		break;
	}
	return ret;
}

static int verify_devices_and_register(struct qmi_tmd_instance *tmd)
{
	struct tmd_get_mitigation_device_list_req_msg_v01 req = {};
	struct tmd_get_mitigation_device_list_resp_msg_v01 *resp;
	u64 generation;
	unsigned int i;
	int ret;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;
	mutex_lock(&tmd->mutex);
	spin_lock(&tmd->state_lock);
	generation = tmd->generation;
	spin_unlock(&tmd->state_lock);
	ret = qmi_tmd_request(tmd, QMI_TMD_GET_MITIGATION_DEVICE_LIST_REQ_V01,
			      TMD_GET_MITIGATION_DEVICE_LIST_REQ_MSG_V01_MAX_MSG_LEN,
		tmd_get_mitigation_device_list_req_msg_v01_ei, &req,
		tmd_get_mitigation_device_list_resp_msg_v01_ei, resp, &resp->resp,
		generation);
	mutex_unlock(&tmd->mutex);
	if (ret)
		goto out;
	if ((resp->mitigation_device_list_valid &&
	     resp->mitigation_device_list_len > ARRAY_SIZE(resp->mitigation_device_list)) ||
	    (resp->mitigation_device_list_ext01_valid &&
	     resp->mitigation_device_list_ext01_len >
			ARRAY_SIZE(resp->mitigation_device_list_ext01))) {
		ret = -EPROTO;
		goto out;
	}
	for (i = 0; resp->mitigation_device_list_valid &&
	     i < resp->mitigation_device_list_len; i++) {
		ret = qmi_tmd_accept_device(tmd, &resp->mitigation_device_list[i], generation);
		if (ret)
			goto out;
	}
	for (i = 0; resp->mitigation_device_list_ext01_valid &&
	     i < resp->mitigation_device_list_ext01_len; i++) {
		ret = qmi_tmd_accept_device(tmd, &resp->mitigation_device_list_ext01[i],
					    generation);
		if (ret)
			break;
	}
out:
	kfree(resp);
	return ret;
}

static void qmi_tmd_svc_arrive(struct work_struct *work)
{
	struct qmi_tmd_instance *tmd = container_of(work, struct qmi_tmd_instance,
						 svc_arrive_work);
	int ret = verify_devices_and_register(tmd);

	if (ret)
		dev_dbg(tmd->dev, "QMI thermal discovery failed: %d\n", ret);
}

static void thermal_qmi_del_server(struct qmi_handle *qmi,
				   struct qmi_service *service)
{
	struct qmi_tmd_instance *tmd = container_of(qmi, struct qmi_tmd_instance, handle);
	struct qmi_cooling_device *data;

	spin_lock(&tmd->state_lock);
	if (!service || (service->node == tmd->server.sq_node &&
			 service->port == tmd->server.sq_port)) {
		tmd->online = false;
		tmd->generation++;
		list_for_each_entry(data, &tmd->tmd_cdev_list, qmi_node) {
			data->connection_active = false;
			data->pending = true;
		}
	}
	spin_unlock(&tmd->state_lock);
}

static void thermal_qmi_net_reset(struct qmi_handle *qmi)
{
	/* The provider has no socket here. Replay follows NEW_SERVER discovery. */
	thermal_qmi_del_server(qmi, NULL);
}

static int thermal_qmi_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	struct qmi_tmd_instance *tmd = container_of(qmi, struct qmi_tmd_instance, handle);
	struct qmi_cooling_device *data;
	int ret = 0;

	spin_lock(&tmd->state_lock);
	if (tmd->stopping) {
		ret = -ENODEV;
	} else {
		tmd->server = (struct sockaddr_qrtr) {
			.sq_family = AF_QIPCRTR, .sq_node = service->node, .sq_port = service->port,
		};
		tmd->generation++;
		tmd->online = true;
		list_for_each_entry(data, &tmd->tmd_cdev_list, qmi_node) {
			data->connection_active = false;
			data->pending = true;
		}
		queue_work(system_highpri_wq, &tmd->svc_arrive_work);
	}
	spin_unlock(&tmd->state_lock);
	return ret;
}

static struct qmi_ops thermal_qmi_event_ops = {
	.new_server = thermal_qmi_new_server,
	.del_server = thermal_qmi_del_server,
	.net_reset = thermal_qmi_net_reset,
};

static void qmi_tmd_cleanup(struct qmi_tmd_controller *controller)
{
	struct qmi_tmd_instance *tmd;
	struct qmi_cooling_device *data;
	unsigned int i;

	for (i = 0; i < controller->count; i++) {
		tmd = &controller->instances[i];
		spin_lock(&tmd->state_lock);
		tmd->stopping = true;
		tmd->online = false;
		tmd->generation++;
		spin_unlock(&tmd->state_lock);
		cancel_work_sync(&tmd->svc_arrive_work);
		list_for_each_entry(data, &tmd->tmd_cdev_list, qmi_node) {
			if (data->cdev)
				thermal_cooling_device_unregister(data->cdev);
		}
		/* Cooling unregister drains users; callbacks never wait on mutex. */
		if (tmd->handle_ready)
			qmi_handle_release(&tmd->handle);
		list_for_each_entry(data, &tmd->tmd_cdev_list, qmi_node)
			of_node_put(data->np);
	}
}

static int qmi_device_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qmi_tmd_controller *controller;
	struct qmi_tmd_instance *tmd;
	struct qmi_cooling_device *data, *other;
	struct device_node *subsys, *child;
	const char *name;
	unsigned int i, idx = 0;
	int count, ret;

	count = of_get_available_child_count(dev->of_node);
	if (!count)
		return -EINVAL;
	controller = devm_kzalloc(dev, sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return -ENOMEM;
	controller->instances = devm_kcalloc(dev, count, sizeof(*tmd), GFP_KERNEL);
	if (!controller->instances)
		return -ENOMEM;
	controller->count = count;
	for (i = 0; i < count; i++) {
		tmd = &controller->instances[i];
		tmd->dev = dev;
		mutex_init(&tmd->mutex);
		spin_lock_init(&tmd->state_lock);
		INIT_LIST_HEAD(&tmd->tmd_cdev_list);
		INIT_WORK(&tmd->svc_arrive_work, qmi_tmd_svc_arrive);
	}
	for_each_available_child_of_node(dev->of_node, subsys) {
		if (idx >= count) {
			ret = -EINVAL;
			goto put_subsys;
		}
		tmd = &controller->instances[idx];
		ret = of_property_read_u32(subsys, "qcom,instance-id", &tmd->inst_id);
		if (ret)
			goto put_subsys;
		for (i = 0; i < idx; i++) {
			if (controller->instances[i].inst_id == tmd->inst_id) {
				ret = -EINVAL;
				goto put_subsys;
			}
		}
		for_each_available_child_of_node(subsys, child) {
			ret = of_property_read_string(child, "qcom,qmi-dev-name", &name);
			if (ret)
				goto put_child;
			for (i = 0; i < ARRAY_SIZE(device_clients); i++)
				if (!strcmp(device_clients[i], name))
					break;
			if (i == ARRAY_SIZE(device_clients)) {
				ret = -EINVAL;
				goto put_child;
			}
			list_for_each_entry(other, &tmd->tmd_cdev_list, qmi_node) {
				if (!strcmp(other->qmi_name, name)) {
					ret = -EINVAL;
					goto put_child;
				}
			}
			data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
			if (!data) {
				ret = -ENOMEM;
				goto put_child;
			}
			ret = strscpy(data->cdev_name, child->name, sizeof(data->cdev_name));
			if (ret < 0)
				goto put_child;
			ret = strscpy(data->qmi_name, name, sizeof(data->qmi_name));
			if (ret < 0)
				goto put_child;
			data->tmd = tmd;
			data->np = of_node_get(child);
			data->pending = true;
			list_add_tail(&data->qmi_node, &tmd->tmd_cdev_list);
		}
		idx++;
	}
	for (i = 0; i < controller->count; i++) {
		tmd = &controller->instances[i];
		if (list_empty(&tmd->tmd_cdev_list))
			continue;
		ret = qmi_handle_init(&tmd->handle,
			TMD_GET_MITIGATION_DEVICE_LIST_RESP_MSG_V01_MAX_MSG_LEN,
			&thermal_qmi_event_ops, NULL);
		if (ret < 0)
			goto cleanup;
		tmd->handle_ready = true;
		ret = qmi_add_lookup(&tmd->handle, TMD_SERVICE_ID_V01,
				     TMD_SERVICE_VERS_V01, tmd->inst_id);
		if (ret < 0)
			goto cleanup;
	}
	platform_set_drvdata(pdev, controller);
	return 0;
put_child:
	of_node_put(child);
put_subsys:
	of_node_put(subsys);
cleanup:
	qmi_tmd_cleanup(controller);
	return ret;
}

static void qmi_device_remove(struct platform_device *pdev)
{
	qmi_tmd_cleanup(platform_get_drvdata(pdev));
}

static const struct of_device_id qmi_device_match[] = {
	{.compatible = "qcom,qmi-cooling-devices"},
	{}
};

static struct platform_driver qmi_device_driver = {
	.probe          = qmi_device_probe,
	.remove         = qmi_device_remove,
	.driver         = {
		.name   = QMI_CDEV_DRIVER,
		.of_match_table = qmi_device_match,
	},
};

static int __init qmi_device_init(void)
{
	return platform_driver_register(&qmi_device_driver);
}
module_init(qmi_device_init);

static void __exit qmi_device_exit(void)
{
	platform_driver_unregister(&qmi_device_driver);
}
module_exit(qmi_device_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QTI QMI cooling device driver");
