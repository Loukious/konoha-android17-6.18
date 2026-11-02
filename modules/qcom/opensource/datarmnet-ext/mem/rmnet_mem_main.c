/* Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only
*/

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/module.h>
#include <linux/mm.h>
#include "rmnet_mem_nl.h"
#include "rmnet_mem.h"
#include "rmnet_mem_priv.h"
MODULE_DESCRIPTION("Qualcomm RMNET page pool manager");
MODULE_LICENSE("GPL v2");
DEFINE_SPINLOCK(rmnet_mem_lock);
int DATARMNETfb2a1a4560[POOL_LEN];
module_param_array(DATARMNETfb2a1a4560, int, NULL, (0xcb7 + 5769 - 0x221c));
MODULE_PARM_DESC(DATARMNETfb2a1a4560,
		 "\x67\x61\x76\x65\x75\x70\x20\x70\x65\x72\x20\x69\x64");
int max_pool_size[POOL_LEN] = { (0xd2d + 202 - 0xdf7), (0xd2d + 202 - 0xdf7),
				MAX_POOL_O2, MAX_POOL_O3 };
module_param_array(max_pool_size, int, NULL, (0xdb7 + 6665 - 0x261c));
MODULE_PARM_DESC(
	max_pool_size,
	"\x4d\x61\x78\x20\x50\x6f\x6f\x6c\x20\x73\x69\x7a\x65\x20\x70\x65\x72\x20\x6f\x72\x64\x65\x72");
int static_pool_size[POOL_LEN];
module_param_array(static_pool_size, int, NULL, (0xcb7 + 5769 - 0x221c));
MODULE_PARM_DESC(
	static_pool_size,
	"\x50\x6f\x6f\x6c\x20\x73\x69\x7a\x65\x20\x70\x65\x72\x20\x6f\x72\x64\x65\x72");
int pool_unbound_feature[POOL_LEN] = { (0xd2d + 202 - 0xdf7),
				       (0xd2d + 202 - 0xdf7),
				       (0xd26 + 209 - 0xdf6),
				       (0xd26 + 209 - 0xdf6) };
module_param_array(pool_unbound_feature, int, NULL, (0xdb7 + 6665 - 0x261c));
MODULE_PARM_DESC(pool_unbound_featue,
		 "\x50\x6f\x6f\x6c\x20\x62\x6f\x75\x6e\x64\x20\x67\x61\x74\x65");
int rmnet_mem_order_requests[POOL_LEN];
module_param_array(rmnet_mem_order_requests, int, NULL,
		   (0xcb7 + 5769 - 0x221c));
MODULE_PARM_DESC(
	rmnet_mem_order_requests,
	"\x52\x65\x71\x75\x65\x73\x74\x20\x70\x65\x72\x20\x6f\x72\x64\x65\x72");
int rmnet_mem_id_req[POOL_LEN];
module_param_array(rmnet_mem_id_req, int, NULL, (0xcb7 + 5769 - 0x221c));
MODULE_PARM_DESC(rmnet_mem_id_req,
		 "\x52\x65\x71\x75\x65\x73\x74\x20\x70\x65\x72\x20\x69\x64");
int rmnet_mem_id_recycled[POOL_LEN];
module_param_array(rmnet_mem_id_recycled, int, NULL, (0xcb7 + 5769 - 0x221c));
MODULE_PARM_DESC(rmnet_mem_id_recycled,
		 "\x52\x65\x63\x79\x63\x6c\x65\x64\x20\x70\x65\x72\x20\x69\x64");
int DATARMNETa293261aea[DATARMNETbc3b36ce41];
module_param_array(DATARMNETa293261aea, int, NULL, (0xcb7 + 5769 - 0x221c));
MODULE_PARM_DESC(
	DATARMNETa293261aea,
	"\x52\x6d\x6e\x65\x74\x20\x6d\x65\x6d\x20\x73\x74\x61\x74\x73\x20\x66\x6f\x72\x20\x6d\x6f\x64\x75\x6c\x65\x73");
int DATARMNETaceba3d00e[DATARMNETfdc300cd1f];
module_param_array(DATARMNETaceba3d00e, int, NULL, (0xcb7 + 5769 - 0x221c));
MODULE_PARM_DESC(DATARMNETaceba3d00e,
		 "\x45\x72\x72\x6f\x72\x20\x63\x6f\x75\x6e\x74\x69\x6e\x67");
unsigned int DATARMNETf85ebffa7a[POOL_LEN];
module_param_array(DATARMNETf85ebffa7a, uint, NULL, (0xdb7 + 6665 - 0x261c));
MODULE_PARM_DESC(
	DATARMNETf85ebffa7a,
	"\x50\x6f\x6f\x6c\x20\x73\x69\x7a\x65\x20\x76\x6f\x74\x65\x20\x74\x68\x61\x74\x20\x69\x73\x20\x61\x63\x74\x69\x76\x65\x20\x6f\x6e\x20\x50\x42\x20\x69\x6e\x64");
unsigned int DATARMNET15e53a8338[POOL_LEN] = { (0xd2d + 202 - 0xdf7),
					   (0xd2d + 202 - 0xdf7),
					   DATARMNETe4e4a9ca82,
					   DATARMNETeb73899668 };
module_param_array(DATARMNET15e53a8338, uint, NULL, (0xcb7 + 5769 - 0x221c));
MODULE_PARM_DESC(
	DATARMNET15e53a8338,
	"\x50\x6f\x6f\x6c\x20\x73\x69\x7a\x65\x20\x77\x71\x20\x77\x69\x6c\x6c\x20\x61\x64\x6a\x75\x73\x74\x20\x74\x6f\x20\x6f\x6e\x20\x72\x75\x6e");
static char *verinfo[] = { "\x32\x30\x30\x33\x62\x61\x65\x33" };
module_param_array(verinfo, charp, NULL, (0xcb7 + 5769 - 0x221c));
MODULE_PARM_DESC(
	verinfo,
	"\x56\x65\x72\x73\x69\x6f\x6e\x20\x6f\x66\x20\x74\x68\x65\x20\x64\x72\x69\x76\x65\x72");
struct workqueue_struct *mem_wq;
struct delayed_work pool_adjust_work;
int DATARMNETb84a1c23e6;
struct hrtimer DATARMNET7bbd010c46;
struct list_head rmnet_mem_pool[POOL_LEN];
struct mem_info {
	struct page *addr;
	struct list_head mem_head;
	u8 order;
};

void rmnet_mem_page_ref_inc_entry(struct page *page, unsigned int id)
{
	page_ref_inc(page);
}
EXPORT_SYMBOL_GPL(rmnet_mem_page_ref_inc_entry);
struct rmnet_mem_notif_s {
	struct raw_notifier_head chain;
	spinlock_t lock;
};

struct rmnet_mem_notif_s rmnet_mem_notifier = {
	.chain = RAW_NOTIFIER_INIT(rmnet_mem_notifier.chain),
	.lock = __SPIN_LOCK_UNLOCKED(rmnet_mem_notifier.lock),
};
EXPORT_SYMBOL_GPL(rmnet_mem_notifier);
int rmnet_mem_get_pool_size(unsigned int order)
{
	if (order >= POOL_LEN) {
		DATARMNETaceba3d00e[DATARMNET0b84d87330]++;
		return (0xd2d + 202 - 0xdf7);
	}
	return (static_pool_size[order]) ? static_pool_size[order] :
					   DATARMNET15e53a8338[order];
}
EXPORT_SYMBOL_GPL(rmnet_mem_get_pool_size);
static int rmnet_mem_mode_notify(unsigned int pool_size)
{
	unsigned long flags;

	spin_lock_irqsave(&rmnet_mem_notifier.lock, flags);
	raw_notifier_call_chain(&rmnet_mem_notifier.chain, pool_size, NULL);
	spin_unlock_irqrestore(&rmnet_mem_notifier.lock, flags);
	return NOTIFY_OK;
}

int rmnet_mem_register_notifier(struct notifier_block *nb)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&rmnet_mem_notifier.lock, flags);
	ret = raw_notifier_chain_register(&rmnet_mem_notifier.chain, nb);
	spin_unlock_irqrestore(&rmnet_mem_notifier.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(rmnet_mem_register_notifier);
int rmnet_mem_unregister_notifier(struct notifier_block *nb)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&rmnet_mem_notifier.lock, flags);
	ret = raw_notifier_chain_unregister(&rmnet_mem_notifier.chain, nb);
	spin_unlock_irqrestore(&rmnet_mem_notifier.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(rmnet_mem_unregister_notifier);
static struct mem_info *rmnet_mem_add_page(struct page *page, u8 pageorder)
{
	struct mem_info *mem_slot;

	mem_slot = kzalloc(sizeof(*mem_slot), GFP_ATOMIC);
	if (!mem_slot) {
		DATARMNETaceba3d00e[DATARMNETbcd0fd349d]++;
		return NULL;
	}
	static_pool_size[pageorder]++;
	mem_slot->order = pageorder;
	mem_slot->addr = (void *)page;
	INIT_LIST_HEAD(&mem_slot->mem_head);
	if (pageorder < POOL_LEN)
		list_add_rcu(&mem_slot->mem_head, &rmnet_mem_pool[pageorder]);
	return mem_slot;
}

static void rmnet_mem_free_all(void)
{
	unsigned long flags;
	struct mem_info *mem_slot;
	struct list_head *ptr = NULL, *next = NULL;
	int i;

	spin_lock_irqsave(&rmnet_mem_lock, flags);
	for (i = (0xd2d + 202 - 0xdf7); i < POOL_LEN; i++) {
		list_for_each_safe(ptr, next, &rmnet_mem_pool[i]) {
			mem_slot = list_entry(ptr, struct mem_info, mem_head);
			list_del(&mem_slot->mem_head);
			put_page(mem_slot->addr);
			static_pool_size[mem_slot->order]--;
			kfree(mem_slot);
		}
	}
	spin_unlock_irqrestore(&rmnet_mem_lock, flags);
}

struct page *rmnet_mem_get_pages_entry(gfp_t gfp_mask, unsigned int order,
				       int *code, int *pageorder,
				       unsigned int id)
{
	struct mem_info *mem_page;
	struct page *page = NULL;
	unsigned long flags;
	unsigned int selected_order = order;
	unsigned int attempts, j;
	bool expand_pool;

	if (order >= POOL_LEN || id >= ARRAY_SIZE(rmnet_mem_id_req)) {
		if (code)
			*code = RMNET_MEM_FAIL;
		if (pageorder)
			*pageorder = 0;
		return NULL;
	}

	spin_lock_irqsave(&rmnet_mem_lock, flags);
	rmnet_mem_id_req[id]++;
	rmnet_mem_order_requests[order]++;
	for (j = order; j > 0 && j < POOL_LEN && !page; j++) {
		attempts = 0;
		do {
			mem_page = list_first_entry_or_null(
				&rmnet_mem_pool[j], struct mem_info, mem_head);
			if (!mem_page)
				break;
			if (page_ref_count(mem_page->addr) == 1) {
				rmnet_mem_id_recycled[j]++;
				page = mem_page->addr;
				page_ref_inc(page);
				selected_order = j;
			}
			list_rotate_left(&rmnet_mem_pool[j]);
		} while (!page && ++attempts <= 5);
	}

	expand_pool = static_pool_size[order] < max_pool_size[order] &&
		      pool_unbound_feature[order];
	if (!expand_pool)
		spin_unlock_irqrestore(&rmnet_mem_lock, flags);
	if (!page) {
		DATARMNETfb2a1a4560[id]++;
		if (order < 3 || expand_pool) {
			page = __dev_alloc_pages(
				expand_pool ? GFP_ATOMIC : gfp_mask, order);
			if (page && expand_pool &&
			    rmnet_mem_add_page(page, order))
				page_ref_inc(page);
		}
	}
	if (expand_pool)
		spin_unlock_irqrestore(&rmnet_mem_lock, flags);

	if (pageorder)
		*pageorder = page ? selected_order : 0;
	if (code) {
		if (!page)
			*code = RMNET_MEM_FAIL;
		else if (selected_order == order)
			*code = RMNET_MEM_SUCCESS;
		else if (selected_order > order)
			*code = RMNET_MEM_UPGRADE;
		else
			*code = RMNET_MEM_DOWNGRADE;
	}
	return page;
}
EXPORT_SYMBOL_GPL(rmnet_mem_get_pages_entry);
void rmnet_mem_put_page_entry(struct page *page)
{
	put_page(page);
}
EXPORT_SYMBOL_GPL(rmnet_mem_put_page_entry);
static void mem_update_pool_work(struct work_struct *work)
{
	int i;
	int DATARMNET2df6d5c0a2;

	local_bh_disable();
	for (i = (0xd2d + 202 - 0xdf7); i < POOL_LEN; i++) {
		DATARMNET2df6d5c0a2 =
			(DATARMNETb84a1c23e6 && DATARMNETf85ebffa7a[i]) ?
				DATARMNET3657d13d98(DATARMNETf85ebffa7a[i],
						    DATARMNET15e53a8338[i]) :
				DATARMNET15e53a8338[i];
		rmnet_mem_adjust(DATARMNET2df6d5c0a2, i);
	}
	local_bh_enable();
}

void rmnet_mem_adjust(unsigned int perm_size, u8 pageorder)
{
	struct mem_info *mem_slot;
	struct list_head *entry, *next;
	struct page *newpage;
	unsigned long flags;
	unsigned int attempts;

	if (pageorder >= POOL_LEN || perm_size > MAX_STATIC_POOL) {
		DATARMNETaceba3d00e[DATARMNETf03ba54970]++;
		return;
	}
	spin_lock_irqsave(&rmnet_mem_lock, flags);
	if (perm_size == static_pool_size[pageorder]) {
		spin_unlock_irqrestore(&rmnet_mem_lock, flags);
		return;
	}
	if (perm_size > static_pool_size[pageorder]) {
		attempts = perm_size - static_pool_size[pageorder];
		while (attempts--) {
			newpage = __dev_alloc_pages(GFP_ATOMIC, pageorder);
			if (newpage && !rmnet_mem_add_page(newpage, pageorder))
				put_page(newpage);
		}
	} else {
		list_for_each_safe(entry, next, &rmnet_mem_pool[pageorder]) {
			if (static_pool_size[pageorder] == perm_size)
				break;
			mem_slot = list_entry(entry, struct mem_info, mem_head);
			list_del(&mem_slot->mem_head);
			put_page(mem_slot->addr);
			kfree(mem_slot);
			static_pool_size[pageorder]--;
		}
	}
	spin_unlock_irqrestore(&rmnet_mem_lock, flags);
	if (pageorder == POOL_NOTIF)
		rmnet_mem_mode_notify(perm_size);
}

static enum hrtimer_restart
DATARMNET2c79fa7b83(struct hrtimer *DATARMNET6e4292679f)
{
	unsigned int DATARMNET28085cfd14;

	DATARMNETb84a1c23e6 = (0xd2d + 202 - 0xdf7);
	DATARMNETa293261aea[DATARMNETc8e634191a]++;
	DATARMNET28085cfd14 = msecs_to_jiffies(DATARMNET675090896c);
	queue_delayed_work(mem_wq, &pool_adjust_work, DATARMNET28085cfd14);
	return HRTIMER_NORESTART;
}

void rmnet_mem_pb_ind(void)
{
	if (!DATARMNETf85ebffa7a[POOL_NOTIF]) {
		DATARMNETa293261aea[DATARMNET55412eb785]++;
		return;
	}
	DATARMNETb84a1c23e6 = (0xd26 + 209 - 0xdf6);
	if (hrtimer_active(&DATARMNET7bbd010c46)) {
		hrtimer_cancel(&DATARMNET7bbd010c46);
	} else {
		cancel_delayed_work(&pool_adjust_work);
		queue_delayed_work(mem_wq, &pool_adjust_work,
				   (0xd2d + 202 - 0xdf7));
	}
	DATARMNETa293261aea[RMNET_MEM_PB_IND]++;
	hrtimer_start(&DATARMNET7bbd010c46,
		      ns_to_ktime(DATARMNETc8ec566153 * DATARMNET68fc0be252),
		      HRTIMER_MODE_REL | HRTIMER_MODE_PINNED);
}
EXPORT_SYMBOL_GPL(rmnet_mem_pb_ind);

static int __init rmnet_mem_module_init(void)
{
	int rc, i;

	pr_info("%s(): Starting rmnet mem module\n", __func__);
	for (i = 0; i < POOL_LEN; i++)
		INIT_LIST_HEAD(&rmnet_mem_pool[i]);
	mem_wq = alloc_workqueue("mem_wq", WQ_HIGHPRI, 0);
	if (!mem_wq)
		return -ENOMEM;
	INIT_DELAYED_WORK(&pool_adjust_work, mem_update_pool_work);
	hrtimer_setup(&DATARMNET7bbd010c46, DATARMNET2c79fa7b83,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	rc = rmnet_mem_nl_register();
	if (rc) {
		destroy_workqueue(mem_wq);
		mem_wq = NULL;
		return rc;
	}
	return 0;
}

static void __exit rmnet_mem_module_exit(void)
{
	rmnet_mem_nl_unregister();
	hrtimer_cancel(&DATARMNET7bbd010c46);
	if (mem_wq) {
		cancel_delayed_work_sync(&pool_adjust_work);
		drain_workqueue(mem_wq);
		destroy_workqueue(mem_wq);
		mem_wq = NULL;
	}
	rmnet_mem_free_all();
}

module_init(rmnet_mem_module_init);
module_exit(rmnet_mem_module_exit);
