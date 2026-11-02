// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/of_device.h>
#include <linux/completion.h>
#include <linux/idr.h>
#include <linux/pm_runtime.h>
#include <linux/refcount.h>
#include <soc/soundwire.h>

struct boardinfo {
	struct list_head	list;
	struct swr_boardinfo	board_info;
};

static LIST_HEAD(board_list);
static LIST_HEAD(swr_master_list);
static DEFINE_MUTEX(board_lock);
static DEFINE_IDR(master_idr);
static DEFINE_MUTEX(swr_lock);

static struct device_type swr_dev_type;

/* Do not hold a lock across controller callbacks: they may re-enter the bus. */
static int swr_master_begin(struct swr_master *master)
{
	unsigned long flags;
	int ret = 0;

	if (!master)
		return -ENODEV;
	spin_lock_irqsave(&master->state_lock, flags);
	if (master->stopping)
		ret = -ENODEV;
	else
		atomic_inc(&master->active_calls);
	spin_unlock_irqrestore(&master->state_lock, flags);
	return ret;
}

static void swr_master_end(struct swr_master *master)
{
	if (atomic_dec_and_test(&master->active_calls))
		wake_up_all(&master->call_wait);
}

/* The codec owns the domain; registration retains only its slave device. */
struct swr_slave_irq {
	struct list_head node;
	struct swr_device *swr;
	struct irq_domain *domain;
	/* Serializes dispatch with synchronous withdrawal. */
	struct mutex dispatch_lock;
	struct task_struct *handler;
	refcount_t refs;
	bool stopping;
};

static LIST_HEAD(swr_slave_irqs);
static DEFINE_MUTEX(swr_slave_irq_lock);

static void swr_slave_irq_put(struct swr_slave_irq *irq)
{
	if (refcount_dec_and_test(&irq->refs)) {
		put_device(&irq->swr->dev);
		mutex_destroy(&irq->dispatch_lock);
		kfree(irq);
	}
}

/* swr_slave_irq_lock protects the registry and the stopping flag. */
static bool swr_slave_irq_in_handler(void)
{
	struct swr_slave_irq *irq;

	list_for_each_entry(irq, &swr_slave_irqs, node)
		if (READ_ONCE(irq->handler) == current)
			return true;
	return false;
}

/**
 * swr_set_slave_irq - publish or withdraw a borrowed codec IRQ domain
 * @swr: retained slave device owned by the caller
 * @domain: domain to publish, or NULL to withdraw and drain dispatch
 *
 * The owner must serialize lifecycle calls and keep its domain, IRQ handlers,
 * and handler data alive until withdrawal returns zero. A NULL domain is safe
 * after master shutdown and succeeds when no domain is registered. A second
 * publication or simultaneous withdrawal returns -EBUSY. Lifecycle calls from
 * any nested slave IRQ handler return -EDEADLK without changing registration;
 * defer those calls to process context after the handler has returned.
 *
 * Context: process context, may sleep. Do not hold a lock needed by a handler.
 */
int swr_set_slave_irq(struct swr_device *swr, struct irq_domain *domain)
{
	struct swr_slave_irq *irq, *found = NULL;
	int ret = 0;

	if (!swr || IS_ERR(domain))
		return -EINVAL;
	if (domain) {
		ret = swr_master_begin(swr->master);
		if (ret)
			return ret;
	}

	mutex_lock(&swr_slave_irq_lock);
	if (swr_slave_irq_in_handler()) {
		ret = -EDEADLK;
		goto unlock;
	}
	list_for_each_entry(irq, &swr_slave_irqs, node) {
		if (irq->swr == swr) {
			found = irq;
			break;
		}
	}

	if (domain) {
		if (found) {
			ret = -EBUSY;
			goto unlock;
		}
		irq = kzalloc(sizeof(*irq), GFP_KERNEL);
		if (!irq) {
			ret = -ENOMEM;
			goto unlock;
		}
		irq->swr = swr;
		irq->domain = domain;
		mutex_init(&irq->dispatch_lock);
		refcount_set(&irq->refs, 1);
		get_device(&swr->dev);
		WRITE_ONCE(swr->slave_irq, domain);
		list_add_tail(&irq->node, &swr_slave_irqs);
		goto unlock;
	}

	if (!found) {
		/* Untracked direct stores cannot provide a safe drain contract. */
		if (READ_ONCE(swr->slave_irq))
			ret = -EINVAL;
		goto unlock;
	}
	if (found->stopping) {
		ret = -EBUSY;
		goto unlock;
	}
	found->stopping = true;
	mutex_unlock(&swr_slave_irq_lock);

	/* A reserved dispatcher either completes first or observes NULL. */
	mutex_lock(&found->dispatch_lock);
	found->domain = NULL;
	WRITE_ONCE(swr->slave_irq, NULL);
	mutex_unlock(&found->dispatch_lock);

	mutex_lock(&swr_slave_irq_lock);
	list_del(&found->node);
	mutex_unlock(&swr_slave_irq_lock);
	swr_slave_irq_put(found);
	return 0;

unlock:
	mutex_unlock(&swr_slave_irq_lock);
	if (domain)
		swr_master_end(swr->master);
	return ret;
}
EXPORT_SYMBOL(swr_set_slave_irq);

/**
 * swr_handle_slave_irq - dispatch one nested interrupt for a registered slave
 * @master: controller whose threaded IRQ handler is running
 * @dev_num: logical slave number reported by the controller
 * @dispatch: controller callback which maps and dispatches the borrowed domain
 *
 * Returns true when the codec reports a pending interrupt after dispatch. The
 * controller retains its native pending loop and device-up check. Registration
 * retains the device; each dispatch reserves both registration and controller.
 * Recursive dispatch from a nested slave IRQ handler is rejected.
 *
 * Context: threaded IRQ/process context, may sleep.
 */
bool swr_handle_slave_irq(struct swr_master *master, u8 dev_num,
			  void (*dispatch)(struct irq_domain *))
{
	struct swr_slave_irq *irq, *found = NULL;
	bool pending = false;

	if (!dispatch || swr_master_begin(master))
		return false;
	mutex_lock(&swr_slave_irq_lock);
	if (!swr_slave_irq_in_handler()) {
		list_for_each_entry(irq, &swr_slave_irqs, node) {
			if (!irq->stopping && irq->swr->master == master &&
			    irq->swr->dev_num == dev_num) {
				refcount_inc(&irq->refs);
				found = irq;
				break;
			}
		}
	}
	mutex_unlock(&swr_slave_irq_lock);
	if (!found)
		goto out;

	mutex_lock(&found->dispatch_lock);
	if (found->domain) {
		WRITE_ONCE(found->handler, current);
		WRITE_ONCE(found->swr->slave_irq_pending, false);
		dispatch(found->domain);
		pending = READ_ONCE(found->swr->slave_irq_pending);
		WRITE_ONCE(found->handler, NULL);
	}
	mutex_unlock(&found->dispatch_lock);
	swr_slave_irq_put(found);
out:
	swr_master_end(master);
	return pending;
}
EXPORT_SYMBOL(swr_handle_slave_irq);

struct swr_transaction {
	struct swr_params params;
	refcount_t refs;
};

static void swr_transaction_put(struct swr_params *params)
{
	struct swr_transaction *txn;

	if (!params)
		return;
	txn = container_of(params, struct swr_transaction, params);
	if (refcount_dec_and_test(&txn->refs))
		kfree(txn);
}

static void swr_transaction_detach(struct swr_master *master,
				   struct swr_params *params)
{
	unsigned long flags;
	bool detached = false;

	spin_lock_irqsave(&master->state_lock, flags);
	if (master->port_txn && master->port_txn[params->tid] == params) {
		master->port_txn[params->tid] = NULL;
		detached = true;
	}
	spin_unlock_irqrestore(&master->state_lock, flags);
	if (detached)
		swr_transaction_put(params);
}

/* The native group leader suppresses duplicate broadcasts from other slaves. */
static bool swr_group_target(struct swr_device *dev, u8 *dev_num,
			     bool broadcast)
{
	struct swr_master *master = dev->master;
	unsigned long flags;
	bool send = true;

	spin_lock_irqsave(&master->state_lock, flags);
	if (dev->group_id) {
		if (master->gr_sid != *dev_num) {
			if (!master->gr_sid)
				master->gr_sid = *dev_num;
			else
				send = false;
		}
		if (broadcast)
			*dev_num = dev->group_id;
	}
	spin_unlock_irqrestore(&master->state_lock, flags);
	return send;
}

static void swr_driver_drain(struct swr_device *swr)
{
	unsigned long flags;

	spin_lock_irqsave(&swr->driver_lock, flags);
	swr->driver_stopping = true;
	spin_unlock_irqrestore(&swr->driver_lock, flags);
	wait_event(swr->driver_wait, !atomic_read(&swr->driver_calls));
	spin_lock_irqsave(&swr->driver_lock, flags);
	swr->driver = NULL;
	spin_unlock_irqrestore(&swr->driver_lock, flags);
	atomic_inc(&swr->scp_generation);
}

static int swr_driver_event(struct swr_device *swr, unsigned int event)
{
	struct swr_driver *drv;
	unsigned long flags;
	int (*callback)(struct swr_device *) = NULL;
	int ret;

	if (!swr)
		return -EINVAL;
	ret = swr_master_begin(swr->master);
	if (ret)
		return ret;
	atomic_inc(&swr->scp_generation);
	spin_lock_irqsave(&swr->driver_lock, flags);
	drv = swr->driver_stopping ? NULL : swr->driver;
	if (drv) {
		callback = event == 0 ? drv->device_up :
			(event == 1 ? drv->device_down : drv->reset_device);
		if (callback)
			atomic_inc(&swr->driver_calls);
	}
	spin_unlock_irqrestore(&swr->driver_lock, flags);
	ret = drv ? -ENODEV : (event == 2 ? -EINVAL : 0);
	if (callback) {
		ret = callback(swr);
		if (atomic_dec_and_test(&swr->driver_calls))
			wake_up_all(&swr->driver_wait);
	}
	swr_master_end(swr->master);
	return ret;
}


#define SOUNDWIRE_NAME_SIZE	32

static void swr_master_put(struct swr_master *master)
{
	if (master)
		put_device(&master->dev);
}

static struct swr_master *swr_master_get(struct swr_master *master)
{
	if (!master || !get_device(&master->dev))
		return NULL;
	return master;
}

static void swr_dev_release(struct device *dev)
{
	struct swr_device *swr = to_swr_device(dev);

	/* Registration/removal detaches the list before dropping its reference. */
	of_node_put(dev->of_node);
	swr_master_put(swr->master);
	kfree(swr);
}


/**
 * swr_remove_device - remove a soundwire device
 * @swr_dev: soundwire device to remove
 *
 * Remove a soundwire device. Go through the soundwire
 * device list that master has and remove swr_dev from
 * it.
 */
void swr_remove_device(struct swr_device *swr)
{
	if (!swr || !swr->master)
		return;
	mutex_lock(&swr->master->mlock);
	list_del_init(&swr->dev_list);
	mutex_unlock(&swr->master->mlock);
}
EXPORT_SYMBOL(swr_remove_device);

/**
 * swr_new_device - instantiate a new soundwire device
 * @master: Controller to which device is connected
 * @info: Describes the soundwire device
 * Context: can sleep
 *
 * Create a soundwire device. Binding is handled through driver model
 * probe/remove methods. A driver may be bound to this device when
 * the function gets returned.
 *
 * Returns a soundwire new device or NULL
 */
struct swr_device *swr_new_device(struct swr_master *master,
				 struct swr_boardinfo const *info)
{
	struct swr_device *swr;
	int ret;

	if (!info || swr_master_begin(master))
		return NULL;
	if (!swr_master_get(master))
		goto out;
	swr = kzalloc(sizeof(*swr), GFP_KERNEL);
	if (!swr) {
		swr_master_put(master);
		goto out;
	}
	swr->master = master;
	swr->addr = info->addr;
	strscpy(swr->name, info->name, sizeof(swr->name));
	INIT_LIST_HEAD(&swr->dev_list);
	spin_lock_init(&swr->driver_lock);
	init_waitqueue_head(&swr->driver_wait);
	atomic_set(&swr->driver_calls, 0);
	atomic_set(&swr->scp_generation, 0);
	swr->driver_stopping = true;
	device_initialize(&swr->dev);
	swr->dev.type = &swr_dev_type;
	swr->dev.parent = &master->dev;
	swr->dev.bus = &soundwire_type;
	swr->dev.of_node = of_node_get(info->of_node);
	ret = dev_set_name(&swr->dev, "%s.%llx", swr->name, swr->addr);
	if (ret)
		goto put;
	mutex_lock(&master->mlock);
	list_add_tail(&swr->dev_list, &master->devices);
	mutex_unlock(&master->mlock);
	ret = device_add(&swr->dev);
	if (ret) {
		swr_remove_device(swr);
		goto put;
	}
	swr_master_end(master);
	return swr;
put:
	put_device(&swr->dev);
out:
	swr_master_end(master);
	return NULL;
}
EXPORT_SYMBOL(swr_new_device);

/**
 * of_register_swr_devices - register child devices on to the soundwire bus
 * @master: pointer to soundwire master device
 *
 * Registers a soundwire device for each child node of master node which has
 * a "swr-devid" property
 *
 */
int of_register_swr_devices(struct swr_master *master)
{
	struct swr_device *swr;
	struct device_node *node;

	if (!master || !master->dev.of_node)
		return -EINVAL;

	for_each_available_child_of_node(master->dev.of_node, node) {
		struct swr_boardinfo info = {};
		u64 addr;

		dev_dbg(&master->dev, "of_swr:register %s\n", node->full_name);

		if (of_alias_from_compatible(node, info.name,
				sizeof(info.name)) < 0) {
			dev_err_ratelimited(&master->dev, "of_swr:modalias failure %s\n",
				node->full_name);
			continue;
		}
		if (of_property_read_u64(node, "reg", &addr)) {
			dev_err_ratelimited(&master->dev, "of_swr:invalid reg %s\n",
				node->full_name);
			continue;
		}
		info.addr = addr;
		info.of_node = node;
		swr = swr_new_device(master, &info);
		if (!swr) {
			dev_err_ratelimited(&master->dev, "of_swr: Register failed %s\n",
				node->full_name);
			continue;
		}
	}
	return 0;
}
EXPORT_SYMBOL(of_register_swr_devices);

/**
 * swr_port_response - response from master to free the completed transaction
 * @mstr: pointer to soundwire master device
 * @tid: transaction id that indicates transaction to be freed.
 *
 * Master calls this function to free the compeleted transaction memory
 */
void swr_port_response(struct swr_master *mstr, u8 tid)
{
	struct swr_params *txn = NULL;
	unsigned long flags;

	if (!mstr)
		return;
	spin_lock_irqsave(&mstr->state_lock, flags);
	if (mstr->port_txn && tid < mstr->last_tid) {
		txn = mstr->port_txn[tid];
		mstr->port_txn[tid] = NULL;
	}
	spin_unlock_irqrestore(&mstr->state_lock, flags);
	swr_transaction_put(txn);
}
EXPORT_SYMBOL(swr_port_response);

/**
 * swr_remove_from_group - remove soundwire slave devices from group
 * @dev: pointer to the soundwire slave device
 * dev_num: device number of the soundwire slave device
 *
 * Returns error code for failure and 0 for success
 */
int swr_remove_from_group(struct swr_device *dev, u8 dev_num)
{
	struct swr_master *master;
	unsigned long flags;
	bool remove;
	int ret;

	if (!dev)
		return -EINVAL;
	master = dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	spin_lock_irqsave(&master->state_lock, flags);
	remove = dev->group_id && master->gr_sid == dev_num;
	spin_unlock_irqrestore(&master->state_lock, flags);
	if (remove && master->remove_from_group)
		master->remove_from_group(master);
	swr_master_end(master);
	return 0;
}
EXPORT_SYMBOL(swr_remove_from_group);

/**
 * swr_slvdev_datapath_control - Enables/Disables soundwire slave device
 *                               data path
 * @dev: pointer to soundwire slave device
 * @dev_num: device number of the soundwire slave device
 *
 * Returns error code for failure and 0 for success
 */
int swr_slvdev_datapath_control(struct swr_device *dev, u8 dev_num, bool enable)
{
	struct swr_master *master;
	int ret;

	if (!dev)
		return -EINVAL;
	master = dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	if (!master->slvdev_datapath_control)
		ret = -EOPNOTSUPP;
	else if (swr_group_target(dev, &dev_num, false))
		ret = master->slvdev_datapath_control(master, enable);
	else
		ret = 0;
	swr_master_end(master);
	return ret;
}
EXPORT_SYMBOL(swr_slvdev_datapath_control);

static int swr_port_request(struct swr_device *dev, u8 *port_id,
			    u8 num_port, u8 *ch_mask, u32 *ch_rate,
			    u8 *num_ch, u8 *port_type, bool connect)
{
	struct swr_master *master;
	struct swr_transaction *txn;
	int (*callback)(struct swr_master *master, struct swr_params *params);
	unsigned long flags;
	int i, ret;

	if (!dev || !port_id || !ch_mask || !port_type || !num_port ||
	    num_port > SWR_MAX_DEV_PORT_NUM || (connect && (!ch_rate || !num_ch)))
		return -EINVAL;
	master = dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	callback = connect ? master->connect_port : master->disconnect_port;
	if (!callback) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	txn = kzalloc(sizeof(*txn), GFP_KERNEL);
	if (!txn) {
		ret = -ENOMEM;
		goto out;
	}
	/* Keep callback storage alive even when the native response is synchronous. */
	refcount_set(&txn->refs, 2);
	txn->params.dev_num = dev->dev_num;
	txn->params.num_port = num_port;
	for (i = 0; i < num_port; i++) {
		txn->params.port_id[i] = port_id[i];
		txn->params.ch_en[i] = ch_mask[i];
		txn->params.port_type[i] = port_type[i];
		if (connect) {
			txn->params.num_ch[i] = num_ch[i];
			txn->params.ch_rate[i] = ch_rate[i];
		}
	}
	spin_lock_irqsave(&master->state_lock, flags);
	for (i = 0; i < master->last_tid; i++)
		if (!master->port_txn[i])
			break;
	if (i == master->last_tid) {
		spin_unlock_irqrestore(&master->state_lock, flags);
		kfree(txn);
		ret = -ENOMEM;
		goto out;
	}
	txn->params.tid = i;
	master->port_txn[i] = &txn->params;
	spin_unlock_irqrestore(&master->state_lock, flags);
	ret = callback(master, &txn->params);
	if (ret)
		swr_transaction_detach(master, &txn->params);
	swr_transaction_put(&txn->params);
out:
	swr_master_end(master);
	return ret;
}

/**
 * swr_connect_port - enable soundwire slave port(s)
 * @dev: pointer to soundwire slave device
 * @port_id: logical port id(s) of soundwire slave device
 * @num_port: number of slave device ports need to be enabled
 * @ch_mask: channels for each port that needs to be enabled
 * @ch_rate: rate at which each port/channels operate
 * @num_ch: number of channels for each port
 *
 * soundwire slave device call swr_connect_port API to enable all/some of
 * its ports and corresponding channels and channel rate. This API will
 * call master connect_port callback function to calculate frame structure
 * and enable master and slave ports
 */
int swr_connect_port(struct swr_device *dev, u8 *port_id, u8 num_port,
			 u8 *ch_mask, u32 *ch_rate, u8 *num_ch, u8 *port_type)
{
	return swr_port_request(dev, port_id, num_port, ch_mask, ch_rate,
				num_ch, port_type, true);
}
EXPORT_SYMBOL(swr_connect_port);

/**
 * swr_disconnect_port - disable soundwire slave port(s)
 * @dev: pointer to soundwire slave device
 * @port_id: logical port id(s) of soundwire slave device
 * @num_port: number of slave device ports need to be disabled
 *
 * soundwire slave device call swr_disconnect_port API to disable all/some of
 * its ports. This API will call master disconnect_port callback function to
 * disable master and slave port and (re)configure frame structure
 */
int swr_disconnect_port(struct swr_device *dev, u8 *port_id, u8 num_port,
			    u8 *ch_mask, u8 *port_type)
{
	return swr_port_request(dev, port_id, num_port, ch_mask, NULL,
				NULL, port_type, false);
}
EXPORT_SYMBOL(swr_disconnect_port);

/**
 * swr_get_logical_dev_num - Get soundwire slave logical device number
 * @dev: pointer to soundwire slave device
 * @dev_id: physical device id of soundwire slave device
 * @dev_num: pointer to logical device num of soundwire slave device
 *
 * This API will get the logical device number of soundwire slave device
 */
int swr_get_logical_dev_num(struct swr_device *dev, u64 dev_id, u8 *dev_num)
{
	struct swr_master *master;
	int ret;

	if (!dev || !dev_num)
		return -EINVAL;
	master = dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	if (master->get_logical_dev_num)
		ret = master->get_logical_dev_num(master, dev_id, dev_num);
	else
		ret = -EOPNOTSUPP;
	swr_master_end(master);
	return ret;
}
EXPORT_SYMBOL(swr_get_logical_dev_num);

/**
 * swr_init_port_params - soundwire slave port params set
 * @dev: pointer to soundwire slave device
 * @num_ports: number of slave ports
 * @pp: port params for all ports for all usecases
 *
 * This API will set soundwire port params from slave
 */
int swr_init_port_params(struct swr_device *dev, u32 num_ports, struct swr_dev_frame_config *pp)
{
	struct swr_master *master;
	int ret;

	if (!dev || !pp || !num_ports || num_ports > SWR_MAX_DEV_PORT_NUM)
		return -EINVAL;
	master = dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	if (master->init_port_params)
		ret = master->init_port_params(master, dev->dev_num, num_ports, pp);
	else
		ret = -EOPNOTSUPP;
	swr_master_end(master);
	return ret;
}
EXPORT_SYMBOL(swr_init_port_params);

/**
 * swr_device_wakeup_vote - Wakeup master and slave devices from clock stop
 * @dev: pointer to soundwire slave device
 *
 * This API will wake up soundwire master and slave device(s) from
 * clock stop.
 */
int swr_device_wakeup_vote(struct swr_device *dev)
{
	struct swr_master *master;
	int ret;

	if (!dev)
		return -EINVAL;
	master = dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	if (master->device_wakeup_vote) {
		master->device_wakeup_vote(master);
		ret = 0;
	} else {
		ret = -EOPNOTSUPP;
	}
	swr_master_end(master);
	return ret;
}
EXPORT_SYMBOL(swr_device_wakeup_vote);

/**
 * swr_device_wakeup_unvote - Unvote Wakeup so that master and slave
 * devices can go back to  clock stop
 * @dev: pointer to soundwire slave device
 *
 * This API will remove vote for wakeup so that soundwire master and
 * slave device(s) can go back to clock stop.
 */
int swr_device_wakeup_unvote(struct swr_device *dev)
{
	struct swr_master *master;
	int ret;

	if (!dev)
		return -EINVAL;
	master = dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	if (master->device_wakeup_unvote) {
		master->device_wakeup_unvote(master);
		ret = 0;
	} else {
		ret = -EOPNOTSUPP;
	}
	swr_master_end(master);
	return ret;
}
EXPORT_SYMBOL(swr_device_wakeup_unvote);

/**
 * swr_read - read soundwire slave device registers
 * @dev: pointer to soundwire slave device
 * @dev_num: logical device num of soundwire slave device
 * @reg_addr: base register address that needs to be read
 * @buf: pointer to store the values of registers from base address
 * @len: length of the buffer
 *
 * This API will read the value of the register address from
 * soundwire slave device
 */
int swr_read(struct swr_device *dev, u8 dev_num, u16 reg_addr,
	     void *buf, u32 len)
{
	struct swr_master *master;
	int ret;

	if (!dev || !buf || !len || len > 0x10000U - reg_addr)
		return -EINVAL;
	master = dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	if (master->read)
		ret = master->read(master, dev_num, reg_addr, buf, len);
	else
		ret = -EOPNOTSUPP;
	swr_master_end(master);
	return ret;
}
EXPORT_SYMBOL(swr_read);

/**
 * swr_bulk_write - write soundwire slave device registers
 * @dev: pointer to soundwire slave device
 * @dev_num: logical device num of soundwire slave device
 * @reg_addr: register address of soundwire slave device
 * @buf: contains value of register address
 * @len: indicates number of registers
 *
 * This API will write the value of the register address to
 * soundwire slave device
 */
int swr_bulk_write(struct swr_device *dev, u8 dev_num, void *reg,
		   const void *buf, size_t len)
{
	struct swr_master *master;
	int ret;

	if (!dev || !buf || !reg || !len)
		return -EINVAL;
	master = dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	if (!master->bulk_write)
		ret = -EOPNOTSUPP;
	else if (swr_group_target(dev, &dev_num, true))
		ret = master->bulk_write(master, dev_num, reg, buf, len);
	else
		ret = 0;
	swr_master_end(master);
	return ret;
}
EXPORT_SYMBOL(swr_bulk_write);

/**
 * swr_write - write soundwire slave device registers
 * @dev: pointer to soundwire slave device
 * @dev_num: logical device num of soundwire slave device
 * @reg_addr: register address of soundwire slave device
 * @buf: contains value of register address
 *
 * This API will write the value of the register address to
 * soundwire slave device
 */
int swr_write(struct swr_device *dev, u8 dev_num, u16 reg_addr,
	      const void *buf)
{
	struct swr_master *master;
	int ret;

	if (!dev || !buf)
		return -EINVAL;
	master = dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	if (!master->write)
		ret = -EOPNOTSUPP;
	else if (swr_group_target(dev, &dev_num, true))
		ret = master->write(master, dev_num, reg_addr, buf);
	else
		ret = 0;
	swr_master_end(master);
	return ret;
}
EXPORT_SYMBOL(swr_write);

/**
 * swr_device_up - Function to bringup the soundwire slave device
 * @swr_dev: pointer to soundwire slave device
 * Context: can sleep
 *
 * This API will be called by soundwire master to bringup the slave
 * device.
 */
int swr_device_up(struct swr_device *swr_dev)
{
	return swr_driver_event(swr_dev, 0);
}
EXPORT_SYMBOL(swr_device_up);

/**
 * swr_device_down - Function to call soundwire slave device down
 * @swr_dev: pointer to soundwire slave device
 * Context: can sleep
 *
 * This API will be called by soundwire master to put slave device in
 * shutdown state.
 */
int swr_device_down(struct swr_device *swr_dev)
{
	return swr_driver_event(swr_dev, 1);
}
EXPORT_SYMBOL(swr_device_down);

/**
 * swr_reset_device - reset soundwire slave device
 * @swr_dev: pointer to soundwire slave device
 * Context: can sleep
 *
 * This API will be called by soundwire master to reset the slave
 * device when the slave device is not responding or in undefined
 * state
 */
int swr_reset_device(struct swr_device *swr_dev)
{
	return swr_driver_event(swr_dev, 2);
}
EXPORT_SYMBOL(swr_reset_device);

/**
 * swr_set_device_group - Assign group id to the slave devices
 * @swr_dev: pointer to soundwire slave device
 * @id: group id to be assigned to slave device
 * Context: can sleep
 *
 * This API will be called either from soundwire master or slave
 * device to assign group id.
 */
int swr_set_device_group(struct swr_device *swr_dev, u8 id)
{
	struct swr_master *master;
	unsigned long flags;
	int ret;

	if (!swr_dev || (id != SWR_GROUP_NONE && id != SWR_GROUP_12 &&
			 id != SWR_GROUP_13 && id != SWR_BROADCAST))
		return -EINVAL;
	master = swr_dev->master;
	ret = swr_master_begin(master);
	if (ret)
		return ret;
	spin_lock_irqsave(&master->state_lock, flags);
	swr_dev->group_id = id;
	if (!id)
		master->gr_sid = 0;
	spin_unlock_irqrestore(&master->state_lock, flags);
	swr_master_end(master);
	return 0;
}
EXPORT_SYMBOL(swr_set_device_group);

static int swr_drv_probe(struct device *dev)
{
	struct swr_driver *sdrv = to_swr_driver(dev->driver);
	struct swr_device *swr = to_swr_device(dev);
	unsigned long flags;
	int ret;

	if (!sdrv || !sdrv->probe)
		return -ENODEV;
	/* Probe I/O may synchronously wake this device through the controller. */
	spin_lock_irqsave(&swr->driver_lock, flags);
	swr->driver = sdrv;
	swr->driver_stopping = false;
	spin_unlock_irqrestore(&swr->driver_lock, flags);
	atomic_inc(&swr->scp_generation);
	ret = sdrv->probe(swr);
	if (ret)
		swr_driver_drain(swr);
	return ret;
}


static int swr_drv_remove(struct device *dev)
{
	struct swr_driver *sdrv = to_swr_driver(dev->driver);
	struct swr_device *swr = to_swr_device(dev);

	swr_driver_drain(swr);
	if (sdrv && sdrv->remove)
		return sdrv->remove(swr);
	return 0;
}


static void swr_drv_shutdown(struct device *dev)
{
	const struct swr_driver *sdrv = to_swr_driver(dev->driver);

	if (!sdrv)
		return;

	if (sdrv->shutdown)
		sdrv->shutdown(to_swr_device(dev));
}

/**
 * swr_driver_register - register a soundwire driver
 * @drv: the driver to register
 * Context: can sleep
 */
int swr_driver_register(struct swr_driver *drv)
{
	if (!drv || !drv->probe)
		return -EINVAL;
	drv->driver.bus = &soundwire_type;
	drv->driver.probe = swr_drv_probe;
	drv->driver.remove = swr_drv_remove;

	if (drv->shutdown)
		drv->driver.shutdown = swr_drv_shutdown;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(swr_driver_register);

/**
 * swr_driver_unregister - unregister a soundwire driver
 * @drv: the driver to unregister
 */
void swr_driver_unregister(struct swr_driver *drv)
{
	if (drv)
		driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(swr_driver_unregister);

static void swr_match_ctrl_to_boardinfo(struct swr_master *master,
				struct swr_boardinfo *bi)
{
	struct swr_device *swr;

	if (master->bus_num != bi->bus_num) {
		dev_dbg(&master->dev,
			"%s: master# %d and bi# %d does not match\n",
			__func__, master->bus_num, bi->bus_num);
		return;
	}

	swr = swr_new_device(master, bi);
	if (!swr)
		dev_err(&master->dev, "can't create new device for %s\n",
			bi->name);
}

/**
 * swr_master_add_boarddevices - Add devices registered by board info
 * @master: master to which these devices are to be added to.
 *
 * This API is called by master when it is up and running. If devices
 * on a master were registered before master, this will make sure that
 * they get probed when master is up.
 */
void swr_master_add_boarddevices(struct swr_master *master)
{
	struct boardinfo *bi;

	if (swr_master_begin(master))
		return;
	mutex_lock(&board_lock);
	if (list_empty(&master->list))
		list_add_tail(&master->list, &swr_master_list);
	list_for_each_entry(bi, &board_list, list)
		swr_match_ctrl_to_boardinfo(master, &bi->board_info);
	mutex_unlock(&board_lock);
	swr_master_end(master);
}
EXPORT_SYMBOL(swr_master_add_boarddevices);

struct swr_device *get_matching_swr_slave_device(struct device_node *np)
{
	struct swr_device *swr = NULL;
	struct swr_master *master;
	bool found = false;

	mutex_lock(&board_lock);
	list_for_each_entry(master, &swr_master_list, list) {
		mutex_lock(&master->mlock);
		list_for_each_entry(swr, &master->devices, dev_list) {
			if (np && swr->dev.of_node == np && get_device(&swr->dev)) {
				found = true;
				mutex_unlock(&master->mlock);
				goto exit;
			}
		}
		mutex_unlock(&master->mlock);
	}
exit:
	mutex_unlock(&board_lock);
	if (!found)
		return NULL;
	return swr;
}
EXPORT_SYMBOL(get_matching_swr_slave_device);

static void swr_unregister_device(struct swr_device *swr)
{
	if (swr) {
		swr_remove_device(swr);
		device_unregister(&swr->dev);
	}
}


static void swr_master_release(struct device *dev)
{
	struct swr_master *master = to_swr_master(dev);

	/* Controllers retain their allocation until this final device release. */
	if (master->release)
		master->release(master);
}


#define swr_master_attr_gr NULL
static struct device_type swr_master_type = {
	.groups     = swr_master_attr_gr,
	.release    = swr_master_release,
};

static int __unregister(struct device *dev, void *null)
{
	swr_unregister_device(to_swr_device(dev));
	return 0;
}

/**
 * swr_unregister_master - unregister soundwire master controller
 * @master: the master being unregistered
 *
 * This API is called by master controller driver to unregister
 *  master controller that was registered by swr_register_master API.
 */
void swr_unregister_master(struct swr_master *master)
{
	struct swr_params **pending;
	unsigned long flags;
	int i;

	if (!master)
		return;
	mutex_lock(&swr_lock);
	if (idr_find(&master_idr, master->bus_num) != master) {
		mutex_unlock(&swr_lock);
		return;
	}
	idr_remove(&master_idr, master->bus_num);
	mutex_unlock(&swr_lock);
	spin_lock_irqsave(&master->state_lock, flags);
	master->stopping = true;
	spin_unlock_irqrestore(&master->state_lock, flags);
	wait_event(master->call_wait, !atomic_read(&master->active_calls));
	mutex_lock(&board_lock);
	list_del_init(&master->list);
	mutex_unlock(&board_lock);
	device_for_each_child(&master->dev, NULL, __unregister);
	spin_lock_irqsave(&master->state_lock, flags);
	pending = master->port_txn;
	master->port_txn = NULL;
	master->last_tid = 0;
	spin_unlock_irqrestore(&master->state_lock, flags);
	for (i = 0; i < 255; i++)
		swr_transaction_put(pending[i]);
	kfree(pending);
	device_unregister(&master->dev);
}
EXPORT_SYMBOL(swr_unregister_master);

/**
 * swr_register_master - register soundwire master controller
 * @master: master to be registered
 *
 * This API will register master with the framework. master->bus_num
 * is the desired number with which soundwire framework registers the
 * master.
 */
int swr_register_master(struct swr_master *master)
{
	int id, ret;

	if (!master)
		return -EINVAL;
	mutex_init(&master->mlock);
	spin_lock_init(&master->state_lock);
	init_waitqueue_head(&master->call_wait);
	atomic_set(&master->active_calls, 0);
	INIT_LIST_HEAD(&master->devices);
	INIT_LIST_HEAD(&master->list);
	master->stopping = true;
	master->gr_sid = 0;
	device_initialize(&master->dev);
	master->dev.bus = &soundwire_type;
	master->dev.type = &swr_master_type;
	master->port_txn = kcalloc(255, sizeof(*master->port_txn), GFP_KERNEL);
	if (!master->port_txn) {
		ret = -ENOMEM;
		goto put;
	}
	master->last_tid = 255;
	mutex_lock(&swr_lock);
	id = of_alias_get_id(master->dev.of_node, "swr");
	if (id >= 0)
		master->bus_num = id;
	if (master->bus_num >= INT_MAX)
		id = -EINVAL;
	else
		id = idr_alloc(&master_idr, master, master->bus_num,
			       master->bus_num + 1, GFP_KERNEL);
	mutex_unlock(&swr_lock);
	if (id < 0) {
		kfree(master->port_txn);
		master->port_txn = NULL;
		master->last_tid = 0;
		ret = id;
		goto put;
	}
	master->bus_num = id;
	ret = dev_set_name(&master->dev, "swr%u", master->bus_num);
	if (!ret)
		ret = device_add(&master->dev);
	if (!ret) {
		master->stopping = false;
		return 0;
	}
	mutex_lock(&swr_lock);
	idr_remove(&master_idr, master->bus_num);
	mutex_unlock(&swr_lock);
	master->stopping = true;
	kfree(master->port_txn);
	master->port_txn = NULL;
	master->last_tid = 0;
put:
	put_device(&master->dev);
	return ret;
}
EXPORT_SYMBOL(swr_register_master);

#define swr_device_attr_gr NULL
#define swr_device_uevent NULL
static struct device_type swr_dev_type = {
	.groups    = swr_device_attr_gr,
	.uevent    = swr_device_uevent,
	.release   = swr_dev_release,
};

static const struct swr_device_id *swr_match(const struct swr_device_id *id,
					     const struct swr_device *swr_dev)
{
	while (id->name[0]) {
		if (strcmp(swr_dev->name, id->name) == 0)
			return id;
		id++;
	}
	return NULL;
}

static int swr_device_match(struct device *dev, const struct device_driver *driver)
{
	struct swr_device *swr_dev;
	const struct swr_driver *drv = container_of_const(driver, struct swr_driver, driver);

	if (!drv)
		return -EINVAL;

	if (dev->type == &swr_dev_type) {
		swr_dev = to_swr_device(dev);
		if (!swr_dev)
			return -EINVAL;
	} else {
		return 0;
	}

	if (drv->id_table)
		return swr_match(drv->id_table, swr_dev) != NULL;

	if (driver->name)
		return strcmp(swr_dev->name, driver->name) == 0;
	return 0;
}
#ifdef CONFIG_PM_SLEEP
static int swr_legacy_suspend(struct device *dev, pm_message_t mesg)
{
	struct swr_device *swr_dev = NULL;
	struct swr_driver *driver;

	if (dev->type == &swr_dev_type)
		swr_dev = to_swr_device(dev);

	if (!swr_dev || !dev->driver)
		return 0;

	driver = to_swr_driver(dev->driver);
	if (!driver->suspend)
		return 0;

	return driver->suspend(swr_dev, mesg);
}

static int swr_legacy_resume(struct device *dev)
{
	struct swr_device *swr_dev = NULL;
	struct swr_driver *driver;

	if (dev->type == &swr_dev_type)
		swr_dev = to_swr_device(dev);

	if (!swr_dev || !dev->driver)
		return 0;

	driver = to_swr_driver(dev->driver);
	if (!driver->resume)
		return 0;

	return driver->resume(swr_dev);
}

static int swr_pm_suspend(struct device *dev)
{
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	if (pm)
		return pm_generic_suspend(dev);
	else
		return swr_legacy_suspend(dev, PMSG_SUSPEND);
}

static int swr_pm_resume(struct device *dev)
{
	const struct dev_pm_ops *pm = dev->driver ? dev->driver->pm : NULL;

	if (pm)
		return pm_generic_resume(dev);
	else
		return swr_legacy_resume(dev);
}
#else
#define swr_pm_suspend	NULL
#define swr_pm_resume	NULL
#endif /*CONFIG_PM_SLEEP*/

static const struct dev_pm_ops soundwire_pm = {
	.suspend = swr_pm_suspend,
	.resume = swr_pm_resume,
	SET_RUNTIME_PM_OPS(
		pm_generic_suspend,
		pm_generic_resume,
		NULL
		)
};

static void soundwire_dev_release(struct device *dev)
{
}

struct device soundwire_dev = {
	.release = soundwire_dev_release,
	.init_name = "soundwire",
};

struct bus_type soundwire_type = {
	.name		= "soundwire",
	.match		= swr_device_match,
	.pm		= &soundwire_pm,
};
EXPORT_SYMBOL(soundwire_type);

static void __exit soundwire_exit(void)
{
	device_unregister(&soundwire_dev);
	bus_unregister(&soundwire_type);
}

static int __init soundwire_init(void)
{
	int retval;

	retval = bus_register(&soundwire_type);
	if (retval)
		return retval;
	retval = device_register(&soundwire_dev);
	if (retval) {
		put_device(&soundwire_dev);
		bus_unregister(&soundwire_type);
	}

	return retval;
}
module_init(soundwire_init);
module_exit(soundwire_exit);


MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Soundwire module");
MODULE_ALIAS("platform:soundwire");
