// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "qcom_mpam_slc: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/qcom_scmi_vendor.h>
#include <linux/scmi_protocol.h>
#include <soc/qcom/mpam_msc.h>
#include <soc/qcom/mpam_slc.h>

#define QCOM_SLC_MPAM_SCMI_STR	0x534c434d50414d /* SLCMPAM */
#define SCMI_GET_PARAM_LEN	32

enum mpam_slc_set_param_ids {
	PARAM_SET_CACHE_PARTITION_MSC = 1,
	PARAM_RESET_CACHE_PARTITION_MSC = 2,
	PARAM_SET_CONFIG_MON_MSC = 3,
	PARAM_SET_CONFIG_SLC_MPAM_START_STOP = 4,
};

enum mpam_slc_get_param_ids {
	PARAM_GET_CLIENT_INFO_MSC = 1,
	PARAM_GET_CACHE_CAPABILITY_MSC = 2,
	PARAM_GET_CACHE_PARTITION_MSC = 3,
};

static uint16_t slc_client_id[] = {
	APPS,
	GPU,
	NSP,
	SLC_CLIENT_MAX,
};

static int mpam_msc_slc_set_params(struct device *dev, void *param, int param_len,
		uint32_t param_id)
{
	struct qcom_mpam_msc *qcom_msc;

	qcom_msc = (struct qcom_mpam_msc *)dev_get_drvdata(dev);
	if (qcom_msc == NULL)
		return -EINVAL;

	return qcom_msc->scmi_ops->set_param(qcom_msc->ph, param, QCOM_SLC_MPAM_SCMI_STR,
				param_id, param_len);
}

static int mpam_msc_slc_get_params(struct device *dev, void *param_in, int param_in_len,
		void *param_out, int param_out_len, uint32_t param_id)
{
	int ret = -EPERM;
	uint8_t buf[SCMI_GET_PARAM_LEN] = {};

	if (param_in_len <= 0 || param_in_len > sizeof(buf) ||
	    param_out_len <= 0 || param_out_len > sizeof(buf) ||
	    !param_in || !param_out)
		return -EINVAL;
	struct qcom_mpam_msc *qcom_msc;

	qcom_msc = (struct qcom_mpam_msc *)dev_get_drvdata(dev);
	if (qcom_msc == NULL)
		return ret;

	if (param_in_len && param_in_len <= SCMI_GET_PARAM_LEN) {
		memcpy(buf, param_in, param_in_len);
		ret = qcom_msc->scmi_ops->get_param(qcom_msc->ph, buf,
			QCOM_SLC_MPAM_SCMI_STR, param_id, param_in_len, param_out_len);
	}

	if (!ret)
		memcpy(param_out, buf, param_out_len);

	return ret;
}

static struct qcom_mpam_msc *slc_capability_check(struct device *dev, struct msc_query *query)
{
	struct qcom_mpam_msc *qcom_msc;
	struct qcom_slc_capability *slc_capability;
	struct slc_client_capability *slc_client_cap;
	int client_idx, partid_idx;

	client_idx = query->client_id;
	partid_idx = query->part_id;

	if (client_idx >= SLC_CLIENT_MAX) {
		dev_err(dev, "Invalid Client ID %d\n", client_idx);
		return NULL;
	}

	qcom_msc = (struct qcom_mpam_msc *)dev_get_drvdata(dev);
	if (qcom_msc == NULL)
		return NULL;

	slc_capability = (struct qcom_slc_capability *)qcom_msc->msc_capability;
	if (!slc_capability || client_idx >= slc_capability->num_clients)
		return NULL;
	slc_client_cap = &(slc_capability->slc_client_cap[client_idx]);

	if (slc_client_cap->enabled == false) {
		dev_err(dev, "Client not enabled for configuration %d\n", client_idx);
		return NULL;
	}

	if (partid_idx >= slc_client_cap->client_info.num_part_id) {
		dev_err(dev, "Invalid PART id %d\n", partid_idx);
		return NULL;
	}

	return qcom_msc;
}

static int slc_set_cache_partition(struct device *dev, void *msc_partid, void *msc_partconfig)
{
	struct qcom_mpam_msc *qcom_msc;
	struct slc_partid_config slc_part_config;
	struct qcom_slc_capability *slc_capability;
	struct slc_client_capability *slc_client_cap;
	struct slc_partid_capability *slc_partid_cap;
	int client_idx, partid_idx, gear_idx;

	memcpy(&slc_part_config.query, msc_partid, sizeof(struct msc_query));
	memcpy(&slc_part_config.gear_config, msc_partconfig, sizeof(struct qcom_slc_gear_val));

	qcom_msc = slc_capability_check(dev, &slc_part_config.query);
	if (qcom_msc == NULL)
		return -EINVAL;

	client_idx = slc_part_config.query.client_id;
	partid_idx = slc_part_config.query.part_id;
	slc_capability = (struct qcom_slc_capability *)qcom_msc->msc_capability;
	slc_client_cap = &(slc_capability->slc_client_cap[client_idx]);
	slc_partid_cap = &(slc_client_cap->slc_partid_cap[partid_idx]);

	for (gear_idx = 0; gear_idx < slc_partid_cap->num_gears; gear_idx++) {
		if (slc_partid_cap->part_id_gears[gear_idx] == slc_part_config.gear_config.gear_val)
			break;
	}

	if (gear_idx == slc_partid_cap->num_gears) {
		dev_err(dev, "GEAR config not valid!\n");
		return -EINVAL;
	}

	return mpam_msc_slc_set_params(dev, &slc_part_config, sizeof(struct slc_partid_config),
			PARAM_SET_CACHE_PARTITION_MSC);
}

static int slc_reset_cache_partition(struct device *dev, void *msc_partid, void *msc_partconfig)
{
	struct qcom_mpam_msc *qcom_msc;
	struct slc_partid_config slc_part_config = {0};

	memcpy(&slc_part_config.query, msc_partid, sizeof(struct msc_query));
	memcpy(&slc_part_config.gear_config, msc_partconfig, sizeof(struct qcom_slc_gear_val));

	qcom_msc = slc_capability_check(dev, &slc_part_config.query);
	if (qcom_msc == NULL)
		return -EINVAL;

	return mpam_msc_slc_set_params(dev, &slc_part_config, sizeof(struct slc_partid_config),
			PARAM_RESET_CACHE_PARTITION_MSC);
}

static int slc_client_query(struct device *dev, void *msc_partid, void *msc_partconfig)
{
	struct qcom_mpam_msc *qcom_msc;
	struct msc_query *query;
	struct slc_client_info *client_info;

	query = (struct msc_query *)msc_partid;
	client_info = (struct slc_client_info *)msc_partconfig;

	qcom_msc = (struct qcom_mpam_msc *)dev_get_drvdata(dev);
	if (qcom_msc == NULL)
		return -EINVAL;

	return mpam_msc_slc_get_params(dev, query, sizeof(struct msc_query), client_info,
			sizeof(struct slc_client_info), PARAM_GET_CLIENT_INFO_MSC);
}

static int slc_get_cache_partition(struct device *dev, void *msc_partid, void *msc_partconfig)
{
	struct qcom_mpam_msc *qcom_msc;
	struct msc_query *query;
	struct qcom_slc_capability *qcom_slc_capability;
	struct slc_client_capability *slc_client_cap;
	struct qcom_slc_gear_val *gear_config;

	query = (struct msc_query *)msc_partid;
	qcom_msc = slc_capability_check(dev, query);
	if (qcom_msc == NULL)
		return -EINVAL;

	gear_config = (struct qcom_slc_gear_val *)msc_partconfig;
	qcom_slc_capability = (struct qcom_slc_capability *)qcom_msc->msc_capability;
	slc_client_cap = &qcom_slc_capability->slc_client_cap[query->client_id];
	if (slc_client_cap->enabled == false)
		return -EINVAL;

	return mpam_msc_slc_get_params(dev, query, sizeof(struct msc_query), gear_config,
			sizeof(struct qcom_slc_gear_val), PARAM_GET_CACHE_PARTITION_MSC);
}

static int slc_get_cache_partition_capability(struct device *dev, void *msc_partid,
		void *msc_partconfig)
{
	struct qcom_mpam_msc *qcom_msc;
	struct msc_query *query;
	struct qcom_slc_capability *qcom_slc_capability;
	struct slc_partid_capability *slc_partid_capability;
	struct slc_client_capability *slc_client_cap;

	query = (struct msc_query *)msc_partid;
	qcom_msc = slc_capability_check(dev, query);
	if (qcom_msc == NULL)
		return -EINVAL;

	slc_partid_capability = (struct slc_partid_capability *) msc_partconfig;
	qcom_slc_capability = (struct qcom_slc_capability *)qcom_msc->msc_capability;
	slc_client_cap = &qcom_slc_capability->slc_client_cap[query->client_id];
	if (slc_client_cap->enabled == false)
		return -EINVAL;

	return mpam_msc_slc_get_params(dev, query, sizeof(struct msc_query), slc_partid_capability,
			sizeof(struct slc_partid_capability), PARAM_GET_CACHE_CAPABILITY_MSC);
}

static int mon_idx_lookup(void __iomem *mem, int client_id, int part_id)
{
	int index;
	for (index = 0; index < SLC_NUM_PARTIDS; index++) {
		void __iomem *record = mem + offsetof(struct qcom_slc_mon_mem, data) +
			index * sizeof(struct qcom_slc_mon_data);
		if (readl_relaxed(record) == client_id && readl_relaxed(record + 4) == part_id)
			return index;
	}
	return -EINVAL;
}

static int slc_mon_config(struct device *dev, void *msc_partid, void *msc_partconfig)
{
	struct msc_query *query = msc_partid;
	struct slc_mon_config_val *config = msc_partconfig;
	struct qcom_mpam_msc *msc = slc_capability_check(dev, query);
	struct qcom_slc_capability *caps;
	struct slc_mon_config request = {};
	unsigned long *enabled;
	u32 available;
	int index, ret;

	if (!msc || config->enable > 1 || config->slc_mon_function > CACHE_READ_MISS_CONFIG)
		return -EINVAL;
	index = mon_idx_lookup(msc->mon_base, query->client_id, query->part_id);
	if (index < 0)
		return index;
	caps = msc->msc_capability;
	if (config->slc_mon_function == CACHE_CAPACITY_CONFIG) {
		enabled = &caps->capacity_enabled;
		available = caps->slc_mon_list.capacity_config_available;
	} else {
		enabled = &caps->miss_enabled;
		available = caps->slc_mon_list.read_miss_config_available;
	}
	if (!!(*enabled & BIT(index)) == config->enable)
		return 0;
	if (config->enable && hweight_long(*enabled) >= available)
		return -ENOSPC;
	request.query = *query;
	request.config = *config;
	ret = mpam_msc_slc_set_params(dev, &request, sizeof(request), PARAM_SET_CONFIG_MON_MSC);
	if (!ret) {
		if (config->enable)
			*enabled |= BIT(index);
		else
			*enabled &= ~BIT(index);
	}
	return ret;
}

#define MAX_SHARED_MEM_RETRY_CNT 5000

static int slc_mon_stats_read(struct device *dev, void *msc_partid, void *data)
{
	struct msc_query *query = msc_partid;
	struct qcom_mpam_msc *msc = slc_capability_check(dev, query);
	union mon_values *value = data;
	void __iomem *record;
	u32 seq;
	int index, retry;

	if (!msc)
		return -EINVAL;
	index = mon_idx_lookup(msc->mon_base, query->client_id, query->part_id);
	if (index < 0)
		return index;
	record = msc->mon_base + offsetof(struct qcom_slc_mon_mem, data) +
		 index * sizeof(struct qcom_slc_mon_data);
	for (retry = 0; retry < MAX_SHARED_MEM_RETRY_CNT; retry++) {
		seq = readl_relaxed(msc->mon_base);
		if (seq & 1)
			continue;
		value->ref.mon_data = 0;
		if (value->ref.slc_mon_function == CACHE_CAPACITY_CONFIG &&
		    readl_relaxed(record + offsetof(struct qcom_slc_mon_data, cap_stats.cap_enabled)))
			value->capacity.num_cache_lines = readl_relaxed(record +
				offsetof(struct qcom_slc_mon_data, cap_stats.num_cache_lines));
		if (value->ref.slc_mon_function == CACHE_READ_MISS_CONFIG &&
		    readl_relaxed(record + offsetof(struct qcom_slc_mon_data, rd_miss_stats.miss_enabled)))
			value->misses.num_rd_misses = readq_relaxed(record +
				offsetof(struct qcom_slc_mon_data, rd_miss_stats.rd_misses));
		value->ref.last_capture_time = readq_relaxed(msc->mon_base +
			 offsetof(struct qcom_slc_mon_mem, last_capture_time));
		if (seq == readl_relaxed(msc->mon_base))
			return 0;
	}
	return -EAGAIN;
}

static struct mpam_msc_ops slc_msc_ops = {
	.set_cache_partition = slc_set_cache_partition,
	.get_cache_partition = slc_get_cache_partition,
	.get_cache_partition_capability = slc_get_cache_partition_capability,
	.reset_cache_partition = slc_reset_cache_partition,
	.mon_config = slc_mon_config,
	.mon_stats_read = slc_mon_stats_read,
};

static int slc_client_info_read(struct device *dev, struct device_node *node)
{
	int client_idx, partid_idx, ret = -EINVAL;
	struct msc_query query = {};
	struct qcom_mpam_msc *qcom_msc;
	struct slc_client_capability *slc_client_cap;
	struct qcom_slc_capability *qcom_slc_capability;

	qcom_msc = (struct qcom_mpam_msc *)dev_get_drvdata(dev);
	if (qcom_msc->qcom_msc_id.qcom_msc_type != SLC)
		return -EINVAL;

	qcom_slc_capability = (struct qcom_slc_capability *)qcom_msc->msc_capability;
	query.qcom_msc_id.qcom_msc_type = qcom_msc->qcom_msc_id.qcom_msc_type;
	query.qcom_msc_id.qcom_msc_class = qcom_msc->qcom_msc_id.qcom_msc_class;
	query.qcom_msc_id.idx = qcom_msc->qcom_msc_id.idx;
	for (client_idx = 0; client_idx < qcom_slc_capability->num_clients; client_idx++) {
		slc_client_cap = &(qcom_slc_capability->slc_client_cap[client_idx]);
		query.client_id = slc_client_id[client_idx];
		slc_client_cap->client_info.client_id = slc_client_id[client_idx];
		ret = of_property_read_string_index(node, "qcom,slc-clients", client_idx,
							&slc_client_cap->client_name);
		if (ret)
			return ret;
		slc_client_cap->enabled = false;
		if (slc_client_query(dev, &query, &(slc_client_cap->client_info)))
			continue;

		if ((slc_client_cap->client_info.num_part_id == 0) ||
				(slc_client_cap->client_info.num_part_id == SLC_INVALID_PARTID))
			continue;

		if (slc_client_cap->client_info.client_id != query.client_id ||
		    slc_client_cap->client_info.num_part_id > MAX_PART_ID)
			return -EPROTO;
		slc_client_cap->enabled = true;
		slc_client_cap->slc_partid_cap = devm_kcalloc(dev,
				slc_client_cap->client_info.num_part_id,
				sizeof(struct slc_partid_capability), GFP_KERNEL);
		if (slc_client_cap->slc_partid_cap == NULL) {
			ret = -ENOMEM;
			break;
		}

		for (partid_idx = 0; partid_idx < slc_client_cap->client_info.num_part_id;
				partid_idx++) {
			query.part_id = partid_idx;
			ret = slc_get_cache_partition_capability(dev, &query,
					&(slc_client_cap->slc_partid_cap[partid_idx]));
			if (ret)
				return ret;
			if (slc_client_cap->slc_partid_cap[partid_idx].num_gears > MAX_NUM_GEARS)
				return -EPROTO;
			for (int gear = 0; gear < slc_client_cap->slc_partid_cap[partid_idx].num_gears; gear++)
				if (slc_client_cap->slc_partid_cap[partid_idx].part_id_gears[gear] >= GEAR_MAX)
					return -EPROTO;
		}
	}

	return 0;
}

static int slc_mpam_enable_disable(struct device *dev, void *msc_partid, void *msc_partconfig)
{
	struct qcom_slc_mpam_enable_cfg mon_cfg = {0};

	memcpy(&mon_cfg.query, msc_partid, sizeof(struct msc_query));
	memcpy(&mon_cfg.enable, msc_partconfig, sizeof(struct mpam_enable));

	return mpam_msc_slc_set_params(dev, &mon_cfg, sizeof(struct qcom_slc_mpam_enable_cfg),
			PARAM_SET_CONFIG_SLC_MPAM_START_STOP);
}

static int slc_mpam_start_stop(struct device *dev, bool start)
{
	struct qcom_mpam_msc *qcom_msc;
	struct msc_query query = {0};
	struct mpam_enable enable = {0};

	qcom_msc = (struct qcom_mpam_msc *)dev_get_drvdata(dev);
	if (qcom_msc == NULL)
		return -EINVAL;

	query.qcom_msc_id.qcom_msc_type = qcom_msc->qcom_msc_id.qcom_msc_type;
	query.qcom_msc_id.qcom_msc_class = qcom_msc->qcom_msc_id.qcom_msc_class;
	query.qcom_msc_id.idx = qcom_msc->qcom_msc_id.idx;
	query.client_id = 0;
	query.part_id = 0;
	enable.value = start;
	return slc_mpam_enable_disable(dev, &query, &enable);
}

static void mpam_msc_slc_stop(void *data)
{
	struct device *dev = data;
	if (slc_mpam_start_stop(dev, false))
		dev_err(dev, "Failed to stop SLC monitor firmware thread\n");
}

static int mpam_msc_slc_probe(struct platform_device *pdev)
{
	int ret = -EINVAL, count;
	uint32_t val;
	struct device_node *node;
	struct qcom_mpam_msc *qcom_msc;
	struct qcom_slc_capability *qcom_slc_capability;
	struct resource *res;

	qcom_msc = devm_kzalloc(&pdev->dev, sizeof(struct qcom_mpam_msc), GFP_KERNEL);
	if (qcom_msc == NULL)
		return -ENOMEM;

	qcom_msc->mpam_available = MPAM_UNINITIALIZAED;
	qcom_msc->sdev = get_qcom_scmi_device();
	if (IS_ERR(qcom_msc->sdev))
		return PTR_ERR(qcom_msc->sdev);

	qcom_msc->scmi_ops = qcom_msc->sdev->handle->devm_protocol_get(qcom_msc->sdev,
			QCOM_SCMI_VENDOR_PROTOCOL, &qcom_msc->ph);
	if (IS_ERR(qcom_msc->scmi_ops))
		return dev_err_probe(&pdev->dev, PTR_ERR(qcom_msc->scmi_ops),
				"Error getting vendor protocol ops\n");

	qcom_msc->qcom_msc_id.qcom_msc_type = SLC;
	qcom_msc->qcom_msc_id.qcom_msc_class = CACHE_TYPE;
	qcom_msc->ops = &slc_msc_ops;
	qcom_msc->dev = &pdev->dev;

	node = pdev->dev.of_node;
	ret = of_property_read_u32(node, "qcom,dev-index", &val);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "qcom,dev-index Node failed!\n");

	if (val > U8_MAX)
		return dev_err_probe(&pdev->dev, -EINVAL, "dev-index wrong value failed!\n");

	qcom_msc->qcom_msc_id.idx = val;
	ret = of_property_read_string(node, "qcom,msc-name", &qcom_msc->msc_name);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "qcom,msc-name Node failed!\n");

	ret = of_property_read_u32(node, "qcom,msc-id", &qcom_msc->msc_id);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "qcom,msc-id Node failed!\n");

	platform_set_drvdata(pdev, qcom_msc);
	ret = slc_mpam_start_stop(&pdev->dev, true);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "MPAM SCMI failure!\n");
	ret = devm_add_action_or_reset(&pdev->dev, mpam_msc_slc_stop, &pdev->dev);
	if (ret)
		return ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mon-base");
	if (!res || resource_size(res) < sizeof(struct qcom_slc_mon_mem))
		return -EINVAL;
	qcom_msc->mon_base = devm_ioremap_resource(&pdev->dev, res);

	if (IS_ERR_OR_NULL(qcom_msc->mon_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(qcom_msc->mon_base),
				"ioremap slc mpam_mon_base failed!\n");

	if (readw_relaxed(qcom_msc->mon_base + offsetof(struct qcom_slc_mon_mem, msc_id)) != qcom_msc->msc_id)
		return dev_err_probe(&pdev->dev, -EINVAL, "IMEM Init failure!\n");

	qcom_msc->msc_capability = devm_kzalloc(&pdev->dev, sizeof(struct qcom_slc_capability),
			GFP_KERNEL);
	if (qcom_msc->msc_capability == NULL) {
		ret = -ENOMEM;
		goto err_probe;
	}

	qcom_slc_capability = (struct qcom_slc_capability *)qcom_msc->msc_capability;
	count = of_property_count_strings(node, "qcom,slc-clients");
	if (count <= 0 || count > SLC_CLIENT_MAX)
		return -EINVAL;
	qcom_slc_capability->num_clients = count;
	qcom_slc_capability->slc_client_cap = devm_kcalloc(&pdev->dev,
			qcom_slc_capability->num_clients, sizeof(struct  slc_client_capability),
			GFP_KERNEL);
	if (qcom_slc_capability->slc_client_cap == NULL) {
		ret = -ENOMEM;
		goto err_probe;
	}

	ret = of_property_read_u32(node, "qcom,num-read-miss-cfg", &val);
	if (ret)
		goto err_probe;

	if (val > SLC_NUM_PARTIDS)
		return -EINVAL;
	qcom_slc_capability->slc_mon_list.read_miss_config_available = val;
	ret = of_property_read_u32(node, "qcom,num-cap-cfg", &val);
	if (ret)
		goto err_probe;

	if (val > SLC_NUM_PARTIDS)
		return -EINVAL;
	qcom_slc_capability->slc_mon_list.capacity_config_available = val;

	qcom_slc_capability->slc_mon_configured.read_miss_configured = 0;
	qcom_slc_capability->slc_mon_configured.capacity_configured = 0;

	qcom_msc->mpam_available = MPAM_AVAILABLE;
	ret = slc_client_info_read(&pdev->dev, node);
	if (ret) {
		dev_err(&pdev->dev, "Failed to detect SLC device\n");
		goto err_probe;
	}

	qcom_msc->mpam_available = MPAM_MONITRS_AVAILABLE;
	ret = attach_mpam_msc(&pdev->dev, qcom_msc, SLC);
	if (!ret)
		return 0;
err_probe:
	return dev_err_probe(&pdev->dev, ret, "MPAM SLC driver probe failed!\n");
}

static void mpam_msc_slc_remove(struct platform_device *pdev)
{
	struct qcom_mpam_msc *msc = platform_get_drvdata(pdev);
	/* Withdraw before managed teardown stops firmware and frees capabilities. */
	detach_mpam_msc(&pdev->dev, msc, SLC);
}
static const struct of_device_id mpam_msc_slc_table[] = {
	{ .compatible = "qcom,slc-mpam" },
	{}
};
MODULE_DEVICE_TABLE(of, mpam_msc_slc_table);

static struct platform_driver mpam_msc_slc_driver = {
	.driver = {
		.name = "mpam-msc-slc",
		.of_match_table = mpam_msc_slc_table,
		.suppress_bind_attrs = true,
	},
	.probe = mpam_msc_slc_probe,
	.remove = mpam_msc_slc_remove,
};

module_platform_driver(mpam_msc_slc_driver);

MODULE_SOFTDEP("pre: llcc_qcom");
MODULE_SOFTDEP("pre: qcom_scmi_client");
MODULE_DESCRIPTION("QCOM MPAM MSC SLC driver");
MODULE_LICENSE("GPL");
