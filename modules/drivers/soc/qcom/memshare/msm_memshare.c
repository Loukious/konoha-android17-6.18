// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/err.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/notifier.h>
#include <linux/overflow.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/rpmsg/qcom_glink.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include "msm_memshare.h"
#include "heap_mem_ext_v01.h"

#include <soc/qcom/secure_buffer.h>
#include <trace/events/rproc_qcom.h>

/* Macros */
static unsigned long(attrs);

static struct qmi_handle *mem_share_svc_handle;
static uint64_t bootup_request;

/* Memshare Driver Structure */
struct memshare_driver {
	struct device *dev;
	struct mutex mem_share;
	void *ssr_notifier;
};

struct memshare_child {
	struct device *dev;
	int client_id;
	unsigned int index;
	struct qcom_glink_mem_entry *mem_entry;
};

static struct memshare_driver *memsh_drv;
static struct memshare_child *memsh_child[MAX_CLIENTS];
static struct mem_blocks memblock[MAX_CLIENTS];
static uint32_t num_clients;

static inline bool is_shared_mapping(struct mem_blocks *mb)
{
	if (!mb)
		return false;

	return mb->hyp_map_info.num_vmids > 1;
}

static int check_client(int client_id, int proc, int request)
{
	unsigned int i;

	for (i = 0; i < num_clients; i++)
		if (memsh_child[i] && memblock[i].client_id == client_id &&
		    memblock[i].peripheral == proc)
			return i;
	return DHMS_MEM_CLIENT_INVALID;
}

static void free_client(int id)
{
	memblock[id].phy_addr = 0;
	memblock[id].virtual_addr = 0;
	memblock[id].allotted = 0;
	memblock[id].size = 0;
	memblock[id].alloc_size = 0;
	memblock[id].hyp_mapping = 0;
	memblock[id].free_memory = 0;
	memblock[id].alloc_request = 0;
	memblock[id].sequence_id = -1;
	memblock[id].memory_type = MEMORY_CMA;
}

/* Never return pages to the allocator while a remote VM still owns them. */
static int memshare_reclaim(unsigned int index)
{
	struct mem_blocks *mb = &memblock[index];
	struct memshare_child *child = memsh_child[index];
	struct qcom_scm_vmperm dest = {
		.vmid = QCOM_SCM_VMID_HLOS,
		.perm = PERM_READ | PERM_WRITE | PERM_EXEC,
	};
	u64 source_vmids = 0;
	unsigned int j;
	int ret;

	if (!mb->allotted)
		return 0;
	if (mb->hyp_mapping) {
		for (j = 0; j < mb->hyp_map_info.num_vmids; j++)
			source_vmids |= BIT_ULL(mb->hyp_map_info.vmids[j]);
		ret = qcom_scm_assign_mem(mb->phy_addr, mb->size,
					  &source_vmids, &dest, 1);
		if (ret)
			return ret;
		mb->hyp_mapping = 0;
	}
	if (child->mem_entry) {
		qcom_glink_mem_entry_free(child->mem_entry);
		child->mem_entry = NULL;
	}
	dma_free_attrs(child->dev, mb->alloc_size, mb->virtual_addr,
		       mb->phy_addr, attrs);
	free_client(index);
	return 0;
}

static void fill_alloc_response(struct mem_alloc_generic_resp_msg_v01 *resp,
						int id, int *flag)
{
	resp->sequence_id_valid = 1;
	resp->sequence_id = memblock[id].sequence_id;
	resp->dhms_mem_alloc_addr_info_valid = 1;
	resp->dhms_mem_alloc_addr_info_len = 1;
	resp->dhms_mem_alloc_addr_info[0].phy_addr = memblock[id].phy_addr;
	resp->dhms_mem_alloc_addr_info[0].num_bytes = memblock[id].size;
	if (!*flag) {
		resp->resp.result = QMI_RESULT_SUCCESS_V01;
		resp->resp.error = QMI_ERR_NONE_V01;
	} else {
		resp->resp.result = QMI_RESULT_FAILURE_V01;
		resp->resp.error = QMI_ERR_NO_MEMORY_V01;
	}
}

static void initialize_client(void)
{
	int i;

	for (i = 0; i < MAX_CLIENTS; i++) {
		memblock[i].allotted = 0;
		memblock[i].size = 0;
		memblock[i].guarantee = 0;
		memblock[i].phy_addr = 0;
		memblock[i].virtual_addr = 0;
		memblock[i].client_id = DHMS_MEM_CLIENT_INVALID;
		memblock[i].peripheral = -1;
		memblock[i].sequence_id = -1;
		memblock[i].memory_type = MEMORY_CMA;
		memblock[i].free_memory = 0;
		memblock[i].hyp_mapping = 0;
	}
}

static int modem_notifier_cb(struct notifier_block *this, unsigned long code,
			     void *data)
{
	unsigned int i;
	int ret;

	mutex_lock(&memsh_drv->mem_share);
	if (code == QCOM_SSR_BEFORE_SHUTDOWN) {
		bootup_request++;
		for (i = 0; i < num_clients; i++)
			memblock[i].alloc_request = 0;
	} else if (code == QCOM_SSR_AFTER_POWERUP) {
		for (i = 0; i < num_clients; i++) {
			struct mem_blocks *mb = &memblock[i];

			if (mb->free_memory && bootup_request >= 2)
				mb->free_memory--;
			if (!mb->free_memory && mb->peripheral == DHMS_MEM_PROC_MPSS_V01 &&
			    !mb->guarantee && !mb->client_request && mb->allotted &&
			    !mb->alloc_request) {
				ret = memshare_reclaim(i);
				if (ret)
					dev_err(memsh_drv->dev, "client %u reclaim failed: %d\n", i, ret);
			}
		}
		bootup_request++;
	}
	mutex_unlock(&memsh_drv->mem_share);
	return NOTIFY_OK;
}

static struct notifier_block nb = {
	.notifier_call = modem_notifier_cb,
};

static int shared_hyp_mapping(unsigned int index)
{
	struct mem_blocks *mb = &memblock[index];
	struct qcom_scm_vmperm dest[2];
	u64 source_vmids = BIT_ULL(QCOM_SCM_VMID_HLOS);
	unsigned int j;
	int ret;

	if (mb->hyp_mapping)
		return 0;
	for (j = 0; j < mb->hyp_map_info.num_vmids; j++) {
		dest[j].vmid = mb->hyp_map_info.vmids[j];
		dest[j].perm = mb->hyp_map_info.perms[j];
	}
	ret = qcom_scm_assign_mem(mb->phy_addr, mb->size, &source_vmids,
				  dest, mb->hyp_map_info.num_vmids);
	if (!ret)
		mb->hyp_mapping = 1;
	return ret;
}

static void handle_alloc_generic_req(struct qmi_handle *handle,
	struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded_msg)
{
	const struct mem_alloc_generic_req_msg_v01 *req = decoded_msg;
	struct mem_alloc_generic_resp_msg_v01 resp = { };
	struct memshare_child *child;
	struct mem_blocks *mb;
	unsigned int usable, size;
	int index, ret, failed = 0;

	resp.resp.result = QMI_RESULT_FAILURE_V01;
	resp.resp.error = QMI_ERR_NO_MEMORY_V01;
	mutex_lock(&memsh_drv->mem_share);
	index = check_client(req->client_id, req->proc_id, CHECK);
	if (index >= MAX_CLIENTS || !req->num_bytes)
		goto out;
	child = memsh_child[index];
	mb = &memblock[index];
	if (!mb->allotted) {
		usable = min(req->num_bytes, mb->init_size);
		if (!usable || check_add_overflow(usable,
				mb->guard_band ? MEMSHARE_GUARD_BYTES : 0U, &size))
			goto out;
		ret = memshare_alloc(child->dev, size, mb);
		if (ret)
			goto out;
		mb->size = usable;
		mb->allotted = 1;
		mb->free_memory = 1;
	}
	ret = shared_hyp_mapping(index);
	if (ret) {
		/* Assignment failure leaves the allocation owned by HLOS. */
		memshare_reclaim(index);
		goto out;
	}
	if (is_shared_mapping(mb) && !child->mem_entry) {
		child->mem_entry = qcom_glink_mem_entry_init(child->dev,
			mb->virtual_addr, mb->phy_addr, mb->size, mb->phy_addr);
		if (IS_ERR_OR_NULL(child->mem_entry)) {
			child->mem_entry = NULL;
			memshare_reclaim(index);
			goto out;
		}
	}
	mb->sequence_id = req->sequence_id;
	mb->alloc_request = 1;
	fill_alloc_response(&resp, index, &failed);
out:
	mutex_unlock(&memsh_drv->mem_share);
	ret = qmi_send_response(handle, sq, txn, MEM_ALLOC_GENERIC_RESP_MSG_V01,
		MEM_ALLOC_REQ_MAX_MSG_LEN_V01, mem_alloc_generic_resp_msg_data_v01_ei, &resp);
	if (ret < 0)
		dev_err(memsh_drv->dev, "allocation response failed: %d\n", ret);
}

static void handle_free_generic_req(struct qmi_handle *handle,
	struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded_msg)
{
	const struct mem_free_generic_req_msg_v01 *req = decoded_msg;
	struct mem_free_generic_resp_msg_v01 resp = { };
	int index, ret;

	resp.resp.result = QMI_RESULT_FAILURE_V01;
	resp.resp.error = QMI_ERR_INTERNAL_V01;
	mutex_lock(&memsh_drv->mem_share);
	index = check_client(req->client_id, req->proc_id, FREE);
	if (index >= MAX_CLIENTS || memblock[index].guarantee || memblock[index].client_request)
		goto out;
	ret = memshare_reclaim(index);
	if (!ret) {
		resp.resp.result = QMI_RESULT_SUCCESS_V01;
		resp.resp.error = QMI_ERR_NONE_V01;
	}
out:
	mutex_unlock(&memsh_drv->mem_share);
	ret = qmi_send_response(handle, sq, txn, MEM_FREE_GENERIC_RESP_MSG_V01,
		MEM_FREE_REQ_MAX_MSG_LEN_V01, mem_free_generic_resp_msg_data_v01_ei, &resp);
	if (ret < 0)
		dev_err(memsh_drv->dev, "free response failed: %d\n", ret);
}

static void handle_query_size_req(struct qmi_handle *handle,
	struct sockaddr_qrtr *sq, struct qmi_txn *txn, const void *decoded_msg)
{
	int rc, index = DHMS_MEM_CLIENT_INVALID;
	struct mem_query_size_req_msg_v01 *query_req;
	struct mem_query_size_rsp_msg_v01 *query_resp;

	mutex_lock(&memsh_drv->mem_share);
	query_req = (struct mem_query_size_req_msg_v01 *)decoded_msg;
	query_resp = kzalloc(sizeof(*query_resp),
					GFP_KERNEL);
	if (!query_resp) {
		mutex_unlock(&memsh_drv->mem_share);
		return;
	}
	query_resp->resp.result = QMI_RESULT_FAILURE_V01;
	query_resp->resp.error = QMI_ERR_INTERNAL_V01;

	dev_dbg(memsh_drv->dev,
		"memshare_query: query on available memory size for client id: %d, proc_id: %d\n",
		query_req->client_id, query_req->proc_id);
	index = check_client(query_req->client_id, query_req->proc_id,
								CHECK);

	if (index >= MAX_CLIENTS) {
		dev_err(memsh_drv->dev,
			"memshare_query: client not found, requested client: %d, proc_id: %d\n",
			query_req->client_id, query_req->proc_id);
		mutex_unlock(&memsh_drv->mem_share);
		goto resp_fill;
	}

	if (memblock[index].init_size) {
		query_resp->size_valid = 1;
		query_resp->size = memblock[index].init_size;
	} else {
		query_resp->size_valid = 1;
		query_resp->size = 0;
	}
	query_resp->resp.result = QMI_RESULT_SUCCESS_V01;
	query_resp->resp.error = QMI_ERR_NONE_V01;
	mutex_unlock(&memsh_drv->mem_share);

resp_fill:
	dev_info(memsh_drv->dev,
		"memshare_query: client_id : %d, query_resp.size :%d, query_resp.resp.result :%lx\n",
		query_req->client_id, query_resp->size,
		(unsigned long)query_resp->resp.result);
	rc = qmi_send_response(mem_share_svc_handle, sq, txn,
			  MEM_QUERY_SIZE_RESP_MSG_V01,
			  MEM_QUERY_MAX_MSG_LEN_V01,
			  mem_query_size_resp_msg_data_v01_ei, query_resp);
	if (rc < 0)
		dev_err(memsh_drv->dev,
		"memshare_query: Error sending the query response: %d\n", rc);

	kfree(query_resp);
	query_resp = NULL;
}

static void mem_share_svc_disconnect_cb(struct qmi_handle *qmi,
				  unsigned int node, unsigned int port)
{
}

static struct qmi_ops server_ops = {
	.del_client = mem_share_svc_disconnect_cb,
};

static struct qmi_msg_handler qmi_memshare_handlers[] = {
	{
		.type = QMI_REQUEST,
		.msg_id = MEM_ALLOC_GENERIC_REQ_MSG_V01,
		.ei = mem_alloc_generic_req_msg_data_v01_ei,
		.decoded_size = sizeof(struct mem_alloc_generic_req_msg_v01),
		.fn = handle_alloc_generic_req,
	},
	{
		.type = QMI_REQUEST,
		.msg_id = MEM_FREE_GENERIC_REQ_MSG_V01,
		.ei = mem_free_generic_req_msg_data_v01_ei,
		.decoded_size = sizeof(struct mem_free_generic_req_msg_v01),
		.fn = handle_free_generic_req,
	},
	{
		.type = QMI_REQUEST,
		.msg_id = MEM_QUERY_SIZE_REQ_MSG_V01,
		.ei = mem_query_size_req_msg_data_v01_ei,
		.decoded_size = sizeof(struct mem_query_size_req_msg_v01),
		.fn = handle_query_size_req,
	},
	{}
};

int memshare_alloc(struct device *dev,
		   unsigned int block_size,
		   struct mem_blocks *pblk)
{
	dev_dbg(memsh_drv->dev,
		"memshare: allocation request for size: %d", block_size);

	if (!pblk) {
		dev_err(memsh_drv->dev,
			"memshare: Failed memory block allocation\n");
		return -ENOMEM;
	}

	pblk->virtual_addr = dma_alloc_attrs(dev, block_size,
						&pblk->phy_addr, GFP_KERNEL,
						attrs);
	if (pblk->virtual_addr == NULL)
		return -ENOMEM;

	pblk->alloc_size = block_size;
	return 0;
}

static int memshare_start_service(void)
{
	int ret;

	mem_share_svc_handle = kzalloc(sizeof(*mem_share_svc_handle), GFP_KERNEL);
	if (!mem_share_svc_handle)
		return -ENOMEM;
	ret = qmi_handle_init(mem_share_svc_handle, MEM_ALLOC_REQ_MAX_MSG_LEN_V01,
			      &server_ops, qmi_memshare_handlers);
	if (ret)
		goto free_handle;
	ret = qmi_add_server(mem_share_svc_handle, MEM_SHARE_SERVICE_SVC_ID,
			     MEM_SHARE_SERVICE_VERS, MEM_SHARE_SERVICE_INS_ID);
	if (!ret)
		return 0;
	qmi_handle_release(mem_share_svc_handle);
free_handle:
	kfree(mem_share_svc_handle);
	mem_share_svc_handle = NULL;
	return ret;
}

static int memshare_child_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct memshare_child *child;
	struct mem_blocks *mb;
	const char *label;
	u32 size, client_id;
	unsigned int alloc_size, index;
	int ret;

	if (!memsh_drv)
		return -EPROBE_DEFER;
	guard(mutex)(&memsh_drv->mem_share);
	index = num_clients;
	if (index >= MAX_CLIENTS)
		return -ENOSPC;
	ret = of_property_read_u32(dev->of_node, "qcom,peripheral-size", &size);
	if (ret)
		return ret;
	ret = of_property_read_u32(dev->of_node, "qcom,client-id", &client_id);
	if (ret)
		return ret;
	ret = of_property_read_string(dev->of_node, "label", &label);
	if (ret)
		return ret;
	child = devm_kzalloc(dev, sizeof(*child), GFP_KERNEL);
	if (!child)
		return -ENOMEM;
	child->dev = dev;
	child->client_id = client_id;
	child->index = index;
	mb = &memblock[index];
	if (!strcmp(label, "modem"))
		mb->peripheral = DHMS_MEM_PROC_MPSS_V01;
	else if (!strcmp(label, "adsp"))
		mb->peripheral = DHMS_MEM_PROC_ADSP_V01;
	else if (!strcmp(label, "wcnss"))
		mb->peripheral = DHMS_MEM_PROC_WCNSS_V01;
	else
		return -EINVAL;
	mb->client_id = client_id;
	mb->init_size = size;
	mb->guarantee = of_property_read_bool(dev->of_node, "qcom,allocate-boot-time");
	mb->client_request = of_property_read_bool(dev->of_node, "qcom,allocate-on-request");
	mb->guard_band = of_property_read_bool(dev->of_node, "qcom,guard-band");
	mb->hyp_map_info.num_vmids = 1;
	mb->hyp_map_info.vmids[0] = VMID_MSS_MSA;
	mb->hyp_map_info.perms[0] = PERM_READ | PERM_WRITE;
	if (of_property_read_bool(dev->of_node, "qcom,shared")) {
		mb->hyp_map_info.num_vmids = 2;
		mb->hyp_map_info.vmids[1] = VMID_HLOS;
		mb->hyp_map_info.perms[1] = PERM_READ | PERM_WRITE;
	}
	if (of_find_property(dev->of_node, "memory-region", NULL)) {
		ret = of_reserved_mem_device_init(dev);
		if (ret)
			return ret;
	}
	memsh_child[index] = child;
	if (mb->guarantee && size) {
		if (check_add_overflow(size, mb->guard_band ? MEMSHARE_GUARD_BYTES : 0U,
				       &alloc_size)) {
			ret = -EOVERFLOW;
			goto release_region;
		}
		ret = memshare_alloc(dev, alloc_size, mb);
		if (ret)
			goto release_region;
		mb->allotted = 1;
		mb->size = size;
		ret = shared_hyp_mapping(index);
		if (ret) {
			memshare_reclaim(index);
			goto release_region;
		}
	}
	platform_set_drvdata(pdev, child);
	num_clients++;
	return 0;
release_region:
	memsh_child[index] = NULL;
	of_reserved_mem_device_release(dev);
	return ret;
}

static void memshare_remove(struct platform_device *pdev)
{
	struct memshare_driver *drv;
	struct memshare_child *child;
	int ret;

	if (of_device_is_compatible(pdev->dev.of_node, "qcom,memshare-peripheral")) {
		child = platform_get_drvdata(pdev);
		ret = memshare_reclaim(child->index);
		if (ret)
			dev_err(child->dev, "secure memory retained after reclaim failure: %d\n", ret);
		memsh_child[child->index] = NULL;
		of_reserved_mem_device_release(child->dev);
		return;
	}
	drv = platform_get_drvdata(pdev);
	if (mem_share_svc_handle) {
		qmi_handle_release(mem_share_svc_handle);
		kfree(mem_share_svc_handle);
		mem_share_svc_handle = NULL;
	}
	if (drv->ssr_notifier)
		qcom_unregister_ssr_notifier(drv->ssr_notifier, &nb);
	of_platform_depopulate(&pdev->dev);
	memsh_drv = NULL;
	num_clients = 0;
}

static int memshare_probe(struct platform_device *pdev)
{
	struct memshare_driver *drv;
	int ret;

	if (of_device_is_compatible(pdev->dev.of_node, "qcom,memshare-peripheral"))
		return memshare_child_probe(pdev);
	if (memsh_drv)
		return -EBUSY;
	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;
	drv->dev = &pdev->dev;
	mutex_init(&drv->mem_share);
	initialize_client();
	num_clients = 0;
	bootup_request = 0;
	memsh_drv = drv;
	platform_set_drvdata(pdev, drv);
	ret = of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
	if (ret)
		goto cleanup;
	drv->ssr_notifier = qcom_register_ssr_notifier("modem", &nb);
	if (IS_ERR(drv->ssr_notifier)) {
		ret = PTR_ERR(drv->ssr_notifier);
		drv->ssr_notifier = NULL;
		goto cleanup;
	}
	ret = memshare_start_service();
	if (!ret)
		return 0;
cleanup:
	memshare_remove(pdev);
	return ret;
}

static const struct of_device_id memshare_match_table[] = {
	{ .compatible = "qcom,memshare", },
	{ .compatible = "qcom,memshare-peripheral", },
	{}
};

MODULE_DEVICE_TABLE(of, memshare_match_table);

static struct platform_driver memshare_pdriver = {
	.probe          = memshare_probe,
	.remove         = memshare_remove,
	.driver = {
		.name   = "memshare",
		.of_match_table = memshare_match_table,
		.suppress_bind_attrs = true,
	},
};

module_platform_driver(memshare_pdriver);

MODULE_DESCRIPTION("Mem Share QMI Service Driver");
MODULE_LICENSE("GPL");
