// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "%s:%s " fmt, KBUILD_MODNAME, __func__

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/suspend.h>
#include <linux/thermal.h>

#include "thermal_sensor_service_v02.h"
#include "qmi_sensors.h"
#include "thermal_zone_internal.h"

#define QMI_SENS_DRIVER		"qmi-therm-sensors-v2"
#define QMI_TS_RESP_TOUT	msecs_to_jiffies(100)

struct qmi_sensor {
	struct device *dev;
	char qmi_name[QMI_CLIENT_NAME_LENGTH];
	bool connection_active;
	bool trips_pending;
	struct list_head ts_node;
	struct thermal_zone_device *tz_dev;
	int last_reading;
	int high_thresh;
	int low_thresh;
	struct qmi_ts_instance *ts;
	enum qmi_ts_sensor sens_type;
	struct work_struct therm_notify_work;
};

struct qmi_ts_instance {
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
	struct list_head ts_sensor_list;
	struct work_struct svc_arrive_work;
	struct qmi_ts_controller *controller;
};

struct qmi_ts_controller {
	struct qmi_ts_instance *instances;
	unsigned int count;
	atomic_t in_suspend;
	struct notifier_block pm_nb;
	bool notifier_ready;
};

static void qmi_ts_thresh_notify(struct work_struct *work)
{
	struct qmi_sensor *sensor = container_of(work, struct qmi_sensor, therm_notify_work);
	struct qmi_ts_instance *ts = sensor->ts;
	struct thermal_zone_device *tz;

	spin_lock(&ts->state_lock);
	tz = ts->stopping ? NULL : sensor->tz_dev;
	spin_unlock(&ts->state_lock);
	if (tz)
		thermal_zone_device_update(tz, THERMAL_TRIP_VIOLATED);
}

static void qmi_ts_ind_cb_v02(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
			      struct qmi_txn *txn, const void *decoded)
{
	const struct ts_temp_report_ind_msg_v02 *ind = decoded;
	struct qmi_ts_instance *ts = container_of(qmi, struct qmi_ts_instance, handle);
	struct qmi_sensor *sensor;
	bool notify;

	if (!ind || !sq || !ind->temp_valid ||
	    strnlen(ind->sensor_id.sensor_id, sizeof(ind->sensor_id.sensor_id)) ==
			sizeof(ind->sensor_id.sensor_id))
		return;
	notify = ind->report_type != QMI_TS_TEMP_REPORT_CURRENT_TEMP_V02 || ind->seq_num_valid;
	spin_lock(&ts->state_lock);
	if (ts->stopping || !ts->online || sq->sq_node != ts->server.sq_node ||
	    sq->sq_port != ts->server.sq_port)
		goto unlock;
	list_for_each_entry(sensor, &ts->ts_sensor_list, ts_node) {
		if (!sensor->connection_active || strcasecmp(sensor->qmi_name,
							     ind->sensor_id.sensor_id))
			continue;
		sensor->last_reading = ind->temp;
		if (notify && sensor->tz_dev &&
		    (ind->temp >= sensor->high_thresh || ind->temp <= sensor->low_thresh))
			queue_work(system_highpri_wq, &sensor->therm_notify_work);
		break;
	}
unlock:
	spin_unlock(&ts->state_lock);
}

/* Caller holds mutex; receive callbacks only take state_lock. */
static int qmi_ts_send_request(struct qmi_ts_instance *ts, int id, size_t len,
			       struct qmi_elem_info *req_ei, void *req,
			   struct qmi_elem_info *resp_ei, void *resp,
			   struct qmi_response_type_v01 *result, u64 generation)
{
	struct sockaddr_qrtr server;
	struct qmi_txn txn;
	int ret;

	spin_lock(&ts->state_lock);
	ret = ts->stopping ? -ENODEV :
		atomic_read(&ts->controller->in_suspend) ? -EAGAIN :
		(!ts->online || generation != ts->generation ? -ENETRESET : 0);
	server = ts->server;
	spin_unlock(&ts->state_lock);
	if (ret)
		return ret;
	ret = qmi_txn_init(&ts->handle, &txn, resp_ei, resp);
	if (ret < 0)
		return ret;
	ret = qmi_send_request(&ts->handle, &server, &txn, id, len, req_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		return ret;
	}
	ret = qmi_txn_wait(&txn, QMI_TS_RESP_TOUT);
	if (ret < 0)
		return ret;
	if (result->result != QMI_RESULT_SUCCESS_V01)
		return -EREMOTEIO;
	spin_lock(&ts->state_lock);
	ret = ts->stopping ? -ENODEV :
		atomic_read(&ts->controller->in_suspend) ? -EAGAIN :
		(!ts->online || generation != ts->generation ? -ENETRESET : 0);
	spin_unlock(&ts->state_lock);
	return ret;
}

static int qmi_ts_request(struct qmi_sensor *sensor, bool current_temp)
{
	struct ts_register_notification_temp_resp_msg_v02 resp = {};
	struct ts_register_notification_temp_req_msg_v02 req = {};
	struct qmi_ts_instance *ts = sensor->ts;
	u64 generation;
	int ret;

	mutex_lock(&ts->mutex);
	spin_lock(&ts->state_lock);
	generation = ts->generation;
	ret = ts->stopping ? -ENODEV :
		(!sensor->connection_active || !ts->online ? -ENETRESET : 0);
	if (!ret && atomic_read(&ts->controller->in_suspend))
		ret = -EAGAIN;
	req.temp_threshold_high = sensor->high_thresh;
	req.temp_threshold_low = sensor->low_thresh;
	spin_unlock(&ts->state_lock);
	if (ret)
		goto unlock;
	strscpy(req.sensor_id.sensor_id, sensor->qmi_name, sizeof(req.sensor_id.sensor_id));
	if (current_temp) {
		req.send_current_temp_report = true;
		req.seq_num_valid = true;
	} else {
		req.temp_threshold_high_valid = req.temp_threshold_high != INT_MAX;
		req.temp_threshold_low_valid = req.temp_threshold_low != -INT_MAX;
	}
	ret = qmi_ts_send_request(ts, QMI_TS_REGISTER_NOTIFICATION_TEMP_REQ_V02,
		TS_REGISTER_NOTIFICATION_TEMP_REQ_MSG_V02_MAX_MSG_LEN,
		ts_register_notification_temp_req_msg_v02_ei, &req,
		ts_register_notification_temp_resp_msg_v02_ei, &resp, &resp.resp, generation);
	if (!current_temp) {
		spin_lock(&ts->state_lock);
		if (!ret && generation == ts->generation && ts->online &&
		    sensor->high_thresh == req.temp_threshold_high &&
		    sensor->low_thresh == req.temp_threshold_low)
			sensor->trips_pending = false;
		spin_unlock(&ts->state_lock);
	}
unlock:
	mutex_unlock(&ts->mutex);
	return ret;
}

static int qmi_sensor_read(struct thermal_zone_device *tz, int *temp)
{
	struct qmi_sensor *sensor = thermal_zone_device_priv(tz);

	/* The native protocol returns a later indication; report the cached sample. */
	qmi_ts_request(sensor, true);
	*temp = READ_ONCE(sensor->last_reading);
	return 0;
}

static int qmi_sensor_set_trips(struct thermal_zone_device *tz, int low, int high)
{
	struct qmi_sensor *sensor = thermal_zone_device_priv(tz);
	struct qmi_ts_instance *ts = sensor->ts;
	bool send = false;
	int ret = 0;

	if (low > high)
		return -EINVAL;
	spin_lock(&ts->state_lock);
	if (ts->stopping) {
		ret = -ENODEV;
	} else if (sensor->high_thresh != high || sensor->low_thresh != low ||
		   sensor->trips_pending) {
		sensor->high_thresh = high;
		sensor->low_thresh = low;
		sensor->trips_pending = true;
		send = sensor->connection_active && ts->online &&
			!atomic_read(&ts->controller->in_suspend);
	}
	spin_unlock(&ts->state_lock);
	return send ? qmi_ts_request(sensor, false) : ret;
}

static struct thermal_zone_device_ops qmi_sensor_ops = {
	.get_temp = qmi_sensor_read,
	.set_trips = qmi_sensor_set_trips,
	.change_mode = qti_tz_change_mode,
	.get_trend = qti_tz_get_trend,
};

static struct qmi_msg_handler handlers[] = {
	{
		.type = QMI_INDICATION,
		.msg_id = QMI_TS_TEMP_REPORT_IND_V02,
		.ei = ts_temp_report_ind_msg_v02_ei,
		.decoded_size = sizeof(struct ts_temp_report_ind_msg_v02),
		.fn = qmi_ts_ind_cb_v02,
	},
	{}
};

static int qmi_register_sensor_device(struct qmi_sensor *sensor)
{
	struct thermal_zone_device *tz;
	struct qmi_ts_instance *ts = sensor->ts;

	tz = devm_thermal_of_zone_register(sensor->dev, sensor->sens_type + ts->inst_id,
					   sensor, &qmi_sensor_ops);
	if (IS_ERR(tz))
		return PTR_ERR(tz);
	spin_lock(&ts->state_lock);
	sensor->tz_dev = tz;
	spin_unlock(&ts->state_lock);
	return 0;
}

static int qmi_ts_accept_sensor(struct qmi_ts_instance *ts, const char *name, u64 generation)
{
	struct qmi_sensor *sensor;
	int ret = 0;

	if (strnlen(name, QMI_TS_SENSOR_ID_LENGTH_MAX_V02 + 1) >
	    QMI_TS_SENSOR_ID_LENGTH_MAX_V02)
		return -EPROTO;
	list_for_each_entry(sensor, &ts->ts_sensor_list, ts_node) {
		if (strcasecmp(sensor->qmi_name, name))
			continue;
		spin_lock(&ts->state_lock);
		if (ts->stopping || !ts->online || generation != ts->generation) {
			spin_unlock(&ts->state_lock);
			return -ENETRESET;
		}
		sensor->connection_active = true;
		sensor->trips_pending = true;
		spin_unlock(&ts->state_lock);
		qmi_ts_request(sensor, true);
		if (!sensor->tz_dev)
			ret = qmi_register_sensor_device(sensor);
		else
			ret = qmi_ts_request(sensor, false);
		/* A listed sensor need not have a thermal zone in this board's DT. */
		if (ret == -ENODEV)
			ret = 0;
		break;
	}
	return ret;
}

static int verify_sensor_and_register(struct qmi_ts_instance *ts)
{
	struct ts_get_total_num_of_sensors_req_msg_v02 total_req = {};
	struct ts_get_total_num_of_sensors_resp_msg_v02 total_resp = {};
	struct ts_get_sensor_list_req_msg_v02 req = {};
	struct ts_get_sensor_list_resp_msg_v02 *resp;
	u64 generation;
	u32 pages, page, i;
	int ret;

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp)
		return -ENOMEM;
	mutex_lock(&ts->mutex);
	spin_lock(&ts->state_lock);
	generation = ts->generation;
	spin_unlock(&ts->state_lock);
	ret = qmi_ts_send_request(ts, QMI_TS_GET_TOTAL_NUM_OF_SENSORS_REQ_V02,
		TS_GET_TOTAL_NUM_OF_SENSORS_REQ_MSG_V02_MAX_MSG_LEN,
		ts_get_total_num_of_sensors_req_msg_v02_ei, &total_req,
		ts_get_total_num_of_sensors_resp_msg_v02_ei, &total_resp,
		&total_resp.resp, generation);
	mutex_unlock(&ts->mutex);
	if (ret)
		goto out;
	pages = total_resp.total_num_sensors / QMI_TS_SENSOR_LIST_MAX_V02 +
		!!(total_resp.total_num_sensors % QMI_TS_SENSOR_LIST_MAX_V02);
	for (page = 0; page < pages; page++) {
		req.list_index = page;
		memset(resp, 0, sizeof(*resp));
		mutex_lock(&ts->mutex);
		ret = qmi_ts_send_request(ts, QMI_TS_GET_SENSOR_LIST_REQ_V02,
			TS_GET_SENSOR_LIST_REQ_MSG_V02_MAX_MSG_LEN,
			ts_get_sensor_list_req_msg_v02_ei, &req,
			ts_get_sensor_list_resp_msg_v02_ei, resp, &resp->resp, generation);
		mutex_unlock(&ts->mutex);
		if (ret)
			goto out;
		if (!resp->sensor_list_valid || !resp->sensor_list_len ||
		    resp->sensor_list_len > ARRAY_SIZE(resp->sensor_list)) {
			ret = -EPROTO;
			goto out;
		}
		for (i = 0; i < resp->sensor_list_len; i++) {
			ret = qmi_ts_accept_sensor(ts, resp->sensor_list[i].sensor_id, generation);
			if (ret)
				goto out;
		}
	}
out:
	kfree(resp);
	return ret;
}

static void qmi_ts_svc_arrive(struct work_struct *work)
{
	struct qmi_ts_instance *ts = container_of(work, struct qmi_ts_instance, svc_arrive_work);
	int ret;

	if (atomic_read(&ts->controller->in_suspend))
		return;
	ret = verify_sensor_and_register(ts);
	if (ret)
		dev_dbg(ts->dev, "QMI sensor discovery failed: %d\n", ret);
}

static void thermal_qmi_del_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	struct qmi_ts_instance *ts = container_of(qmi, struct qmi_ts_instance, handle);
	struct qmi_sensor *sensor;

	spin_lock(&ts->state_lock);
	if (!service || (service->node == ts->server.sq_node &&
			 service->port == ts->server.sq_port)) {
		ts->online = false;
		ts->generation++;
		list_for_each_entry(sensor, &ts->ts_sensor_list, ts_node) {
			sensor->connection_active = false;
			sensor->trips_pending = true;
		}
	}
	spin_unlock(&ts->state_lock);
}

static void thermal_qmi_net_reset(struct qmi_handle *qmi)
{
	thermal_qmi_del_server(qmi, NULL);
}

static int thermal_qmi_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	struct qmi_ts_instance *ts = container_of(qmi, struct qmi_ts_instance, handle);
	struct qmi_sensor *sensor;
	int ret = 0;

	spin_lock(&ts->state_lock);
	if (ts->stopping) {
		ret = -ENODEV;
	} else {
		ts->server = (struct sockaddr_qrtr) {
			.sq_family = AF_QIPCRTR, .sq_node = service->node, .sq_port = service->port,
		};
		ts->generation++;
		ts->online = true;
		list_for_each_entry(sensor, &ts->ts_sensor_list, ts_node) {
			sensor->connection_active = false;
			sensor->trips_pending = true;
		}
		queue_work(system_highpri_wq, &ts->svc_arrive_work);
	}
	spin_unlock(&ts->state_lock);
	return ret;
}

static struct qmi_ops thermal_qmi_event_ops = {
	.new_server = thermal_qmi_new_server,
	.del_server = thermal_qmi_del_server,
	.net_reset = thermal_qmi_net_reset,
};

static int qmi_sensor_pm_notify(struct notifier_block *nb, unsigned long mode, void *unused)
{
	struct qmi_ts_controller *controller = container_of(nb, struct qmi_ts_controller, pm_nb);
	struct qmi_ts_instance *ts;
	unsigned int i;

	switch (mode) {
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
	case PM_SUSPEND_PREPARE:
		atomic_set(&controller->in_suspend, 1);
		for (i = 0; i < controller->count; i++) {
			mutex_lock(&controller->instances[i].mutex);
			mutex_unlock(&controller->instances[i].mutex);
		}
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		atomic_set(&controller->in_suspend, 0);
		for (i = 0; i < controller->count; i++) {
			ts = &controller->instances[i];
			spin_lock(&ts->state_lock);
			if (ts->online && !ts->stopping)
				queue_work(system_highpri_wq, &ts->svc_arrive_work);
			spin_unlock(&ts->state_lock);
		}
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static void qmi_ts_cleanup(struct qmi_ts_controller *controller)
{
	struct qmi_ts_instance *ts;
	struct qmi_sensor *sensor;
	unsigned int i;

	for (i = 0; i < controller->count; i++) {
		ts = &controller->instances[i];
		spin_lock(&ts->state_lock);
		ts->stopping = true;
		ts->online = false;
		ts->generation++;
		spin_unlock(&ts->state_lock);
	}
	if (controller->notifier_ready)
		unregister_pm_notifier(&controller->pm_nb);
	for (i = 0; i < controller->count; i++) {
		ts = &controller->instances[i];
		cancel_work_sync(&ts->svc_arrive_work);
		list_for_each_entry(sensor, &ts->ts_sensor_list, ts_node) {
			cancel_work_sync(&sensor->therm_notify_work);
			if (sensor->tz_dev)
				devm_thermal_of_zone_unregister(sensor->dev, sensor->tz_dev);
		}
		if (ts->handle_ready)
			qmi_handle_release(&ts->handle);
	}
}

static int qmi_sens_device_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qmi_ts_controller *controller;
	struct qmi_ts_instance *ts;
	struct qmi_sensor *sensor, *other;
	struct device_node *child;
	const char *name;
	unsigned int idx = 0, i, j;
	int count, names, n, ret;

	count = of_get_available_child_count(dev->of_node);
	if (!count)
		return -EINVAL;
	controller = devm_kzalloc(dev, sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return -ENOMEM;
	controller->instances = devm_kcalloc(dev, count, sizeof(*ts), GFP_KERNEL);
	if (!controller->instances)
		return -ENOMEM;
	controller->count = count;
	for (i = 0; i < count; i++) {
		ts = &controller->instances[i];
		ts->dev = dev;
		ts->controller = controller;
		mutex_init(&ts->mutex);
		spin_lock_init(&ts->state_lock);
		INIT_LIST_HEAD(&ts->ts_sensor_list);
		INIT_WORK(&ts->svc_arrive_work, qmi_ts_svc_arrive);
	}
	for_each_available_child_of_node(dev->of_node, child) {
		if (idx >= count) {
			ret = -EINVAL;
			goto put_child;
		}
		ts = &controller->instances[idx];
		ret = of_property_read_u32(child, "qcom,instance-id", &ts->inst_id);
		if (ret)
			goto put_child;
		if (ts->inst_id > INT_MAX - QMI_TS_MAX_NR) {
			ret = -ERANGE;
			goto put_child;
		}
		for (i = 0; i < idx; i++) {
			if (controller->instances[i].inst_id == ts->inst_id) {
				ret = -EINVAL;
				goto put_child;
			}
		}
		names = of_property_count_strings(child, "qcom,qmi-sensor-names");
		if (names <= 0 || names > QMI_TS_MAX_NR) {
			ret = -EINVAL;
			goto put_child;
		}
		for (n = 0; n < names; n++) {
			ret = of_property_read_string_index(child, "qcom,qmi-sensor-names",
							    n, &name);
			if (ret)
				goto put_child;
			for (i = 0; i < QMI_TS_MAX_NR; i++)
				if (!strcmp(sensor_clients[i], name))
					break;
			if (i == QMI_TS_MAX_NR) {
				ret = -EINVAL;
				goto put_child;
			}
			for (j = 0; j <= idx; j++) {
				list_for_each_entry(other, &controller->instances[j].ts_sensor_list,
						    ts_node) {
					if (other->sens_type + other->ts->inst_id ==
					    i + ts->inst_id) {
						ret = -EINVAL;
						goto put_child;
					}
				}
			}
			sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
			if (!sensor) {
				ret = -ENOMEM;
				goto put_child;
			}
			ret = strscpy(sensor->qmi_name, name, sizeof(sensor->qmi_name));
			if (ret < 0)
				goto put_child;
			sensor->dev = dev;
			sensor->ts = ts;
			sensor->sens_type = i;
			sensor->high_thresh = INT_MAX;
			sensor->low_thresh = -INT_MAX;
			sensor->trips_pending = true;
			INIT_WORK(&sensor->therm_notify_work, qmi_ts_thresh_notify);
			list_add_tail(&sensor->ts_node, &ts->ts_sensor_list);
		}
		idx++;
	}
	controller->pm_nb.notifier_call = qmi_sensor_pm_notify;
	ret = register_pm_notifier(&controller->pm_nb);
	if (ret)
		goto cleanup;
	controller->notifier_ready = true;
	for (i = 0; i < controller->count; i++) {
		ts = &controller->instances[i];
		ret = qmi_handle_init(&ts->handle, TS_GET_SENSOR_LIST_RESP_MSG_V02_MAX_MSG_LEN,
				      &thermal_qmi_event_ops, handlers);
		if (ret < 0)
			goto cleanup;
		ts->handle_ready = true;
		ret = qmi_add_lookup(&ts->handle, TS_SERVICE_ID_V02,
				     TS_SERVICE_VERS_V02, ts->inst_id);
		if (ret < 0)
			goto cleanup;
	}
	platform_set_drvdata(pdev, controller);
	return 0;
put_child:
	of_node_put(child);
cleanup:
	qmi_ts_cleanup(controller);
	return ret;
}

static void qmi_sens_device_remove(struct platform_device *pdev)
{
	qmi_ts_cleanup(platform_get_drvdata(pdev));
}

static const struct of_device_id qmi_sens_device_match[] = {
	{.compatible = "qcom,qmi-sensors"},
	{}
};

static struct platform_driver qmi_sens_device_driver = {
	.probe          = qmi_sens_device_probe,
	.remove         = qmi_sens_device_remove,
	.driver         = {
		.name   = QMI_SENS_DRIVER,
		.of_match_table = qmi_sens_device_match,
	},
};

module_platform_driver(qmi_sens_device_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QTI QMI thermal sensor v2 driver");
