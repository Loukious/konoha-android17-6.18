/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QCOM_CORESIGHT_CSR_H
#define _QCOM_CORESIGHT_CSR_H
#include <linux/coresight.h>
int qcom_csr_attach(struct device *dev, struct coresight_platform_data *pdata,
                    struct coresight_device **helper);
int qcom_csr_set_byte_counter(struct coresight_device *csdev, u32 offset, u32 count);
int qcom_csr_route_ids(struct coresight_path *path, const u8 *ids, unsigned int count, bool enable);
#endif
