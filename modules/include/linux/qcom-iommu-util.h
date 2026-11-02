/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __QCOM_IOMMU_UTIL_H
#define __QCOM_IOMMU_UTIL_H

#include <linux/iommu.h>
#include <linux/dma-mapping.h>
#include <linux/iova.h>

#include <soc/qcom/secure_buffer.h>

/* IOMMU fault behaviors */
#define QCOM_IOMMU_FAULT_MODEL_NON_FATAL	BIT(0)
#define QCOM_IOMMU_FAULT_MODEL_NO_CFRE		BIT(1)
#define QCOM_IOMMU_FAULT_MODEL_NO_STALL		BIT(2)
#define QCOM_IOMMU_FAULT_MODEL_HUPCF		BIT(3)

/* IOMMU mapping configurations */
#define QCOM_IOMMU_MAPPING_CONF_S1_BYPASS	BIT(0)
#define QCOM_IOMMU_MAPPING_CONF_ATOMIC		BIT(1)
#define QCOM_IOMMU_MAPPING_CONF_FAST		BIT(2)

/* iommu transaction flags */
/* 1 Write, 0 Read */
#define QCOM_IOMMU_ATOS_TRANS_WRITE	BIT(0)
/* 1 Privileged, 0 Unprivileged */
#define QCOM_IOMMU_ATOS_TRANS_PRIV	BIT(1)
/* 1 Instruction fetch, 0 Data access */
#define QCOM_IOMMU_ATOS_TRANS_INST	BIT(2)
/* Non secure unprivileged Data read operation */
#define QCOM_IOMMU_ATOS_TRANS_DEFAULT	(0U)

#ifndef IOMMU_SYS_CACHE
/* Attributes are not supported, so render them ineffective. */
#define IOMMU_SYS_CACHE		(0)
#define IOMMU_SYS_CACHE_NWA	(0)
#endif

/* vendor iommu fault flags */
#define IOMMU_FAULT_TRANSLATION         (1 << 2)
#define IOMMU_FAULT_PERMISSION          (1 << 3)
#define IOMMU_FAULT_EXTERNAL            (1 << 4)
#define IOMMU_FAULT_TRANSACTION_STALLED (1 << 5)

/* iommu transaction flags */
#define IOMMU_TRANS_WRITE	BIT(0)	/* 1 Write, 0 Read */
#define IOMMU_TRANS_PRIV	BIT(1)	/* 1 Privileged, 0 Unprivileged */
#define IOMMU_TRANS_INST	BIT(2)	/* 1 Instruction fetch, 0 Data access */
#define IOMMU_TRANS_SEC	BIT(3)	/* 1 Secure, 0 Non-secure access*/

/* Non secure unprivileged Data read operation */
#define IOMMU_TRANS_DEFAULT	(0U)

typedef void (*fault_handler_irq_t)(struct iommu_domain *, void *);

struct iommu_pgtbl_info {
	void *ops;
};

struct qcom_iommu_atos_txn {
	u64 addr;
	u32 flags;
	u32 id;
};

enum sid_switch_direction {
	SID_ACQUIRE,
	SID_RELEASE,
};

struct qcom_iommu_fault_ids {
	u32 bid;
	u32 pid;
	u32 mid;
};

/*
 * @sid_switch: add/remove all SIDS in the iommu domain containing dev from
 *              iommu registers.
 */
struct qcom_iommu_ops {
	phys_addr_t (*iova_to_phys_hard)(struct iommu_domain *domain,
					struct qcom_iommu_atos_txn *txn);
	int (*sid_switch)(struct device *dev, enum sid_switch_direction dir);
	int (*get_fault_ids)(struct iommu_domain *domain,
			struct qcom_iommu_fault_ids *ids);
	int (*get_context_bank_nr)(struct iommu_domain *domain);
	int (*get_asid_nr)(struct iommu_domain *domain);
	int (*set_secure_vmid)(struct iommu_domain *domain, enum vmid vmid);
	int (*set_fault_model)(struct iommu_domain *domain, int fault_model);
	void (*set_fault_handler)(struct iommu_domain *domain,
				  iommu_fault_handler_t handler, void *token);
	void (*set_fault_handler_irq)(struct iommu_domain *domain,
			fault_handler_irq_t handler_irq, void *token);
	void (*register_device_fault_handler_irq)(struct device *dev,
			fault_handler_irq_t handler, void *token);
	int (*enable_s1_translation)(struct iommu_domain *domain);
	int (*get_mappings_configuration)(struct iommu_domain *domain);
	void (*skip_tlb_management)(struct iommu_domain *domain, bool skip);
	struct iommu_ops iommu_ops;
	struct iommu_domain_ops domain_ops;
};

int register_qcom_iommu_ops(struct qcom_iommu_ops *ops);
void unregister_qcom_iommu_ops(struct qcom_iommu_ops *ops);

/* Preserve the 6.6 unrecoverable-fault payload outside the 6.18 IOPF API. */
#define QCOM_IOMMU_FAULT_DMA_UNRECOV 1

enum qcom_iommu_fault_reason {
	QCOM_IOMMU_FAULT_REASON_UNKNOWN = 0,

	/* Could not access the PASID table (fetch caused external abort) */
	QCOM_IOMMU_FAULT_REASON_PASID_FETCH,

	/* PASID entry is invalid or has configuration errors */
	QCOM_IOMMU_FAULT_REASON_BAD_PASID_ENTRY,

	/*
	 * PASID is out of range (e.g. exceeds the maximum PASID
	 * supported by the IOMMU) or disabled.
	 */
	QCOM_IOMMU_FAULT_REASON_PASID_INVALID,

	/*
	 * An external abort occurred fetching (or updating) a translation
	 * table descriptor
	 */
	QCOM_IOMMU_FAULT_REASON_WALK_EABT,

	/*
	 * Could not access the page table entry (Bad address),
	 * actual translation fault
	 */
	QCOM_IOMMU_FAULT_REASON_PTE_FETCH,

	/* Protection flag check failed */
	QCOM_IOMMU_FAULT_REASON_PERMISSION,

	/* access flag check failed */
	QCOM_IOMMU_FAULT_REASON_ACCESS,

	/* Output address of a translation stage caused Address Size fault */
	QCOM_IOMMU_FAULT_REASON_OOR_ADDRESS,
};

/**
 * struct qcom_iommu_fault_unrecoverable - Unrecoverable fault data
 * @reason: reason of the fault, from &enum qcom_iommu_fault_reason
 * @flags: parameters of this fault (QCOM_IOMMU_FAULT_UNRECOV_* values)
 * @pasid: Process Address Space ID
 * @perm: requested permission access using by the incoming transaction
 *        (IOMMU_FAULT_PERM_* values)
 * @addr: offending page address
 * @fetch_addr: address that caused a fetch abort, if any
 */
struct qcom_iommu_fault_unrecoverable {
	__u32	reason;
#define QCOM_IOMMU_FAULT_UNRECOV_PASID_VALID		BIT(0)
#define QCOM_IOMMU_FAULT_UNRECOV_ADDR_VALID		BIT(1)
#define QCOM_IOMMU_FAULT_UNRECOV_FETCH_ADDR_VALID	BIT(2)
	__u32	flags;
	__u32	pasid;
	__u32	perm;
	__u64	addr;
	__u64	fetch_addr;
};

struct qcom_iommu_fault {
	__u32 type;
	__u32 padding;
	union {
		struct qcom_iommu_fault_unrecoverable event;
		__u8 padding2[56];
	};
};

typedef int (*qcom_iommu_device_fault_handler_t)(struct qcom_iommu_fault *fault,
						 void *data);
int qcom_iommu_register_device_fault_handler(struct device *dev,
					     qcom_iommu_device_fault_handler_t handler, void *data);
int qcom_iommu_unregister_device_fault_handler(struct device *dev);
int qcom_iommu_report_device_fault(struct device *dev, struct qcom_iommu_fault *fault);

struct device_node *qcom_iommu_group_parse_phandle(struct device *dev);
int qcom_iommu_generate_dma_regions(struct device *dev,
				    struct list_head *head);
void qcom_iommu_generate_resv_regions(struct device *dev,
				      struct list_head *list);
int qcom_iommu_get_fast_iova_range(struct device *dev,
				   dma_addr_t *ret_iova_base,
				   dma_addr_t *ret_iova_end);

/* Remove once this function is exported by upstream kernel */
void qcom_iommu_get_resv_regions(struct device *dev, struct list_head *list);

phys_addr_t qcom_iommu_iova_to_phys_hard(struct iommu_domain *domain,
				    struct qcom_iommu_atos_txn *txn);

int qcom_iommu_sid_switch(struct device *dev, enum sid_switch_direction dir);

int qcom_skip_tlb_management(struct device *dev, bool skip);

extern int qcom_iommu_get_fault_ids(struct iommu_domain *domain,
				struct qcom_iommu_fault_ids *f_ids);
extern int qcom_iommu_get_msi_size(struct device *dev, u32 *msi_size);

int qcom_iommu_get_context_bank_nr(struct iommu_domain *domain);

int qcom_iommu_get_asid_nr(struct iommu_domain *domain);

int qcom_iommu_set_secure_vmid(struct iommu_domain *domain, enum vmid vmid);

int qcom_iommu_set_fault_model(struct iommu_domain *domain, int fault_model);

int qcom_iommu_set_fault_handler(struct iommu_domain *domain,
				 iommu_fault_handler_t handler, void *token);

int qcom_iommu_set_fault_handler_irq(struct iommu_domain *domain,
		fault_handler_irq_t handler_irq, void *token);
int qcom_iommu_register_device_fault_handler_irq(struct device *dev,
		fault_handler_irq_t handler, void *token);

int qcom_iommu_enable_s1_translation(struct iommu_domain *domain);

int qcom_iommu_get_mappings_configuration(struct iommu_domain *domain);

#ifdef CONFIG_IOMMU_IO_PGTABLE_LPAE
int __init qcom_arm_lpae_do_selftests(void);
#else
static inline int __init qcom_arm_lpae_do_selftests(void)
{
	return 0;
}
#endif
#endif /* __QCOM_IOMMU_UTIL_H */
