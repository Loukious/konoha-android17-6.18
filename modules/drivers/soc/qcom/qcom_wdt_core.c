// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/irqdomain.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/percpu.h>
#include <linux/of.h>
#include <linux/cpu_pm.h>
#include <linux/platform_device.h>
#include <linux/wait.h>
#include <linux/reboot.h>
#include <linux/panic_notifier.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <soc/qcom/minidump.h>
#include <soc/qcom/watchdog.h>
#include <linux/cpumask.h>
#include <linux/cpu_pm.h>
#include <uapi/linux/sched/types.h>
#include <linux/sched/clock.h>
#include <linux/irq.h>
#include <linux/sort.h>
#include <linux/kernel_stat.h>
#include <linux/kallsyms.h>
#include <linux/kdebug.h>
#include <asm/hardirq.h>
#include <linux/suspend.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/rcupdate.h>

static bool boot_trace;
module_param(boot_trace, bool, 0400);
MODULE_PARM_DESC(boot_trace, "Log boot initialization checkpoints");

static void qcom_wdt_boot_checkpoint(struct device *dev, const char *stage, int ret)
{
	if (boot_trace)
		dev_info(dev, "Konoha boot: %s ret=%d\n", stage, ret);
}

#define MASK_SIZE        32

static struct msm_watchdog_data __rcu *wdog_data;
static DEFINE_MUTEX(wdog_register_lock);

static void qcom_wdt_dump_cpu_alive_mask(struct msm_watchdog_data *wdog_dd)
{
	static char alive_mask_buf[MASK_SIZE];

	scnprintf(alive_mask_buf, MASK_SIZE, "%*pb1", cpumask_pr_args(
				&wdog_dd->alive_mask));
	dev_info(wdog_dd->dev, "cpu alive mask from last pet %s\n",
				alive_mask_buf);
}

#ifdef CONFIG_QCOM_IRQ_STAT
static void record_irq_count(void)
{
	int irq, ipi_nr;
	unsigned int count;
	struct irq_desc *desc;

	ipi_nr = nr_ipi_get();

	rcu_read_lock();
	for_each_irq_nr(irq) {
		if (irq <= ipi_nr)
			continue;

		desc = irq_to_desc(irq);
		if (!desc)
			continue;

		count = kstat_irqs_usr(irq);
		WRITE_ONCE(desc->android_vendor_data1, count);
	}
	rcu_read_unlock();
}

struct irq_info {
	int irq;
	unsigned int count;
};

static void compute_irq_count(struct msm_watchdog_data *wdog_dd)
{
	int i, irq, ipi_nr;
	unsigned int count, diff;
	struct irq_desc *desc;
	unsigned long time_diff;
	struct irq_info irq_info_list[NR_TOP_HITTERS] = {};
	unsigned int list_min_cnt = 0, list_min_pos = 0;

	ipi_nr = nr_ipi_get();
	time_diff = (sched_clock() - wdog_dd->last_pet) / 1000000;

	rcu_read_lock();
	for_each_irq_nr(irq) {
		if (irq <= ipi_nr)
			continue;

		desc = irq_to_desc(irq);
		if (!desc)
			continue;

		count = kstat_irqs_usr(irq);
		diff = count - READ_ONCE(desc->android_vendor_data1);

		if (diff > list_min_cnt) {
			irq_info_list[list_min_pos].count = diff;
			irq_info_list[list_min_pos].irq = irq;

			list_min_pos = 0;
			list_min_cnt = irq_info_list[0].count;
			for (i = 1; i < NR_TOP_HITTERS; i++) {
				if (irq_info_list[i].count < list_min_cnt) {
					list_min_pos = i;
					list_min_cnt = irq_info_list[i].count;
				}
			}
		}
	}
	rcu_read_unlock();

	pr_emerg("Top irqs in last %lu ms:\n", time_diff);
	for (i = 0; i < NR_TOP_HITTERS; i++) {
		count = irq_info_list[i].count;
		if (count != 0) {
			unsigned long flags;

			irq = irq_info_list[i].irq;

			rcu_read_lock();
			desc = irq_to_desc(irq);
			if (!desc || !raw_spin_trylock_irqsave(&desc->lock, flags)) {
				rcu_read_unlock();
				continue;
			}
			pr_emerg("IRQ %d [%s:%s] - %d times\n", irq,
				(desc->irq_data.chip) ?
				desc->irq_data.chip->name : "UNKNOWN",
				(desc->action) ?
				desc->action->name : "NONAME",
				count);
			raw_spin_unlock_irqrestore(&desc->lock, flags);
			rcu_read_unlock();
		}
	}
}
#else
static void record_irq_count(void) { }
static void compute_irq_count(struct msm_watchdog_data *wdog_dd) { }
#endif

static int qcom_wdt_hibernation_notifier(struct notifier_block *nb,
				unsigned long event, void *dummy)
{
	struct msm_watchdog_data *wdog_dd =
		container_of(nb, struct msm_watchdog_data, pm_blk);

	if (event == PM_HIBERNATION_PREPARE)
		wdog_dd->hibernate = true;
	else if (event == PM_POST_HIBERNATION)
		wdog_dd->hibernate = false;
	return NOTIFY_OK;
}

#ifdef CONFIG_PM_SLEEP
int qcom_wdt_pet_suspend(struct device *dev)
{
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);
	int ret = 0;

	guard(mutex)(&wdog_dd->disable_lock);
	if (!wdog_dd->active)
		return 0;
	spin_lock(&wdog_dd->freeze_lock);
	wdog_dd->freeze_in_progress = true;
	spin_unlock(&wdog_dd->freeze_lock);
	timer_delete_sync(&wdog_dd->pet_timer);
	timer_delete_sync(&wdog_dd->user_pet_timer);
	ret = wdog_dd->ops->reset_wdt(wdog_dd);
	if (!ret && (!wdog_dd->wakeup_irq_enable || wdog_dd->hibernate ||
		     pm_suspend_target_state == PM_SUSPEND_MEM ||
		     pm_suspend_target_state == PM_SUSPEND_TO_IDLE)) {
		ret = wdog_dd->ops->disable_wdt(wdog_dd);
		if (!ret)
			wdog_dd->enabled = false;
	}
	if (ret) {
		spin_lock(&wdog_dd->freeze_lock);
		wdog_dd->freeze_in_progress = false;
		mod_timer(&wdog_dd->pet_timer,
			  jiffies + msecs_to_jiffies(wdog_dd->pet_time));
		spin_unlock(&wdog_dd->freeze_lock);
		if (wdog_dd->user_pet_enabled)
			mod_timer(&wdog_dd->user_pet_timer,
				  jiffies + msecs_to_jiffies(wdog_dd->bark_time + 3000));
	}
	wdog_dd->last_pet = sched_clock();
	return ret;
}
EXPORT_SYMBOL(qcom_wdt_pet_suspend);

int qcom_wdt_pet_resume(struct device *dev)
{
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);
	u32 val = BIT(EN);
	int ret;

	guard(mutex)(&wdog_dd->disable_lock);
	if (!wdog_dd->active)
		return 0;
	if (!wdog_dd->enabled) {
		ret = wdog_dd->ops->set_bark_time(wdog_dd->bark_time, wdog_dd);
		if (ret)
			return ret;
		ret = wdog_dd->ops->set_bite_time(wdog_dd->bark_time + 3000, wdog_dd);
		if (ret)
			return ret;
		if (wdog_dd->wakeup_irq_enable)
			val |= BIT(UNMASKED_INT_EN);
		ret = wdog_dd->ops->enable_wdt(val, wdog_dd);
		if (ret)
			return ret;
		wdog_dd->enabled = true;
	}
	ret = wdog_dd->ops->reset_wdt(wdog_dd);
	wdog_dd->last_pet = sched_clock();
	spin_lock(&wdog_dd->freeze_lock);
	wdog_dd->freeze_in_progress = false;
	mod_timer(&wdog_dd->pet_timer,
		  jiffies + msecs_to_jiffies(wdog_dd->pet_time));
	spin_unlock(&wdog_dd->freeze_lock);
	if (wdog_dd->user_pet_enabled)
		mod_timer(&wdog_dd->user_pet_timer,
			  jiffies + msecs_to_jiffies(wdog_dd->bark_time + 3000));
	return ret;
}
EXPORT_SYMBOL(qcom_wdt_pet_resume);
#endif

static void qcom_wdt_reset_on_oops(struct msm_watchdog_data *wdog_dd,
			int timeout)
{
	u32 timeout_ms = min_t(u64, ((u64)max(timeout, 0) + 10) * 1000,
			       wdog_dd->max_timeout_ms);

	wdog_dd->ops->reset_wdt(wdog_dd);
	wdog_dd->ops->set_bark_time(timeout_ms, wdog_dd);
	wdog_dd->ops->set_bite_time(timeout_ms, wdog_dd);
}

static int qcom_wdt_panic_handler(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	struct msm_watchdog_data *wdog_dd = container_of(this,
				struct msm_watchdog_data, panic_blk);

	if (!READ_ONCE(wdog_dd->active))
		return NOTIFY_DONE;
	wdog_dd->in_panic = true;
	if (WDOG_BITE_EARLY_PANIC) {
		pr_info("Triggering early bite\n");
		qcom_wdt_trigger_bite();
	}
	if (panic_timeout == 0)
		wdog_dd->ops->disable_wdt(wdog_dd);
	else
		qcom_wdt_reset_on_oops(wdog_dd, panic_timeout);

	return NOTIFY_DONE;
}

#ifdef CONFIG_QCOM_MINIDUMP_PANIC_DUMP
static int qcom_wdt_die_handler(struct notifier_block *this,
				unsigned long val, void *data)
{
	struct msm_watchdog_data *wdog_dd = container_of(this,
				struct msm_watchdog_data, die_blk);

	if (!READ_ONCE(wdog_dd->active))
		return NOTIFY_DONE;
	qcom_wdt_reset_on_oops(wdog_dd, 5);
	return NOTIFY_DONE;
}

static int qcom_wdt_register_die_notifier(struct msm_watchdog_data *wdog_dd)
{
	wdog_dd->die_blk.notifier_call = qcom_wdt_die_handler;
	wdog_dd->die_blk.priority = INT_MAX - 1;
	return register_die_notifier(&wdog_dd->die_blk);
}

static void qcom_wdt_unregister_die_notifier(struct msm_watchdog_data *wdog_dd)
{
	unregister_die_notifier(&wdog_dd->die_blk);
}
#else
static int qcom_wdt_register_die_notifier(struct msm_watchdog_data *wdog_dd)
{
	return 0;
}
static void qcom_wdt_unregister_die_notifier(struct msm_watchdog_data *wdog_dd) { }
#endif

static void qcom_wdt_release_resources(struct msm_watchdog_data *wdog_dd)
{
	WRITE_ONCE(wdog_dd->active, false);
	timer_shutdown_sync(&wdog_dd->pet_timer);
	timer_shutdown_sync(&wdog_dd->user_pet_timer);
	if (wdog_dd->watchdog_task) {
		kthread_stop(wdog_dd->watchdog_task);
		wdog_dd->watchdog_task = NULL;
	}
	if (wdog_dd->cpu_pm_registered)
		cpu_pm_unregister_notifier(&wdog_dd->wdog_cpu_pm_nb);
	if (wdog_dd->restart_registered)
		unregister_restart_handler(&wdog_dd->restart_blk);
	if (wdog_dd->die_registered)
		qcom_wdt_unregister_die_notifier(wdog_dd);
	if (wdog_dd->panic_registered)
		atomic_notifier_chain_unregister(&panic_notifier_list, &wdog_dd->panic_blk);
	if (wdog_dd->pm_registered)
		unregister_pm_notifier(&wdog_dd->pm_blk);
	if (wdog_dd->hp_state >= 0)
		cpuhp_remove_state(wdog_dd->hp_state);
	if (wdog_dd->irq_requested) {
		if (wdog_dd->irq_ppi)
			free_percpu_irq(wdog_dd->bark_irq, wdog_dd->wdog_cpu_dd);
		else
			devm_free_irq(wdog_dd->dev, wdog_dd->bark_irq, wdog_dd);
	}
	free_percpu(wdog_dd->wdog_cpu_dd);
	wdog_dd->wdog_cpu_dd = NULL;
}

static int qcom_wdt_disable(struct msm_watchdog_data *wdog_dd)
{
	int ret;

	spin_lock(&wdog_dd->freeze_lock);
	ret = wdog_dd->ops->disable_wdt(wdog_dd);
	if (!ret) {
		wdog_dd->enabled = false;
		wdog_dd->freeze_in_progress = true;
	}
	spin_unlock(&wdog_dd->freeze_lock);
	if (ret)
		return ret;
	qcom_wdt_release_resources(wdog_dd);
	dev_info(wdog_dd->dev, "QCOM Apps Watchdog deactivated\n");
	return 0;
}

static int restart_wdog_handler(struct notifier_block *this,
			       unsigned long event, void *ptr)
{
	struct msm_watchdog_data *wdog_dd = container_of(this,
				struct msm_watchdog_data, restart_blk);
	if (WDOG_BITE_ON_PANIC && wdog_dd->in_panic) {
		/*
		 * Trigger a watchdog bite here and if this fails,
		 * device will take the usual restart path.
		 */
		pr_info("Triggering late bite\n");
		qcom_wdt_trigger_bite();
	}
	return NOTIFY_DONE;
}

static bool qcom_wdt_ready(struct msm_watchdog_data *wdog_dd)
{
	/* Pairs with publication after all probe resources are installed. */
	return smp_load_acquire(&wdog_dd->ready);
}

static ssize_t qcom_wdt_disable_get(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);
	int ret;
	int disable_val;

	if (!qcom_wdt_ready(wdog_dd))
		return -EAGAIN;
	mutex_lock(&wdog_dd->disable_lock);
	disable_val  = wdog_dd->enabled ? 0 : 1;
	ret = scnprintf(buf, PAGE_SIZE, "%d\n", disable_val);
	mutex_unlock(&wdog_dd->disable_lock);
	return ret;
}

static ssize_t qcom_wdt_disable_set(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);
	u8 disable;
	int ret;

	if (!qcom_wdt_ready(wdog_dd))
		return -EAGAIN;
	ret = kstrtou8(buf, 10, &disable);
	if (ret) {
		dev_err(wdog_dd->dev, "invalid user input\n");
		return ret;
	}
	if (disable == 1) {
		mutex_lock(&wdog_dd->disable_lock);
		if (!wdog_dd->active) {
			dev_err(wdog_dd->dev, "MSM Apps Watchdog already disabled\n");
			mutex_unlock(&wdog_dd->disable_lock);
			return count;
		}
		ret = qcom_scm_sec_wdog_deactivate();
		if (ret) {
			dev_err(wdog_dd->dev,
				"Failed to deactivate secure wdog, ret = %d\n",
				 ret);
		}
		if (!ret)
			ret = qcom_wdt_disable(wdog_dd);
		mutex_unlock(&wdog_dd->disable_lock);
		if (ret)
			return ret;
	} else {
		pr_err("invalid operation, only disable = 1 supported\n");
		return -EINVAL;
	}
	return count;
}

static DEVICE_ATTR(disable, 0600, qcom_wdt_disable_get, qcom_wdt_disable_set);

/*
 * Userspace Watchdog Support:
 * Write 1 to the "user_pet_enabled" file to enable hw support for a
 * userspace watchdog.
 * Userspace is required to pet the watchdog by continuing to write 1
 * to this file in the expected interval.
 * Userspace may disable this requirement by writing 0 to this same
 * file.
 */
static void __qcom_wdt_user_pet(struct msm_watchdog_data *wdog_dd)
{
	WRITE_ONCE(wdog_dd->user_pet_complete, true);
	wake_up(&wdog_dd->pet_complete);
}

static ssize_t qcom_wdt_user_pet_enabled_get(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);
	int ret;

	if (!qcom_wdt_ready(wdog_dd))
		return -EAGAIN;
	ret = scnprintf(buf, PAGE_SIZE, "%d\n",
			wdog_dd->user_pet_enabled);
	return ret;
}

static ssize_t qcom_wdt_user_pet_enabled_set(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);
	int ret;
	unsigned long delay_time = 0;
	bool already_enabled;

	if (!qcom_wdt_ready(wdog_dd))
		return -EAGAIN;
	guard(mutex)(&wdog_dd->disable_lock);
	if (!wdog_dd->active)
		return -ENODEV;
	already_enabled = wdog_dd->user_pet_enabled;
	ret = kstrtobool(buf, &wdog_dd->user_pet_enabled);
	if (ret) {
		dev_err(wdog_dd->dev, "invalid user input\n");
		return ret;
	}

	delay_time = msecs_to_jiffies(wdog_dd->bark_time + 3 * 1000);
	if (wdog_dd->user_pet_enabled)
		mod_timer(&wdog_dd->user_pet_timer, jiffies + delay_time);
	else if (already_enabled)
		timer_delete_sync(&wdog_dd->user_pet_timer);

	__qcom_wdt_user_pet(wdog_dd);

	return count;
}

static DEVICE_ATTR(user_pet_enabled, 0600, qcom_wdt_user_pet_enabled_get,
						qcom_wdt_user_pet_enabled_set);

static ssize_t qcom_wdt_pet_time_get(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);
	int ret;

	if (!qcom_wdt_ready(wdog_dd))
		return -EAGAIN;
	ret = scnprintf(buf, PAGE_SIZE, "%d\n", wdog_dd->pet_time);
	return ret;
}

static DEVICE_ATTR(pet_time, 0400, qcom_wdt_pet_time_get, NULL);

static ssize_t wakeup_enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);

	if (!qcom_wdt_ready(wdog_dd))
		return -EAGAIN;
	return scnprintf(buf, PAGE_SIZE, "%d\n", wdog_dd->wakeup_irq_enable);
}
static ssize_t wakeup_enable_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct msm_watchdog_data *wdog_dd = dev_get_drvdata(dev);
	u8 enable;
	int ret;

	if (!qcom_wdt_ready(wdog_dd))
		return -EAGAIN;
	ret = kstrtou8(buf, 10, &enable);
	if (ret || enable > 1) {
		dev_err(wdog_dd->dev, "invalid user input\n");
		return ret ? : -EINVAL;
	}
	guard(mutex)(&wdog_dd->disable_lock);
	if (!wdog_dd->active)
		return -ENODEV;
	/* echo 1 > wakeup_enable means wakeup irq is enabled */
	wdog_dd->wakeup_irq_enable = enable;
	if (wdog_dd->enabled) {
		u8 val = BIT(EN);

		if (wdog_dd->wakeup_irq_enable)
			val |= BIT(UNMASKED_INT_EN);
		ret = wdog_dd->ops->enable_wdt(val, wdog_dd);
		if (ret)
			return ret;
	}
	return count;
}
static DEVICE_ATTR_ADMIN_RW(wakeup_enable);

static void qcom_wdt_keep_alive_response(void *info)
{
	struct msm_watchdog_data *wdog_dd = info;
	int cpu = smp_processor_id();

	cpumask_set_cpu(cpu, &wdog_dd->alive_mask);
	wdog_dd->ping_end[cpu] = sched_clock();
	/* Make sure alive mask is cleared and set in order */
	smp_mb();
}

/*
 * If this function does not return, it implies one of the
 * other cpu's is not responsive.
 */
static void qcom_wdt_ping_other_cpus(struct msm_watchdog_data *wdog_dd)
{
	int cpu;

	cpumask_clear(&wdog_dd->alive_mask);
	/* Make sure alive mask is cleared and set in order */
	smp_mb();
	for_each_cpu(cpu, cpu_online_mask) {
		if (!READ_ONCE(wdog_dd->cpu_idle_pc_state[cpu])) {
			if (boot_trace)
				dev_info(wdog_dd->dev, "Konoha boot: wdt.ipi.enter cpu=%d\n",
					 cpu);
			wdog_dd->ping_start[cpu] = sched_clock();
			smp_call_function_single(cpu,
						 qcom_wdt_keep_alive_response,
						 wdog_dd, 1);
			if (boot_trace)
				dev_info(wdog_dd->dev, "Konoha boot: wdt.ipi.exit cpu=%d\n",
					 cpu);
		}
	}
}

static void qcom_wdt_pet_task_wakeup(struct timer_list *t)
{
	struct msm_watchdog_data *wdog_dd =
		timer_container_of(wdog_dd, t, pet_timer);
	qcom_wdt_boot_checkpoint(wdog_dd->dev, "wdt.timer", 0);
	WRITE_ONCE(wdog_dd->timer_expired, true);
	wdog_dd->timer_fired = sched_clock();
	wake_up(&wdog_dd->pet_complete);
}
static void qcom_wdt_user_pet_bite(struct timer_list *t)
{
	struct msm_watchdog_data *wdog_dd =
		timer_container_of(wdog_dd, t, user_pet_timer);
	if (!READ_ONCE(wdog_dd->user_pet_complete)) {
		dev_info(wdog_dd->dev, "QCOM Apps Watchdog user pet timeout!\n");
		qcom_wdt_trigger_bite();
	}
}

static __ref int qcom_wdt_kthread(void *arg)
{
	struct msm_watchdog_data *wdog_dd = arg;
	unsigned long delay_time = 0;
	struct sched_param param = {.sched_priority = MAX_RT_PRIO-1};
	int ret, cpu;

	sched_setscheduler(current, SCHED_FIFO, &param);
	while (!kthread_should_stop()) {
		do {
			ret = wait_event_interruptible(wdog_dd->pet_complete,
						READ_ONCE(wdog_dd->timer_expired) ||
						kthread_should_stop());
		} while (ret != 0 && !kthread_should_stop());
		if (kthread_should_stop())
			break;

		qcom_wdt_boot_checkpoint(wdog_dd->dev, "wdt.thread", 0);
		wdog_dd->thread_start = sched_clock();
		for_each_cpu(cpu, cpu_present_mask)
			wdog_dd->ping_start[cpu] = wdog_dd->ping_end[cpu] = 0;

		if (wdog_dd->do_ipi_ping)
			qcom_wdt_ping_other_cpus(wdog_dd);

		do {
			ret = wait_event_interruptible(wdog_dd->pet_complete,
						READ_ONCE(wdog_dd->user_pet_complete) ||
						kthread_should_stop());
		} while (ret != 0 && !kthread_should_stop());
		if (kthread_should_stop())
			break;

		WRITE_ONCE(wdog_dd->timer_expired, false);
		WRITE_ONCE(wdog_dd->user_pet_complete, !READ_ONCE(wdog_dd->user_pet_enabled));

		spin_lock(&wdog_dd->freeze_lock);
		if (wdog_dd->enabled && !wdog_dd->freeze_in_progress) {
			delay_time = msecs_to_jiffies(wdog_dd->pet_time);
			qcom_wdt_boot_checkpoint(wdog_dd->dev, "wdt.reset.enter", 0);
			ret = wdog_dd->ops->reset_wdt(wdog_dd);
			qcom_wdt_boot_checkpoint(wdog_dd->dev, "wdt.reset.exit", ret);
			wdog_dd->last_pet = sched_clock();
		}
		/* Check again before scheduling
		 * Could have been changed on other cpu
		 */
		if (!kthread_should_stop()) {
			if (!wdog_dd->freeze_in_progress)
				mod_timer(&wdog_dd->pet_timer,
					jiffies + delay_time);
		}
		spin_unlock(&wdog_dd->freeze_lock);

		record_irq_count();
	}
	return 0;
}

static int qcom_wdt_cpu_pm_notify(struct notifier_block *this,
			      unsigned long action, void *v)
{
	struct msm_watchdog_data *wdog_dd = container_of(this,
				struct msm_watchdog_data, wdog_cpu_pm_nb);
	int cpu;

	cpu = raw_smp_processor_id();

	switch (action) {
	case CPU_PM_ENTER:
		WRITE_ONCE(wdog_dd->cpu_idle_pc_state[cpu], 1);
		break;
	case CPU_PM_ENTER_FAILED:
	case CPU_PM_EXIT:
		WRITE_ONCE(wdog_dd->cpu_idle_pc_state[cpu], 0);
		break;
	}

	return NOTIFY_OK;
}


/**
 * qcom_wdt_trigger_bite - Executes a watchdog bite.
 *
 * Return: function will not return, to allow for the bite to occur
 */
void qcom_wdt_trigger_bite(void)
{
	struct msm_watchdog_data *wdog_dd;

	rcu_read_lock();
	wdog_dd = rcu_dereference(wdog_data);
	if (!wdog_dd || !READ_ONCE(wdog_dd->active)) {
		rcu_read_unlock();
		return;
	}
	compute_irq_count(wdog_dd);
	dev_emerg(wdog_dd->dev, "Causing a QCOM Apps Watchdog bite!\n");
	wdog_dd->ops->show_wdt_status(wdog_dd);
	wdog_dd->ops->set_bite_time(1, wdog_dd);
	wdog_dd->ops->reset_wdt(wdog_dd);
	/* Keep the data pinned until hardware completes the reset. */
	mdelay(10000);
	while (1)
		udelay(1);
}
EXPORT_SYMBOL(qcom_wdt_trigger_bite);

static irqreturn_t qcom_wdt_bark_handler(int irq, void *dev_id)
{
	struct msm_watchdog_data *wdog_dd = dev_id;
	unsigned long nanosec_rem;
	unsigned long long t = sched_clock();
	unsigned long long tp = wdog_dd->last_pet;

	if (!READ_ONCE(wdog_dd->active))
		return IRQ_HANDLED;
	nanosec_rem = do_div(t, 1000000000);
	dev_info(wdog_dd->dev, "QCOM Apps Watchdog bark! Now = %lu.%06lu\n",
			(unsigned long) t, nanosec_rem / 1000);

	nanosec_rem = do_div(tp, 1000000000);
	dev_info(wdog_dd->dev, "QCOM Apps Watchdog last pet at %lu.%06lu\n",
			(unsigned long) tp, nanosec_rem / 1000);
	if (wdog_dd->do_ipi_ping)
		qcom_wdt_dump_cpu_alive_mask(wdog_dd);

	if (wdog_dd->freeze_in_progress)
		dev_info(wdog_dd->dev, "Suspend in progress\n");

	md_dump_process();
	qcom_wdt_trigger_bite();

	return IRQ_HANDLED;
}

static irqreturn_t qcom_wdt_ppi_bark(int irq, void *dev_id_percpu)
{
	void *dev_id = raw_cpu_ptr((void __percpu *)dev_id_percpu);
	struct msm_watchdog_data *wdog_dd = *((struct msm_watchdog_data **)dev_id);

	return qcom_wdt_bark_handler(irq, wdog_dd);
}

static umode_t qcom_wdt_attr_visible(struct kobject *kobj,
				     struct attribute *attr, int index)
{
	if (!QCOM_WATCHDOG_USERSPACE_PET &&
	    (attr == &dev_attr_pet_time.attr || attr == &dev_attr_user_pet_enabled.attr))
		return 0;
	return attr->mode;
}

static struct attribute *qcom_wdt_attrs[] = {
	&dev_attr_disable.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_pet_time.attr,
	&dev_attr_user_pet_enabled.attr,
	NULL,
};

static const struct attribute_group qcom_wdt_group = {
	.attrs = qcom_wdt_attrs,
	.is_visible = qcom_wdt_attr_visible,
};

static int qcom_wdt_cpu_online(unsigned int cpu)
{
	struct msm_watchdog_data *wdog_dd = rcu_access_pointer(wdog_data);

	enable_percpu_irq(wdog_dd->bark_irq, 0);
	return 0;
}

static int qcom_wdt_cpu_offline(unsigned int cpu)
{
	struct msm_watchdog_data *wdog_dd = rcu_access_pointer(wdog_data);

	disable_percpu_irq(wdog_dd->bark_irq);
	return 0;
}

static int qcom_wdt_init(struct msm_watchdog_data *wdog_dd,
			 struct platform_device *pdev)
{
	u32 val = BIT(EN);
	int ret, cpu;

	if (wdog_dd->irq_ppi) {
		wdog_dd->wdog_cpu_dd = alloc_percpu(struct msm_watchdog_data *);
		if (!wdog_dd->wdog_cpu_dd)
			return -ENOMEM;
		for_each_possible_cpu(cpu) {
			struct msm_watchdog_data **cpu_dd;

			cpu_dd = per_cpu_ptr(wdog_dd->wdog_cpu_dd, cpu);
			*cpu_dd = wdog_dd;
		}
		ret = request_percpu_irq(wdog_dd->bark_irq, qcom_wdt_ppi_bark,
					"apps_wdog_bark", wdog_dd->wdog_cpu_dd);
	} else {
		ret = devm_request_irq(wdog_dd->dev, wdog_dd->bark_irq,
				       qcom_wdt_bark_handler,
				       IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND,
				       "apps_wdog_bark", wdog_dd);
	}
	if (ret)
		return ret;
	wdog_dd->irq_requested = true;
	wdog_dd->pm_blk.notifier_call = qcom_wdt_hibernation_notifier;
	ret = register_pm_notifier(&wdog_dd->pm_blk);
	if (ret)
		return ret;
	wdog_dd->pm_registered = true;
	ret = wdog_dd->ops->set_bark_time(wdog_dd->bark_time, wdog_dd);
	if (ret)
		return ret;
	ret = wdog_dd->ops->set_bite_time(wdog_dd->bark_time + 3000, wdog_dd);
	if (ret)
		return ret;
	wdog_dd->panic_blk.priority = INT_MAX - 1;
	wdog_dd->panic_blk.notifier_call = qcom_wdt_panic_handler;
	ret = atomic_notifier_chain_register(&panic_notifier_list, &wdog_dd->panic_blk);
	if (ret)
		return ret;
	wdog_dd->panic_registered = true;
	ret = qcom_wdt_register_die_notifier(wdog_dd);
	if (ret)
		return ret;
	wdog_dd->die_registered = true;
	wdog_dd->restart_blk.priority = 255;
	wdog_dd->restart_blk.notifier_call = restart_wdog_handler;
	ret = register_restart_handler(&wdog_dd->restart_blk);
	if (ret)
		return ret;
	wdog_dd->restart_registered = true;
	if (!IPI_CORES_IN_LPM) {
		wdog_dd->wdog_cpu_pm_nb.notifier_call = qcom_wdt_cpu_pm_notify;
		ret = cpu_pm_register_notifier(&wdog_dd->wdog_cpu_pm_nb);
		if (ret)
			return ret;
		wdog_dd->cpu_pm_registered = true;
	}
	if (wdog_dd->irq_ppi) {
		ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "soc/qcom:watchdog",
					qcom_wdt_cpu_online, qcom_wdt_cpu_offline);
		if (ret < 0)
			return ret;
		wdog_dd->hp_state = ret;
	}
	ret = sysfs_create_group(&pdev->dev.kobj, &qcom_wdt_group);
	if (ret)
		return ret;
	/* Sysfs callbacks reject operations until register publishes readiness. */
	if (wdog_dd->wakeup_irq_enable)
		val |= BIT(UNMASKED_INT_EN);
	ret = wdog_dd->ops->enable_wdt(val, wdog_dd);
	if (ret) {
		sysfs_remove_group(&pdev->dev.kobj, &qcom_wdt_group);
		return ret;
	}
	wdog_dd->enabled = true;
	WRITE_ONCE(wdog_dd->active, true);
	wdog_dd->ops->reset_wdt(wdog_dd);
	wdog_dd->last_pet = sched_clock();
	mod_timer(&wdog_dd->pet_timer,
		  jiffies + msecs_to_jiffies(wdog_dd->pet_time));
	wake_up_process(wdog_dd->watchdog_task);
	dev_info(wdog_dd->dev, "QCOM Apps Watchdog Initialized\n");
	return 0;
}

static void qcom_wdt_dump_pdata(struct msm_watchdog_data *pdata)
{
	dev_dbg(pdata->dev, "wdog bark_time %d", pdata->bark_time);
	dev_dbg(pdata->dev, "wdog pet_time %d", pdata->pet_time);
	dev_dbg(pdata->dev, "wdog perform ipi ping %d", pdata->do_ipi_ping);
	dev_dbg(pdata->dev, "wdog base address is 0x%lx\n", (unsigned long)
								pdata->base);
}

static int qcom_wdt_dt_to_pdata(struct platform_device *pdev,
				struct msm_watchdog_data *pdata)
{
	int irq = platform_get_irq(pdev, 0);

	if (irq < 0)
		return irq;
	pdata->bark_irq = irq;
	pdata->irq_ppi = irq_is_percpu(pdata->bark_irq);
	pdata->bark_time = QCOM_WATCHDOG_BARK_TIME;
	pdata->pet_time = QCOM_WATCHDOG_PET_TIME;
	pdata->do_ipi_ping = QCOM_WATCHDOG_IPI_PING;
	pdata->wakeup_irq_enable = QCOM_WATCHDOG_WAKEUP_ENABLE;
	qcom_wdt_dump_pdata(pdata);
	return 0;
}

int qcom_wdt_register(struct platform_device *pdev,
			struct msm_watchdog_data *wdog_dd,
			char *wdog_dd_name)
{
	struct md_region md_entry = {};
	int ret;

	if (!pdev || !wdog_dd || !wdog_dd_name || !wdog_dd->ops)
		return -EINVAL;
	guard(mutex)(&wdog_register_lock);
	if (rcu_access_pointer(wdog_data))
		return -EBUSY;
	wdog_dd->dev = &pdev->dev;
	ret = qcom_wdt_dt_to_pdata(pdev, wdog_dd);
	if (ret)
		return ret;
	if (!wdog_dd->pet_time || wdog_dd->pet_time >= wdog_dd->bark_time ||
	    wdog_dd->bark_time > wdog_dd->max_timeout_ms ||
	    wdog_dd->max_timeout_ms - wdog_dd->bark_time < 3000)
		return -EINVAL;
	mutex_init(&wdog_dd->disable_lock);
	init_waitqueue_head(&wdog_dd->pet_complete);
	spin_lock_init(&wdog_dd->freeze_lock);
	WRITE_ONCE(wdog_dd->user_pet_complete, true);
	wdog_dd->hp_state = -1;
	timer_setup(&wdog_dd->pet_timer, qcom_wdt_pet_task_wakeup, 0);
	timer_setup(&wdog_dd->user_pet_timer, qcom_wdt_user_pet_bite, 0);
	cpumask_clear(&wdog_dd->alive_mask);
	platform_set_drvdata(pdev, wdog_dd);
	rcu_assign_pointer(wdog_data, wdog_dd);
	wdog_dd->watchdog_task = kthread_create(qcom_wdt_kthread, wdog_dd, wdog_dd_name);
	if (IS_ERR(wdog_dd->watchdog_task)) {
		ret = PTR_ERR(wdog_dd->watchdog_task);
		wdog_dd->watchdog_task = NULL;
		goto fail;
	}
	ret = qcom_wdt_init(wdog_dd, pdev);
	if (ret)
		goto fail;

	strscpy(md_entry.name, "KWDOGDATA", sizeof(md_entry.name));
	md_entry.virt_addr = (uintptr_t)wdog_dd;
	md_entry.phys_addr = virt_to_phys(wdog_dd);
	md_entry.size = sizeof(*wdog_dd);
	if (msm_minidump_add_region(&md_entry) < 0)
		dev_err(wdog_dd->dev, "Failed to add Wdt data in Minidump\n");
	/* Release sysfs callbacks only after setup can no longer fail. */
	smp_store_release(&wdog_dd->ready, true);
	return 0;
fail:
	qcom_wdt_release_resources(wdog_dd);
	RCU_INIT_POINTER(wdog_data, NULL);
	synchronize_rcu();
	platform_set_drvdata(pdev, NULL);
	return ret;
}
EXPORT_SYMBOL(qcom_wdt_register);

MODULE_DESCRIPTION("QCOM Watchdog Driver Core");
MODULE_LICENSE("GPL");
