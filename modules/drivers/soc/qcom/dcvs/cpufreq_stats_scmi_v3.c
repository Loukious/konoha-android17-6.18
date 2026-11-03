// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved. */
#include <linux/scmi_protocol.h>
#include "../../../firmware/arm_scmi/protocols.h"
#include "cpufreq_stats_common.h"

/* Standard SCMI PERF PROTOCOL_ATTRIBUTES command and pinned 6.18 response.
 * Query the real protocol instead of interpreting a private provider structure.
 */
#define PERF_PROTOCOL_ATTRIBUTES 1
struct scmi_perf_stats_attributes {
	__le16 num_domains;
	__le16 flags;
	__le32 stats_addr_low;
	__le32 stats_addr_high;
	__le32 stats_size;
};

static int cpufreq_stats_perf_metadata(const struct scmi_protocol_handle *ph,
				       u64 *address, u32 *size)
{
	struct scmi_perf_stats_attributes attributes;
	struct scmi_xfer *transfer;
	int ret;

	if (!ph || !ph->xops || !ph->xops->xfer_get_init ||
	    !ph->xops->do_xfer || !ph->xops->xfer_put)
		return -EOPNOTSUPP;
	ret = ph->xops->xfer_get_init(ph, PERF_PROTOCOL_ATTRIBUTES, 0,
				    sizeof(attributes), &transfer);
	if (ret)
		return ret;
	ret = ph->xops->do_xfer(ph, transfer);
	if (!ret) {
		if (transfer->rx.len < sizeof(attributes)) {
			ret = -EPROTO;
		} else {
			memcpy(&attributes, transfer->rx.buf, sizeof(attributes));
			*address = le32_to_cpu(attributes.stats_addr_low) |
				   (u64)le32_to_cpu(attributes.stats_addr_high) << 32;
			*size = le32_to_cpu(attributes.stats_size);
			if (!*size)
				ret = -ENODATA;
		}
	}
	ph->xops->xfer_put(ph, transfer);
	return ret;
}

static int scmi_cpufreq_stats_probe(struct scmi_device *sdev)
{
	const struct scmi_perf_proto_ops *ops;
	struct scmi_protocol_handle *ph;
	u64 address;
	u32 size;
	int ret;

	if (!sdev->handle)
		return -ENODEV;
	ops = sdev->handle->devm_protocol_get(sdev, SCMI_PROTOCOL_PERF, &ph);
	if (IS_ERR(ops))
		return PTR_ERR(ops);
	ret = cpufreq_stats_perf_metadata(ph, &address, &size);
	if (ret)
		return dev_err_probe(&sdev->dev, ret, "PERF statistics metadata unavailable\n");
	return cpufreq_stats_map(&sdev->dev, address, size);
}
static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_PERF, "cpufreqstats" },
	{ }
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);
static struct scmi_driver scmi_cpufreq_stats_driver = {
	.name = "scmi-cpufreq-stats",
	.probe = scmi_cpufreq_stats_probe,
	.id_table = scmi_id_table,
};
module_scmi_driver(scmi_cpufreq_stats_driver);
MODULE_DESCRIPTION("QTI SCMI CPUFREQ STATS driver");
MODULE_LICENSE("GPL");
