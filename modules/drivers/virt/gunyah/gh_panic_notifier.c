// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt)	"gh_panic_notifier: " fmt

#include <linux/dma-direct.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/panic_notifier.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/gunyah/gh_dbl.h>
#include <linux/gunyah/gh_panic_notifier.h>
#include <linux/gunyah/gh_rm_drv.h>
#include <linux/gunyah/gh_vm.h>
#include <soc/qcom/secure_buffer.h>

#define GH_RECOVERY_VM_MAX 2
#define GH_PANIC_DBL_MASK				0x1

struct recovery_vm {
	bool recovery;
	struct device_attribute recovery_attr;
};

struct gh_panic_notifier_dev {
	struct device *dev;
	struct resource res;
	void *base;
	dma_addr_t dma_handle;
	struct mutex lock;
	gh_vmid_t self_vmid, peer_vmid;
	bool assigned;
	bool shared;
	bool panic_registered;
	bool cma_initialized;
	bool stopping;
	bool pins_held;
	struct list_head quarantine_node;
	unsigned int recovery_files;
	struct recovery_vm recovery_vms[GH_RECOVERY_VM_MAX];
	u64 size;
	u32 label, peer_name, memparcel;
	bool primary_vm;
	void *tx_dbl;
	void *rx_dbl;
	struct wakeup_source *ws;
	struct notifier_block vm_nb;
	struct notifier_block gh_panic_blk;
};

static const struct {
	enum gh_vm_names val;
	const char *str;
} recovery_name_to_vm_name[] = {
	{GH_TRUSTED_VM, "trustedvm_recovery"},
	{GH_OEM_VM, "oemvm_recovery"},
};

SRCU_NOTIFIER_HEAD_STATIC(gh_panic_notifier);

/* Removed primary devices keep their ownership records until real reclaim succeeds. */
static LIST_HEAD(gh_panic_quarantine);
static DEFINE_MUTEX(gh_panic_quarantine_lock);

static inline struct recovery_vm *get_recovery_vm_from_vmid(struct gh_panic_notifier_dev *gpnd, gh_vmid_t vmid)
{
	struct recovery_vm *vm = NULL;
	enum gh_vm_names vm_name;
	int i, ret;

	ret = gh_rm_get_vm_name(vmid, &vm_name);
	if (ret) {
		pr_err("Failed to get VM name for VMID%d ret=%d\n", vmid, ret);
		return vm;
	}

	for (i = 0; i < ARRAY_SIZE(recovery_name_to_vm_name); ++i) {
		if (recovery_name_to_vm_name[i].val == vm_name) {
			vm = &gpnd->recovery_vms[i];
			break;
		}
	}

	return vm;
}

int gh_panic_notifier_register(struct notifier_block *nb)
{
	return srcu_notifier_chain_register(&gh_panic_notifier, nb);
}
EXPORT_SYMBOL_GPL(gh_panic_notifier_register);

int gh_panic_notifier_unregister(struct notifier_block *nb)
{
	return srcu_notifier_chain_unregister(&gh_panic_notifier, nb);
}
EXPORT_SYMBOL_GPL(gh_panic_notifier_unregister);

static inline int gh_panic_notifier_kick(void *tx_dbl)
{
	gh_dbl_flags_t dbl_mask = GH_PANIC_DBL_MASK;
	int ret;

	ret = gh_dbl_send(tx_dbl, &dbl_mask, GH_DBL_NONBLOCK);
	if (ret)
		printk_deferred("failed to raise virq to the sender %d\n", ret);

	return ret;
}

static void gh_panic_notify_receiver(int irq, void *data)
{
	struct gh_panic_notifier_dev *gpnd = data;
	gh_dbl_flags_t dbl_mask = GH_PANIC_DBL_MASK;
	int ret;

	/* Registration can publish this threaded callback before returning. */
	mutex_lock(&gpnd->lock);
	if (gpnd->stopping)
		goto out;
	ret = gh_dbl_read_and_clean(gpnd->rx_dbl, &dbl_mask, GH_DBL_NONBLOCK);
	if (ret || !(dbl_mask & GH_PANIC_DBL_MASK))
		goto out;
	__pm_stay_awake(gpnd->ws);
	srcu_notifier_call_chain(&gh_panic_notifier, 0, NULL);
	/* Secondary VM maps this shared acknowledgement as IO memory. */
	writeb(1, gpnd->base);
out:
	mutex_unlock(&gpnd->lock);
}

/* Called with gpnd->lock held and its panic callback unregistered/drained. */
static int gh_panic_notifier_release_mem(struct gh_panic_notifier_dev *gpnd)
{
	struct qcom_scm_vmperm dst = {
		gpnd->self_vmid, PERM_READ | PERM_WRITE | PERM_EXEC
	};
	u64 owners;
	int ret;

	if (!gpnd->base)
		return 0;
	if (gpnd->shared) {
		ret = ghd_rm_mem_reclaim(gpnd->memparcel, 0);
		if (ret)
			return ret;
		gpnd->shared = false;
	}
	if (gpnd->assigned) {
		owners = BIT_ULL(gpnd->self_vmid) | BIT_ULL(gpnd->peer_vmid);
		ret = qcom_scm_assign_mem(gpnd->res.start, gpnd->size, &owners, &dst, 1);
		if (ret)
			return ret;
		gpnd->assigned = false;
	}
	dma_free_coherent(gpnd->dev, gpnd->size, gpnd->base, gpnd->dma_handle);
	gpnd->base = NULL;
	gpnd->memparcel = 0;
	return 0;
}

/* Drop pins only after any caller's CMA/device cleanup has finished. */
static void gh_panic_notifier_drop_refs(struct gh_panic_notifier_dev *gpnd)
{
	if (gpnd->pins_held && !gpnd->base) {
		gpnd->pins_held = false;
		put_device(gpnd->dev);
		module_put(THIS_MODULE);
	}
}

/* SRCU VM callbacks may sleep. Never free a parcel still owned by its peer. */
static int gh_panic_notifier_retry_quarantine(struct device *dev, gh_vmid_t peer)
{
	struct gh_panic_notifier_dev *gpnd, *next;
	int ret, pending = 0;

	mutex_lock(&gh_panic_quarantine_lock);
	list_for_each_entry_safe(gpnd, next, &gh_panic_quarantine, quarantine_node) {
		if (dev ? gpnd->dev != dev : gpnd->peer_vmid != peer)
			continue;
		ret = gh_panic_notifier_release_mem(gpnd);
		if (ret) {
			pending = -EBUSY;
			continue;
		}
		list_del(&gpnd->quarantine_node);
		if (gpnd->cma_initialized)
			of_reserved_mem_device_release(gpnd->dev);
		gh_panic_notifier_drop_refs(gpnd);
		kfree(gpnd);
	}
	mutex_unlock(&gh_panic_quarantine_lock);
	return pending;
}

static int gh_panic_notifier_quarantine_cb(struct notifier_block *nb,
					 unsigned long cmd, void *data)
{
	if (data && (cmd == GH_VM_POWERUP_FAIL || cmd == GH_VM_EARLY_POWEROFF))
		gh_panic_notifier_retry_quarantine(NULL, *(gh_vmid_t *)data);
	return NOTIFY_DONE;
}

static struct notifier_block gh_panic_quarantine_nb = {
	.notifier_call = gh_panic_notifier_quarantine_cb,
	.priority = INT_MIN,
};

static int gh_panic_notifier_share_mem(struct gh_panic_notifier_dev *gpnd,
			gh_vmid_t self, gh_vmid_t peer)
{
	struct qcom_scm_vmperm dst[] = {
		{self, PERM_READ | PERM_WRITE}, {peer, PERM_READ | PERM_WRITE}
	};
	struct gh_acl_desc *acl;
	struct gh_sgl_desc *sgl;
	u64 owners;
	int ret;

	if (self >= 64 || peer >= 64 || self == peer)
		return -EINVAL;
	if (gpnd->assigned || gpnd->shared || !gpnd->base)
		return -EINVAL;
	/* Descriptors must exist before changing the SCM ownership. */
	acl = kzalloc(struct_size(acl, acl_entries, 2), GFP_KERNEL);
	if (!acl)
		return -ENOMEM;
	sgl = kzalloc(struct_size(sgl, sgl_entries, 1), GFP_KERNEL);
	if (!sgl) {
		kfree(acl);
		return -ENOMEM;
	}
	acl->n_acl_entries = 2;
	acl->acl_entries[0].vmid = self;
	acl->acl_entries[0].perms = GH_RM_ACL_R | GH_RM_ACL_W;
	acl->acl_entries[1].vmid = peer;
	acl->acl_entries[1].perms = GH_RM_ACL_R | GH_RM_ACL_W;
	sgl->n_sgl_entries = 1;
	sgl->sgl_entries[0].ipa_base = gpnd->res.start;
	sgl->sgl_entries[0].size = gpnd->size;
	gpnd->self_vmid = self;
	gpnd->peer_vmid = peer;
	owners = BIT_ULL(self);
	ret = qcom_scm_assign_mem(gpnd->res.start, gpnd->size, &owners, dst, ARRAY_SIZE(dst));
	if (ret)
		goto out;
	gpnd->assigned = true;
	ret = ghd_rm_mem_share(GH_RM_MEM_TYPE_NORMAL, 0, gpnd->label,
			       acl, sgl, NULL, &gpnd->memparcel);
	if (!ret)
		gpnd->shared = true;
	/* On error release_mem will restore only the ownership steps that succeeded. */
out:
	kfree(sgl);
	kfree(acl);
	return ret;
}

static int set_irqchip_state(struct irq_desc *desc, unsigned int irq,
			  enum irqchip_irq_state which, bool val)
{
	struct irq_data *data;
	struct irq_chip *chip;
	int ret = -EINVAL;

	if (!desc)
		return ret;

	data = irq_desc_get_irq_data(desc);
	do {
		chip = irq_data_get_irq_chip(data);
		if (WARN_ON_ONCE(!chip))
			return -ENODEV;

		if (chip->irq_set_irqchip_state)
			break;

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
		data = data->parent_data;
#else
		data = NULL;
#endif
	} while (data);

	if (data)
		ret = chip->irq_set_irqchip_state(data, which, val);

	return ret;
}

static int get_irqchip_state(struct irq_desc *desc,
			  enum irqchip_irq_state which, bool *state)
{
	struct irq_data *data;
	struct irq_chip *chip;
	int ret = -EINVAL;

	if (!desc)
		return ret;

	data = irq_desc_get_irq_data(desc);
	do {
		chip = irq_data_get_irq_chip(data);
		if (WARN_ON_ONCE(!chip))
			return -ENODEV;

		if (chip->irq_get_irqchip_state)
			break;

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
		data = data->parent_data;
#else
		data = NULL;
#endif
	} while (data);

	if (data)
		ret = chip->irq_get_irqchip_state(data, which, state);

	return ret;
}

static void clear_pending_irq(void)
{
	bool state;
	unsigned int i;
	struct irq_desc *desc;

	rcu_read_lock();
	for_each_irq_desc(i, desc) {
		struct irq_chip *chip;
		int ret;

		chip = irq_desc_get_chip(desc);
		if (!chip)
			continue;

		ret = get_irqchip_state(desc, IRQCHIP_STATE_PENDING, &state);
		if (!ret && state) {
			/* Clear interrupt pending status */
			ret = set_irqchip_state(desc, i, IRQCHIP_STATE_PENDING, false);
			if (ret && irqd_irq_inprogress(&desc->irq_data) &&
				chip->irq_eoi)
				chip->irq_eoi(&desc->irq_data);

			if (chip->irq_mask)
				chip->irq_mask(&desc->irq_data);

			if (chip->irq_disable && !irqd_irq_disabled(&desc->irq_data))
				chip->irq_disable(&desc->irq_data);
		}
	}
	rcu_read_unlock();
}

static int gh_panic_notifier_notify(struct notifier_block *nb,
			  unsigned long event, void *ptr)
{
	struct gh_panic_notifier_dev *gpnd;
	unsigned int retry_times = 20;
	gh_vmid_t peer_vmid;
	bool *handle_done;
	int ret;

	gpnd = container_of(nb, struct gh_panic_notifier_dev, gh_panic_blk);
	handle_done = gpnd->base;
	if (!handle_done)
		return NOTIFY_DONE;

	peer_vmid = gpnd->peer_vmid;
	ret = gh_panic_notifier_kick(gpnd->tx_dbl);
	if (ret)
		return NOTIFY_DONE;
	/*
	 * When PVM panic, only one cpu can work and disable local irq in PVM.
	 * if there are interrupts pending, they never can be responsed. And call
	 * gh_hcall_vcpu_run will return at once and the vcpu can't be scheduled.
	 * So we should clear the pending interrupts and mask the interrupts before
	 * call gh_hcall_vcpu_run.
	 */
	do {
		clear_pending_irq();
		ret = gh_poll_vcpu_run(peer_vmid);
		if (ret) {
			printk_deferred("Failed poll vcpu run %d\n", ret);
			break;
		}
		retry_times--;
	} while (!READ_ONCE(*handle_done) && retry_times > 0);

	if (!READ_ONCE(*handle_done))
		printk_deferred("Notify the panic to VM fail\n");

	return NOTIFY_DONE;
}

static void gh_panic_notifier_stop_panic(struct gh_panic_notifier_dev *gpnd)
{
	if (gpnd->panic_registered) {
		atomic_notifier_chain_unregister(&panic_notifier_list, &gpnd->gh_panic_blk);
		gpnd->panic_registered = false;
	}
}

static int gh_panic_notifier_vm_cb(struct notifier_block *nb, unsigned long cmd,
			     void *data)
{
	struct gh_panic_notifier_dev *gpnd = container_of(nb, struct gh_panic_notifier_dev, vm_nb);
	struct recovery_vm *vm;
	gh_vmid_t peer, self, notified;
	int ret;

	if (!data)
		return NOTIFY_DONE;
	notified = *(gh_vmid_t *)data;
	mutex_lock(&gpnd->lock);
	if (gpnd->stopping)
		goto out;
	switch (cmd) {
	case GH_VM_BEFORE_POWERUP:
		if (ghd_rm_get_vmid(gpnd->peer_name, &peer) ||
		    ghd_rm_get_vmid(GH_PRIMARY_VM, &self))
			break;
		if (peer != notified)
			break;
		/* A repeated event must not replace a live or unreclaimed allocation. */
		if (gpnd->base)
			break;
		if (self >= 64 || peer >= 64 || self == peer)
			break;
		/* Unload may have started while an SRCU notification was in flight. */
		if (!try_module_get(THIS_MODULE))
			break;
		gpnd->base = dma_alloc_coherent(gpnd->dev, gpnd->size, &gpnd->dma_handle, GFP_KERNEL);
		if (!gpnd->base) {
			module_put(THIS_MODULE);
			break;
		}
		get_device(gpnd->dev);
		gpnd->pins_held = true;
		gpnd->res.start = dma_to_phys(gpnd->dev, gpnd->dma_handle);
		gpnd->res.end = gpnd->res.start + gpnd->size - 1;
		WRITE_ONCE(*(bool *)gpnd->base, false);
		ret = gh_panic_notifier_share_mem(gpnd, self, peer);
		if (!ret) {
			gpnd->gh_panic_blk.notifier_call = gh_panic_notifier_notify;
			ret = atomic_notifier_chain_register(&panic_notifier_list, &gpnd->gh_panic_blk);
			if (!ret)
				gpnd->panic_registered = true;
		}
		if (ret) {
			dev_err(gpnd->dev, "Panic memory setup failed: %d\n", ret);
			ret = gh_panic_notifier_release_mem(gpnd);
			if (ret)
				dev_err(gpnd->dev, "Panic memory ownership pending: %d\n", ret);
			gh_panic_notifier_drop_refs(gpnd);
		}
		break;
	case GH_VM_POWERUP_FAIL:
	case GH_VM_EARLY_POWEROFF:
		/* RM name lookup can already be unavailable during teardown. */
		if (!gpnd->base || gpnd->peer_vmid != notified)
			break;
		gh_panic_notifier_stop_panic(gpnd);
		ret = gh_panic_notifier_release_mem(gpnd);
		if (ret)
			dev_err(gpnd->dev, "Panic memory reclaim pending: %d\n", ret);
		gh_panic_notifier_drop_refs(gpnd);
		break;
	case GH_VM_CRASH:
		vm = get_recovery_vm_from_vmid(gpnd, notified);
		if (vm && !READ_ONCE(vm->recovery)) {
			if (gpnd->base && gpnd->peer_vmid == notified)
				gh_panic_notifier_stop_panic(gpnd);
			mutex_unlock(&gpnd->lock);
			panic("Resetting the SoC");
			return NOTIFY_DONE;
		}
		break;
	default:
		break;
	}
out:
	mutex_unlock(&gpnd->lock);
	return NOTIFY_DONE;
}

static int gh_panic_notifier_svm_mem_map(struct gh_panic_notifier_dev *gpnd)
{
	const char *compat = "qcom,gunyah-panic-gen";
	struct device_node *np = NULL;
	struct device_node *shm_np;
	struct resource *res;
	u32 label;
	int ret;

	while ((np = of_find_compatible_node(np, NULL, compat))) {
		ret = of_property_read_u32(np, "qcom,label", &label);
		if (ret) {
			continue;
		}
		if (label == gpnd->label)
			break;

	}
	if (!np) {
		dev_err(gpnd->dev, "can't find the label=%d memory!\n", gpnd->label);
		return -ENODEV;
	}

	shm_np = of_parse_phandle(np, "memory-region", 0);
	of_node_put(np);
	if (!shm_np)
		return -ENODEV;
	ret = of_address_to_resource(shm_np, 0, &gpnd->res);
	of_node_put(shm_np);
	if (ret) {
		dev_err(gpnd->dev, "of_address_to_resource failed!\n");
		return -EINVAL;
	}

	gpnd->size = resource_size(&gpnd->res);
	if (!gpnd->size)
		return -EINVAL;
	res = devm_request_mem_region(gpnd->dev, gpnd->res.start, gpnd->size,
				dev_name(gpnd->dev));
	if (!res) {
		dev_err(gpnd->dev, "request mem region fail\n");
		return -ENXIO;
	}

	gpnd->base = devm_ioremap_wc(gpnd->dev, gpnd->res.start, gpnd->size);
	if (!gpnd->base) {
		dev_err(gpnd->dev, "ioremap fail\n");
		return -ENOMEM;
	}

	return 0;
}

static int gh_panic_notifier_pvm_mem_probe(struct gh_panic_notifier_dev *gpnd)
{
	struct device *dev = gpnd->dev;
	u32 size;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(dev, "%s: dma_set_mask_and_coherent failed\n", __func__);
		return ret;
	}

	ret = of_reserved_mem_device_init_by_idx(dev, dev->of_node, 0);
	if (ret) {
		dev_err(dev, "%s: Failed to initialize CMA mem, ret %d\n", __func__, ret);
		return ret;
	}

	gpnd->cma_initialized = true;
	ret = of_property_read_u32(gpnd->dev->of_node, "shared-buffer-size", &size);
	if (ret) {
		dev_err(dev, "%s: Failed to get shared memory size, ret %d\n",
				__func__, ret);
		of_reserved_mem_device_release(dev);
		gpnd->cma_initialized = false;
		return ret;
	}
	if (!size || !IS_ALIGNED(size, PAGE_SIZE)) {
		of_reserved_mem_device_release(dev);
		gpnd->cma_initialized = false;
		return -EINVAL;
	}
	gpnd->size = size;
	return 0;
}

static ssize_t recovery_show(struct device *dev,
			     struct device_attribute *attribute, char *buf)
{
	struct recovery_vm *vm;

	vm = container_of(attribute, struct recovery_vm, recovery_attr);

	return sysfs_emit(buf, "%s\n",
			READ_ONCE(vm->recovery) ? "enabled" : "disabled");
}

static ssize_t recovery_store(struct device *dev,
			      struct device_attribute *attribute,
			      const char *buf, size_t count)
{
	struct recovery_vm *vm;

	vm = container_of(attribute, struct recovery_vm, recovery_attr);

	if (sysfs_streq(buf, "enabled"))
		WRITE_ONCE(vm->recovery, true);
	else if (sysfs_streq(buf, "disabled"))
		WRITE_ONCE(vm->recovery, false);
	else
		return -EINVAL;

	return count;
}

static int init_recovery_vms(struct gh_panic_notifier_dev *gpnd)
{
	struct recovery_vm *vm;
	int i, ret;

	for (i = 0; i < GH_RECOVERY_VM_MAX; i++) {
		vm = &gpnd->recovery_vms[i];
		sysfs_attr_init(&vm->recovery_attr.attr);
		WRITE_ONCE(vm->recovery, false);
		vm->recovery_attr.show = recovery_show;
		vm->recovery_attr.store = recovery_store;
		vm->recovery_attr.attr.name = recovery_name_to_vm_name[i].str;
		vm->recovery_attr.attr.mode = 0600;
		ret = sysfs_create_file(&gpnd->dev->kobj, &vm->recovery_attr.attr);
		if (ret) {
			pr_err("Create recovery sysfs entry for vm:%d  failed: %d\n",
				recovery_name_to_vm_name[i].val, ret);
			while (gpnd->recovery_files)
				sysfs_remove_file(&gpnd->dev->kobj,
					&gpnd->recovery_vms[--gpnd->recovery_files].recovery_attr.attr);
			return ret;
		}
		gpnd->recovery_files++;
	}
	return 0;
}

static int gh_panic_notifier_probe_resources(struct platform_device *pdev,
					   struct gh_panic_notifier_dev *gpnd)
{
	int ret;
	ret = of_property_read_u32(pdev->dev.of_node, "gunyah-label", &gpnd->label);
	if (ret)
		return ret;
	if (gpnd->label >= GH_DBL_LABEL_MAX)
		return -EINVAL;
	if (gpnd->primary_vm) {
		ret = gh_panic_notifier_pvm_mem_probe(gpnd);
		if (ret)
			return ret;
		ret = of_property_read_u32(pdev->dev.of_node, "peer-name", &gpnd->peer_name);
		if (ret)
			gpnd->peer_name = GH_SELF_VM;
		gpnd->tx_dbl = gh_dbl_tx_register(gpnd->label);
		if (IS_ERR_OR_NULL(gpnd->tx_dbl)) {
			ret = gpnd->tx_dbl ? PTR_ERR(gpnd->tx_dbl) : -ENODEV;
			goto release_cma;
		}
		ret = init_recovery_vms(gpnd);
		if (ret)
			goto unregister_tx;
		gpnd->vm_nb.notifier_call = gh_panic_notifier_vm_cb;
		gpnd->vm_nb.priority = INT_MAX;
		ret = gh_register_vm_notifier(&gpnd->vm_nb);
		if (!ret)
			return 0;
		while (gpnd->recovery_files)
			sysfs_remove_file(&gpnd->dev->kobj,
				&gpnd->recovery_vms[--gpnd->recovery_files].recovery_attr.attr);
unregister_tx:
		gh_dbl_tx_unregister(gpnd->tx_dbl);
release_cma:
		of_reserved_mem_device_release(gpnd->dev);
		gpnd->cma_initialized = false;
		return ret;
	}
	ret = gh_panic_notifier_svm_mem_map(gpnd);
	if (ret)
		return ret;
	gpnd->ws = wakeup_source_register(gpnd->dev, dev_name(gpnd->dev));
	if (!gpnd->ws)
		return -ENOMEM;
	mutex_lock(&gpnd->lock);
	gpnd->rx_dbl = gh_dbl_rx_register(gpnd->label, gh_panic_notify_receiver, gpnd);
	mutex_unlock(&gpnd->lock);
	if (IS_ERR_OR_NULL(gpnd->rx_dbl)) {
		ret = gpnd->rx_dbl ? PTR_ERR(gpnd->rx_dbl) : -ENODEV;
		wakeup_source_unregister(gpnd->ws);
		return ret;
	}
	return 0;
}

static int gh_panic_notifier_probe(struct platform_device *pdev)
{
	struct gh_panic_notifier_dev *gpnd;
	bool primary = of_property_read_bool(pdev->dev.of_node, "qcom,primary-vm");
	int ret;

	if (primary) {
		ret = gh_panic_notifier_retry_quarantine(&pdev->dev, 0);
		if (ret)
			return ret;
		/* Primary ownership records may outlive device removal. */
		gpnd = kzalloc(sizeof(*gpnd), GFP_KERNEL);
	} else {
		gpnd = devm_kzalloc(&pdev->dev, sizeof(*gpnd), GFP_KERNEL);
	}
	if (!gpnd)
		return -ENOMEM;
	gpnd->dev = &pdev->dev;
	gpnd->primary_vm = primary;
	mutex_init(&gpnd->lock);
	INIT_LIST_HEAD(&gpnd->quarantine_node);
	platform_set_drvdata(pdev, gpnd);
	ret = gh_panic_notifier_probe_resources(pdev, gpnd);
	if (ret) {
		platform_set_drvdata(pdev, NULL);
		if (primary)
			kfree(gpnd);
	}
	return ret;
}

static void gh_panic_notifier_remove(struct platform_device *pdev)
{
	struct gh_panic_notifier_dev *gpnd = platform_get_drvdata(pdev);
	int ret;

	if (gpnd->primary_vm) {
		mutex_lock(&gpnd->lock);
		gpnd->stopping = true;
		mutex_unlock(&gpnd->lock);
		gh_unregister_vm_notifier(&gpnd->vm_nb);
		mutex_lock(&gpnd->lock);
		gh_panic_notifier_stop_panic(gpnd);
		ret = gh_panic_notifier_release_mem(gpnd);
		mutex_unlock(&gpnd->lock);
		while (gpnd->recovery_files)
			sysfs_remove_file(&gpnd->dev->kobj,
				&gpnd->recovery_vms[--gpnd->recovery_files].recovery_attr.attr);
		gh_dbl_tx_unregister(gpnd->tx_dbl);
		if (ret) {
			/* Keep both the DMA allocation and its non-devm ownership record. */
			dev_err(gpnd->dev, "Panic memory quarantined until peer teardown/reprobe: %d\n", ret);
			mutex_lock(&gh_panic_quarantine_lock);
			list_add_tail(&gpnd->quarantine_node, &gh_panic_quarantine);
			mutex_unlock(&gh_panic_quarantine_lock);
		} else {
			if (gpnd->cma_initialized)
				of_reserved_mem_device_release(gpnd->dev);
			gh_panic_notifier_drop_refs(gpnd);
			kfree(gpnd);
		}
	} else {
		mutex_lock(&gpnd->lock);
		gpnd->stopping = true;
		mutex_unlock(&gpnd->lock);
		/* The repaired provider drains callbacks without holding gpnd->lock. */
		gh_dbl_rx_unregister(gpnd->rx_dbl);
		wakeup_source_unregister(gpnd->ws);
	}
	platform_set_drvdata(pdev, NULL);
}

static const struct of_device_id gh_panic_notifier_match_table[] = {
	{ .compatible = "qcom,gh-panic-notifier" },
	{}
};

static struct platform_driver gh_panic_notifier_driver = {
	.driver = {
		.name = "gh_panic_notifier",
		.of_match_table = gh_panic_notifier_match_table,
		.suppress_bind_attrs = true,
	 },
	.probe = gh_panic_notifier_probe,
	.remove = gh_panic_notifier_remove,
};

static int __init gh_panic_notifier_init(void)
{
	int ret;

	ret = gh_register_vm_notifier(&gh_panic_quarantine_nb);
	if (ret)
		return ret;
	ret = platform_driver_register(&gh_panic_notifier_driver);
	if (ret)
		gh_unregister_vm_notifier(&gh_panic_quarantine_nb);
	return ret;
}

#if IS_ENABLED(CONFIG_ARCH_QTI_VM)
arch_initcall(gh_panic_notifier_init);
#else
module_init(gh_panic_notifier_init);
#endif

static __exit void gh_panic_notifier_exit(void)
{
	platform_driver_unregister(&gh_panic_notifier_driver);
	gh_unregister_vm_notifier(&gh_panic_quarantine_nb);
}
module_exit(gh_panic_notifier_exit);

MODULE_DESCRIPTION(" Qualcomm Technologies, Inc. Gunyah Panic Notifier Driver");
MODULE_LICENSE("GPL");
