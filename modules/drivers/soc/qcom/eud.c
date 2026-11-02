// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/extcon.h>
#include <linux/extcon-provider.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/bitops.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/clk.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/delay.h>

#define EUD_ENABLE_CMD 1
#define EUD_DISABLE_CMD 0

#define EUD_REG_COM_TX_ID		0x0000
#define EUD_REG_COM_TX_LEN		0x0004
#define EUD_REG_COM_TX_DAT		0x0008
#define EUD_REG_COM_RX_ID		0x000C
#define EUD_REG_COM_RX_LEN		0x0010
#define EUD_REG_COM_RX_DAT		0x0014
#define EUD_REG_EUD_EN2			0x0000
#define EUD_REG_INT1_EN_MASK		0x0024
#define EUD_REG_INT_STATUS_1		0x0044
#define EUD_REG_CTL_OUT_1		0x0074
#define EUD_REG_VBUS_INT_CLR		0x0080
#define EUD_REG_CHGR_INT_CLR		0x0084
#define EUD_REG_CSR_EUD_EN		0x1014
#define EUD_REG_SW_ATTACH_DET		0x1018
#define EUD_REG_UTMI_DELAY_LSB		0x1030
#define EUD_REG_UTMI_DELAY_MSB		0x1034
#define EUD_CHIKNBIT_EN_DEL_CNTR	0x118C

#define EUD_INT_RX		BIT(0)
#define EUD_INT_TX		BIT(1)
#define EUD_INT_VBUS		BIT(2)
#define EUD_INT_CHGR		BIT(3)
#define EUD_INT_SAFE_MODE	BIT(4)
#define EUD_INT_ALL		(EUD_INT_RX | EUD_INT_TX | \
				EUD_INT_VBUS | EUD_INT_CHGR | \
				EUD_INT_SAFE_MODE)

#define EUD_NR			1
#define EUD_CONSOLE		NULL
#define UART_ID			0x90
#define MAX_FIFO_SIZE		14
#define EUD_UTMI_DELAY_MASK	0xff
#define EUD_UTMI_DELAY_MIN	28

#define PORT_EUD_UART		300

#define EUD_TCSR_ENABLE_BIT	BIT(0)

struct eud_chip {
	struct device			*dev;
	int				eud_irq;
	unsigned int			pending_events;
	bool				usb_attach;
	bool				chgr_enable;
	void __iomem			*eud_reg_base;
	struct extcon_dev		*extcon;
	struct uart_port		port;
	struct work_struct		eud_work;
	struct power_supply		*usb_psy;
	bool				secure_eud_en;
	bool				need_phy_clk_vote;
	phys_addr_t			eud_mode_mgr2_phys_base;
	struct clk			*eud_ahb2phy_clk;
	struct clk			*eud_clkref_clk;
	bool				eud_clkref_enabled;
	bool				eud_enabled;
	bool				ahb_enabled;
	bool				irq_registered;
	bool				irq_wake_enabled;
	bool				uart_registered;
	bool				uart_active;
	bool				removing;
	bool				suspended;
	bool				stopped;
	bool				maybe_enabled;
	u16				utmi_switch_delay;
};

static const unsigned int eud_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_CHG_USB_SDP,
	EXTCON_JIG,
	EXTCON_NONE,
};

static int enable = EUD_DISABLE_CMD;
static DEFINE_MUTEX(eud_lock);
static bool eud_claimed;
static struct platform_device *eud_private;

static int msm_eud_clkref_en(struct eud_chip *chip, bool enable)
{
	int ret = 0;

	if (chip->eud_clkref_enabled == enable)
		return 0;

	if (enable) {
		/* Vote for EUD USB reference clk, needed to keep QREF enabled */
		ret = clk_prepare_enable(chip->eud_clkref_clk);
		if (ret) {
			dev_err(chip->dev,
				"Failed to enable eud_clkref_clk\n");
			return ret;
		}
	} else {
		clk_disable_unprepare(chip->eud_clkref_clk);
	}

	chip->eud_clkref_enabled = enable;

	return 0;
}

static void msm_eud_enable_irqs(struct eud_chip *chip)
{
	unsigned long flags;
	u32 mask;

	uart_port_lock_irqsave(&chip->port, &flags);
	mask = readl_relaxed(chip->eud_reg_base + EUD_REG_INT1_EN_MASK);
	mask |= EUD_INT_VBUS | EUD_INT_CHGR | EUD_INT_SAFE_MODE;
	writel_relaxed(mask, chip->eud_reg_base + EUD_REG_INT1_EN_MASK);
	/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
	uart_port_unlock_irqrestore(&chip->port, flags);
}

static int msm_eud_hw_is_enabled(struct platform_device *pdev)
{
	struct eud_chip *chip = platform_get_drvdata(pdev);
	u32 sec_eud_enabled = 0;

	if (chip->secure_eud_en) {
		int ret = qcom_scm_io_readl(
				chip->eud_mode_mgr2_phys_base + EUD_REG_EUD_EN2,
				&sec_eud_enabled);
		if (ret) {
			dev_err(&pdev->dev,
				"qcom_scm_io_readl failed with rc: %d\n", ret);
			return ret;
		}

		return !!(sec_eud_enabled & BIT(0));
	}

	return readl_relaxed(chip->eud_reg_base + EUD_REG_CSR_EUD_EN) & BIT(0);
}

static int set_eud_utmi_switch_delay(struct eud_chip *chip)
{
	u8 val;

	if (chip->utmi_switch_delay) {
		val = (chip->utmi_switch_delay >> 8) & EUD_UTMI_DELAY_MASK;
		writew_relaxed(val, chip->eud_reg_base + EUD_REG_UTMI_DELAY_MSB);
		val = chip->utmi_switch_delay & EUD_UTMI_DELAY_MASK;
		writew_relaxed(val, chip->eud_reg_base + EUD_REG_UTMI_DELAY_LSB);
	}

	return 0;
}

static int check_eud_mode_mgr2(struct eud_chip *chip)
{
	u32 val;
	int ret;

	ret = qcom_scm_io_readl(chip->eud_mode_mgr2_phys_base, &val);
	return ret ? ret : !!(val & BIT(0));
}

static int enable_eud(struct platform_device *pdev)
{
	struct eud_chip *priv = platform_get_drvdata(pdev);
	int ret, secure_enabled = 0;

	if (priv->eud_enabled)
		return 0;
	if (priv->secure_eud_en) {
		secure_enabled = check_eud_mode_mgr2(priv);
		if (secure_enabled < 0)
			return secure_enabled;
	}
	ret = msm_eud_clkref_en(priv, true);
	if (ret)
		return ret;
	/* Preserve the vendor spoof-disconnect/connect sequence. */
	ret = extcon_set_state_sync(priv->extcon, EXTCON_USB, true);
	if (ret)
		goto release_clock;
	extcon_set_state(priv->extcon, EXTCON_JIG, false);
	ret = extcon_set_state_sync(priv->extcon, EXTCON_USB, false);
	if (ret)
		goto release_clock;
	set_eud_utmi_switch_delay(priv);
	writel_relaxed(BIT(0), priv->eud_reg_base + EUD_REG_CSR_EUD_EN);
	priv->maybe_enabled = true;
	msm_eud_enable_irqs(priv);
	if (priv->secure_eud_en && !secure_enabled) {
		ret = qcom_scm_io_writel(priv->eud_mode_mgr2_phys_base + EUD_REG_EUD_EN2,
					EUD_ENABLE_CMD);
		if (ret)
			return ret;
	}
	/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
	usleep_range(50, 100);
	ret = extcon_set_state_sync(priv->extcon, EXTCON_USB, true);
	if (ret)
		return ret;
	ret = extcon_set_state_sync(priv->extcon, EXTCON_CHG_USB_SDP, true);
	if (ret)
		return ret;
	priv->eud_enabled = true;
	return 0;
release_clock:
	if (!priv->maybe_enabled)
		msm_eud_clkref_en(priv, false);
	return ret;
}

static int disable_eud(struct platform_device *pdev)
{
	struct eud_chip *priv = platform_get_drvdata(pdev);
	int ret;

	if (!priv->eud_enabled && !priv->maybe_enabled)
		return 0;
	extcon_set_state(priv->extcon, EXTCON_JIG, false);
	ret = extcon_set_state_sync(priv->extcon, EXTCON_USB, false);
	if (ret && !priv->removing)
		return ret;
	writel_relaxed(0, priv->eud_reg_base + EUD_REG_CSR_EUD_EN);
	priv->eud_enabled = false;
	/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
	if (priv->secure_eud_en) {
		ret = qcom_scm_io_writel(priv->eud_mode_mgr2_phys_base + EUD_REG_EUD_EN2,
					EUD_DISABLE_CMD);
		if (ret)
			return ret;
	}
	priv->eud_enabled = false;
	priv->maybe_enabled = false;
	msm_eud_clkref_en(priv, false);
	usleep_range(50, 100);
	if (priv->removing)
		return 0;
	return extcon_set_state_sync(priv->extcon, EXTCON_USB, true);
}

static int param_eud_set(const char *val, const struct kernel_param *kp)
{
	struct eud_chip *chip;
	int requested, ret;

	ret = kstrtoint(val, 0, &requested);
	if (ret || (requested != EUD_ENABLE_CMD && requested != EUD_DISABLE_CMD))
		return -EINVAL;
	mutex_lock(&eud_lock);
	ret = 0;
	if (eud_private) {
		chip = platform_get_drvdata(eud_private);
		if (chip->suspended)
			ret = -EBUSY;
		else
			ret = requested ? enable_eud(eud_private) : disable_eud(eud_private);
	}
	if (!ret)
		*(int *)kp->arg = requested;
	mutex_unlock(&eud_lock);
	return ret;
}

static const struct kernel_param_ops eud_param_ops = {
	.set = param_eud_set,
	.get = param_get_int,
};

module_param_cb(enable, &eud_param_ops, &enable, 0644);

static bool is_usb_psy_available(struct eud_chip *chip)
{
	if (!chip->usb_psy)
		chip->usb_psy = power_supply_get_by_name("usb");

	if (!chip->usb_psy)
		return false;

	return true;
}

static void eud_event_notifier(struct work_struct *work)
{
	struct eud_chip *chip = container_of(work, struct eud_chip, eud_work);
	union power_supply_propval pval;
	unsigned long flags;
	unsigned int pending;
	bool attached, charging;
	int ret;

	for (;;) {
		uart_port_lock_irqsave(&chip->port, &flags);
		pending = chip->pending_events;
		chip->pending_events = 0;
		attached = chip->usb_attach;
		charging = chip->chgr_enable;
		uart_port_unlock_irqrestore(&chip->port, flags);
		if (!pending || READ_ONCE(chip->removing))
			return;
		mutex_lock(&eud_lock);
		if (!chip->removing) {
			if (pending & EUD_INT_VBUS) {
				extcon_set_state(chip->extcon, EXTCON_JIG, true);
				extcon_set_state_sync(chip->extcon, EXTCON_USB, attached);
			}
			if ((pending & EUD_INT_CHGR) && is_usb_psy_available(chip)) {
				pval.intval = charging ? -EINVAL : 0;
				ret = power_supply_set_property(chip->usb_psy,
						POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, &pval);
				if (ret)
					dev_err(chip->dev, "Power-supply update failed: %d\n", ret);
			}
		}
		mutex_unlock(&eud_lock);
	}
}

static void usb_attach_detach(struct eud_chip *chip)
{
	unsigned long flags;
	u32 reg = readl_relaxed(chip->eud_reg_base + EUD_REG_CTL_OUT_1);

	uart_port_lock_irqsave(&chip->port, &flags);
	chip->usb_attach = !!(reg & BIT(4));
	chip->pending_events |= EUD_INT_VBUS;
	uart_port_unlock_irqrestore(&chip->port, flags);
	queue_work(system_freezable_wq, &chip->eud_work);
	writel_relaxed(BIT(0), chip->eud_reg_base + EUD_REG_VBUS_INT_CLR);
	/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
	writel_relaxed(0, chip->eud_reg_base + EUD_REG_VBUS_INT_CLR);
}

static void chgr_enable_disable(struct eud_chip *chip)
{
	unsigned long flags;
	u32 reg = readl_relaxed(chip->eud_reg_base + EUD_REG_CTL_OUT_1);

	uart_port_lock_irqsave(&chip->port, &flags);
	chip->chgr_enable = !!(reg & BIT(6));
	chip->pending_events |= EUD_INT_CHGR;
	uart_port_unlock_irqrestore(&chip->port, flags);
	queue_work(system_freezable_wq, &chip->eud_work);
	writel_relaxed(BIT(0), chip->eud_reg_base + EUD_REG_CHGR_INT_CLR);
	/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
	writel_relaxed(0, chip->eud_reg_base + EUD_REG_CHGR_INT_CLR);
}

static void pet_eud(struct eud_chip *chip)
{
	u32 reg;

	/* read sw_attach_det[0] to find attach/detach event */
	reg = readl_relaxed(chip->eud_reg_base + EUD_REG_SW_ATTACH_DET);
	if (reg & BIT(0)) {
		/* Detach & Attach pet for EUD */
		writel_relaxed(0, chip->eud_reg_base + EUD_REG_SW_ATTACH_DET);
		/* Ensure Register Writes Complete */
		/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
		/* Delay to make sure detach pet is done before attach pet */
		udelay(100);
		writel_relaxed(BIT(0), chip->eud_reg_base +
					EUD_REG_SW_ATTACH_DET);
		/* Ensure Register Writes Complete */
		/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
	} else {
		/* Attach pet for EUD */
		writel_relaxed(BIT(0), chip->eud_reg_base +
					EUD_REG_SW_ATTACH_DET);
		/* Ensure Register Writes Complete */
		/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
	}
}

static unsigned int eud_tx_empty(struct uart_port *port)
{
	u32 reg;

	/* read status register and cross check for Tx interrupt */
	reg = readl_relaxed(port->membase + EUD_REG_INT_STATUS_1);
	if (reg & EUD_INT_TX)
		return TIOCSER_TEMT;
	else
		return 0;
}

static void eud_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	/* Nothing to set */
}

static unsigned int eud_get_mctrl(struct uart_port *port)
{
	return 0;
}

static void eud_set_termios(struct uart_port *port, struct ktermios *new,
				const struct ktermios *old)
{
	/* Nothing to do here, but to satisfy the serial core */
}

static void eud_serial_irqs(struct uart_port *port, u32 set, u32 clear)
{
	struct eud_chip *chip = container_of(port, struct eud_chip, port);
	u32 mask = readl_relaxed(port->membase + EUD_REG_INT1_EN_MASK);

	if (READ_ONCE(chip->removing)) {
		set = 0;
		clear = EUD_INT_ALL;
	}
	mask = (mask & ~clear) | set;
	writel_relaxed(mask, port->membase + EUD_REG_INT1_EN_MASK);
	/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
}

static void eud_stop_tx(struct uart_port *port)
{
	eud_serial_irqs(port, 0, EUD_INT_TX);
}

static void eud_start_tx(struct uart_port *port)
{
	eud_serial_irqs(port, EUD_INT_TX, 0);
}

static void eud_stop_rx(struct uart_port *port)
{
	eud_serial_irqs(port, 0, EUD_INT_RX);
}

static int eud_startup(struct uart_port *port)
{
	struct eud_chip *chip = container_of(port, struct eud_chip, port);
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	if (READ_ONCE(chip->removing)) {
		uart_port_unlock_irqrestore(port, flags);
		return -ENODEV;
	}
	chip->uart_active = true;
	eud_serial_irqs(port, EUD_INT_RX, 0);
	uart_port_unlock_irqrestore(port, flags);
	return 0;
}

static void eud_shutdown(struct uart_port *port)
{
	struct eud_chip *chip = container_of(port, struct eud_chip, port);
	unsigned long flags;

	uart_port_lock_irqsave(port, &flags);
	chip->uart_active = false;
	eud_serial_irqs(port, 0, EUD_INT_TX | EUD_INT_RX);
	uart_port_unlock_irqrestore(port, flags);
}

static const char *eud_type(struct uart_port *port)
{
	return (port->type == PORT_EUD_UART) ? "EUD UART" : NULL;
}

static int eud_request_port(struct uart_port *port)
{
	/* Nothing to request */
	return 0;
}

static void eud_release_port(struct uart_port *port)
{
	/* Nothing to release */
}

static void eud_config_port(struct uart_port *port, int flags)
{
	port->type = PORT_EUD_UART;
	eud_serial_irqs(port, 0, EUD_INT_TX | EUD_INT_RX);
}

static int eud_verify_port(struct uart_port *port,
				       struct serial_struct *ser)
{
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_EUD_UART)
		return -EINVAL;
	return 0;
}

/* serial functions supported */
static const struct uart_ops eud_uart_ops = {
	.tx_empty	= eud_tx_empty,
	.set_mctrl	= eud_set_mctrl,
	.get_mctrl	= eud_get_mctrl,
	.set_termios	= eud_set_termios,
	.stop_tx	= eud_stop_tx,
	.start_tx	= eud_start_tx,
	.stop_rx	= eud_stop_rx,
	.startup	= eud_startup,
	.shutdown	= eud_shutdown,
	.type		= eud_type,
	.release_port	= eud_release_port,
	.request_port	= eud_request_port,
	.config_port	= eud_config_port,
	.verify_port	= eud_verify_port,
};

static struct uart_driver eud_uart_driver = {
	.owner		= THIS_MODULE,
	.driver_name	= "msm-eud",
	.dev_name	= "ttyEUD",
	.nr		= EUD_NR,
	.cons		= EUD_CONSOLE,
};

static void eud_uart_rx(struct eud_chip *chip)
{
	struct uart_port *port = &chip->port;
	unsigned long flags;
	unsigned int len, i;
	unsigned char ch;
	bool active;

	if (readl_relaxed(chip->eud_reg_base + EUD_REG_COM_RX_ID) != UART_ID)
		return;
	uart_port_lock_irqsave(port, &flags);
	active = chip->uart_active && port->state;
	len = readl_relaxed(chip->eud_reg_base + EUD_REG_COM_RX_LEN);
	if (len > MAX_FIFO_SIZE) {
		port->icount.overrun++;
		len = MAX_FIFO_SIZE;
	}
	for (i = 0; i < len; i++) {
		ch = readl_relaxed(chip->eud_reg_base + EUD_REG_COM_RX_DAT);
		if (!active)
			continue;
		port->icount.rx++;
		if (!uart_handle_sysrq_char(port, ch))
			uart_insert_char(port, 0, 0, ch, TTY_NORMAL);
	}
	if (active)
		tty_flip_buffer_push(&port->state->port);
	uart_port_unlock_irqrestore(port, flags);
}

static void eud_uart_tx(struct eud_chip *chip)
{
	struct uart_port *port = &chip->port;
	unsigned long flags;
	unsigned int count = MAX_FIFO_SIZE;
	unsigned char ch;

	uart_port_lock_irqsave(port, &flags);
	if (!chip->uart_active || !port->state)
		goto out;
	writel_relaxed(UART_ID, chip->eud_reg_base + EUD_REG_COM_TX_ID);
	if (readl_relaxed(chip->eud_reg_base + EUD_REG_COM_TX_ID) != UART_ID)
		goto out;
	if (port->x_char) {
		writel_relaxed(port->x_char, port->membase + EUD_REG_COM_TX_DAT);
		port->x_char = 0;
		port->icount.tx++;
		count--;
	}
	if (!uart_tx_stopped(port))
		while (count-- && uart_fifo_get(port, &ch))
			writel_relaxed(ch, port->membase + EUD_REG_COM_TX_DAT);
	if (kfifo_len(&port->state->port.xmit_fifo) < WAKEUP_CHARS)
		uart_write_wakeup(port);
	if (uart_tx_stopped(port) || kfifo_is_empty(&port->state->port.xmit_fifo))
		eud_stop_tx(port);
out:
	uart_port_unlock_irqrestore(port, flags);
}

static irqreturn_t handle_eud_irq(int irq, void *data)
{
	struct eud_chip *chip = data;
	u8 reg, handled;
	u32 int_mask_en1 = readl_relaxed(chip->eud_reg_base +
					EUD_REG_INT1_EN_MASK);

	/* read status register and find out which interrupt triggered */
	reg = readb_relaxed(chip->eud_reg_base + EUD_REG_INT_STATUS_1);
	reg &= int_mask_en1 & EUD_INT_ALL;
	handled = reg;
	if (!reg || READ_ONCE(chip->removing))
		return IRQ_NONE;
	dev_dbg(chip->dev, "EUD Interrupt is received status:%x\n", reg);

	if (reg & EUD_INT_RX) {
		dev_dbg(chip->dev, "EUD RX Interrupt is received\n");
		eud_uart_rx(chip);
		reg &= ~EUD_INT_RX;
	}

	if (reg & EUD_INT_TX) {
		if (EUD_INT_TX & int_mask_en1) {
			dev_dbg(chip->dev, "EUD TX Interrupt is received\n");
			eud_uart_tx(chip);
		}
		reg &= ~EUD_INT_TX;
	}

	if (reg & EUD_INT_VBUS) {
		dev_dbg(chip->dev, "EUD VBUS Interrupt is received\n");
		usb_attach_detach(chip);
		reg &= ~EUD_INT_VBUS;
	}

	if (reg & EUD_INT_CHGR) {
		dev_dbg(chip->dev, "EUD CHGR Interrupt is received\n");
		chgr_enable_disable(chip);
		reg &= ~EUD_INT_CHGR;
	}

	if (reg & EUD_INT_SAFE_MODE) {
		dev_dbg(chip->dev, "EUD SAFE MODE Interrupt is received\n");
		pet_eud(chip);
		reg &= ~EUD_INT_SAFE_MODE;
	}

	if (reg) {
		dev_dbg(chip->dev, "Unhandled EUD interrupt status:%x\n", reg);
		return IRQ_NONE;
	}

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static int msm_eud_suspend(struct device *dev)
{
	struct eud_chip *chip = dev_get_drvdata(dev);

	mutex_lock(&eud_lock);
	chip->suspended = true;
	mutex_unlock(&eud_lock);
	return 0;
}

static int msm_eud_resume(struct device *dev)
{
	struct eud_chip *chip = dev_get_drvdata(dev);

	mutex_lock(&eud_lock);
	chip->suspended = false;
	mutex_unlock(&eud_lock);
	return 0;
}

static int msm_eud_suspend_noirq(struct device *dev)
{
	struct eud_chip *chip = dev_get_drvdata(dev);

	if (chip->ahb_enabled) {
		clk_disable_unprepare(chip->eud_ahb2phy_clk);
		chip->ahb_enabled = false;
	}
	return 0;
}

static int msm_eud_resume_noirq(struct device *dev)
{
	struct eud_chip *chip = dev_get_drvdata(dev);
	int ret;

	if (!chip->need_phy_clk_vote || chip->ahb_enabled)
		return 0;
	ret = clk_prepare_enable(chip->eud_ahb2phy_clk);
	if (!ret)
		chip->ahb_enabled = true;
	return ret;
}

static void msm_eud_release_claim(void *data)
{
	mutex_lock(&eud_lock);
	eud_claimed = false;
	mutex_unlock(&eud_lock);
}

static void msm_eud_cleanup(void *data)
{
	struct platform_device *pdev = data;
	struct eud_chip *chip = platform_get_drvdata(pdev);
	int ret;

	mutex_lock(&eud_lock);
	if (chip->stopped) {
		mutex_unlock(&eud_lock);
		return;
	}
	chip->stopped = true;
	WRITE_ONCE(chip->removing, true);
	if (eud_private == pdev)
		eud_private = NULL;
	mutex_unlock(&eud_lock);
	writel_relaxed(0, chip->eud_reg_base + EUD_REG_INT1_EN_MASK);
	/* Disable the source before synchronizing and releasing its handler. */
	wmb();
	if (chip->irq_registered) {
		if (chip->irq_wake_enabled)
			disable_irq_wake(chip->eud_irq);
		devm_free_irq(chip->dev, chip->eud_irq, chip);
		chip->irq_registered = false;
	}
	cancel_work_sync(&chip->eud_work);
	if (chip->uart_registered) {
		uart_remove_one_port(&eud_uart_driver, &chip->port);
		chip->uart_registered = false;
	}
	writel_relaxed(0, chip->eud_reg_base + EUD_REG_INT1_EN_MASK);
	/* Serial teardown must leave every EUD interrupt masked. */
	wmb();
	ret = disable_eud(pdev);
	if (ret)
		dev_err(chip->dev, "EUD disable failed during cleanup: %d\n", ret);
	msm_eud_clkref_en(chip, false);
	if (chip->ahb_enabled) {
		clk_disable_unprepare(chip->eud_ahb2phy_clk);
		chip->ahb_enabled = false;
	}
	if (chip->usb_psy) {
		power_supply_put(chip->usb_psy);
		chip->usb_psy = NULL;
	}
	device_init_wakeup(chip->dev, false);
}

static int msm_eud_probe(struct platform_device *pdev)
{
	struct eud_chip *chip;
	struct uart_port *port;
	struct resource *res;
	int ret, hw_enabled;

	mutex_lock(&eud_lock);
	if (eud_claimed) {
		mutex_unlock(&eud_lock);
		return -EBUSY;
	}
	eud_claimed = true;
	mutex_unlock(&eud_lock);
	ret = devm_add_action_or_reset(&pdev->dev, msm_eud_release_claim, NULL);
	if (ret)
		return ret;
	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = &pdev->dev;
	if ((of_property_read_bool(chip->dev->of_node, "qcom,secure-eud-en") ||
	     of_property_read_bool(chip->dev->of_node, "qcom,eud-tcsr-check-enable")) &&
	    !qcom_scm_is_available())
		return -EPROBE_DEFER;
	platform_set_drvdata(pdev, chip);
	INIT_WORK(&chip->eud_work, eud_event_notifier);
	port = &chip->port;
	spin_lock_init(&port->lock);
	port->line = 0;
	port->type = PORT_EUD_UART;
	port->dev = chip->dev;
	port->fifosize = MAX_FIFO_SIZE;
	port->iotype = SERIAL_IO_MEM;
	port->flags = UPF_BOOT_AUTOCONF;
	port->ops = &eud_uart_ops;
	chip->extcon = devm_extcon_dev_allocate(chip->dev, eud_extcon_cable);
	if (IS_ERR(chip->extcon))
		return PTR_ERR(chip->extcon);
	ret = devm_extcon_dev_register(chip->dev, chip->extcon);
	if (ret)
		return ret;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "eud_base");
	if (!res || resource_size(res) < EUD_CHIKNBIT_EN_DEL_CNTR + sizeof(u32))
		return -EINVAL;
	chip->eud_reg_base = devm_ioremap_resource(chip->dev, res);
	if (IS_ERR(chip->eud_reg_base))
		return PTR_ERR(chip->eud_reg_base);
	port->membase = chip->eud_reg_base;
	chip->eud_irq = platform_get_irq_byname(pdev, "eud_irq");
	if (chip->eud_irq < 0)
		return chip->eud_irq;
	port->irq = chip->eud_irq;
	chip->secure_eud_en = of_property_read_bool(chip->dev->of_node, "qcom,secure-eud-en");
	if (chip->secure_eud_en) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "eud_mode_mgr2");
		if (!res || resource_size(res) < sizeof(u32))
			return -EINVAL;
		chip->eud_mode_mgr2_phys_base = res->start;
	}
	chip->need_phy_clk_vote = of_property_read_bool(chip->dev->of_node,
						      "qcom,eud-clock-vote-req");
	if (chip->need_phy_clk_vote) {
		chip->eud_ahb2phy_clk = devm_clk_get(chip->dev, "eud_ahb2phy_clk");
		if (IS_ERR(chip->eud_ahb2phy_clk))
			return PTR_ERR(chip->eud_ahb2phy_clk);
	}
	chip->eud_clkref_clk = devm_clk_get_optional(chip->dev, "eud_clkref_clk");
	if (IS_ERR(chip->eud_clkref_clk))
		return PTR_ERR(chip->eud_clkref_clk);
	if (chip->need_phy_clk_vote) {
		ret = clk_prepare_enable(chip->eud_ahb2phy_clk);
		if (ret)
			return ret;
		chip->ahb_enabled = true;
	}
	ret = of_property_read_u16(chip->dev->of_node, "qcom,eud-utmi-delay",
					&chip->utmi_switch_delay);
	if (ret || chip->utmi_switch_delay < EUD_UTMI_DELAY_MIN)
		chip->utmi_switch_delay = 0;
	if (of_property_read_bool(chip->dev->of_node, "qcom,eud-tcsr-check-enable")) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "eud_tcsr_check_reg");
		if (!res) {
			ret = -EINVAL;
			goto error;
		}
		ret = qcom_scm_io_writel(res->start, EUD_TCSR_ENABLE_BIT);
		if (ret)
			goto error;
	}
	hw_enabled = msm_eud_hw_is_enabled(pdev);
	if (hw_enabled < 0) {
		ret = hw_enabled;
		goto error;
	}
	/* No interrupt can observe uninitialized work or serial state. */
	writel_relaxed(0, chip->eud_reg_base + EUD_REG_INT1_EN_MASK);
	/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
	ret = uart_add_one_port(&eud_uart_driver, port);
	if (ret)
		goto error;
	chip->uart_registered = true;
	ret = devm_request_irq(chip->dev, chip->eud_irq, handle_eud_irq,
			      IRQF_TRIGGER_HIGH, "eud_irq", chip);
	if (ret)
		goto error;
	chip->irq_registered = true;
	ret = device_init_wakeup(chip->dev, true);
	if (ret)
		goto error;
	ret = enable_irq_wake(chip->eud_irq);
	if (ret)
		goto error;
	chip->irq_wake_enabled = true;
	writel_relaxed(1, chip->eud_reg_base + EUD_CHIKNBIT_EN_DEL_CNTR);
	/* Complete MMIO writes before the next hardware or notification step. */
	wmb();
	if (hw_enabled) {
		chip->maybe_enabled = true;
		ret = msm_eud_clkref_en(chip, true);
		if (ret)
			goto error;
		set_eud_utmi_switch_delay(chip);
		msm_eud_enable_irqs(chip);
		ret = extcon_set_state_sync(chip->extcon, EXTCON_USB, true);
		if (ret)
			goto error;
		ret = extcon_set_state_sync(chip->extcon, EXTCON_CHG_USB_SDP, true);
		if (ret)
			goto error;
		chip->eud_enabled = true;
	}
	mutex_lock(&eud_lock);
	if (!hw_enabled && enable)
		ret = enable_eud(pdev);
	else
		ret = 0;
	if (!ret) {
		if (hw_enabled)
			enable = EUD_ENABLE_CMD;
		eud_private = pdev;
	}
	mutex_unlock(&eud_lock);
	if (ret)
		goto error;
	return devm_add_action_or_reset(chip->dev, msm_eud_cleanup, pdev);
error:
	msm_eud_cleanup(pdev);
	return ret;
}

static void msm_eud_remove(struct platform_device *pdev)
{
	msm_eud_cleanup(pdev);
}

static const struct of_device_id msm_eud_dt_match[] = {
	{.compatible = "qcom,msm-eud"},
	{},
};
MODULE_DEVICE_TABLE(of, msm_eud_dt_match);

static const struct dev_pm_ops msm_eud_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(msm_eud_suspend, msm_eud_resume)
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(msm_eud_suspend_noirq, msm_eud_resume_noirq)
};

static struct platform_driver msm_eud_driver = {
	.probe		= msm_eud_probe,
	.remove		= msm_eud_remove,
	.driver		= {
		.name		= "msm-eud",
		.pm = &msm_eud_dev_pm_ops,
		.of_match_table = msm_eud_dt_match,
	},
};

static int __init msm_eud_init(void)
{
	int ret;

	ret = uart_register_driver(&eud_uart_driver);
	if (ret) {
		pr_err("%s: Failed to register EUD UART driver\n",
			__func__);
		return ret;
	}

	ret = platform_driver_register(&msm_eud_driver);
	if (ret) {
		pr_err("%s: Failed to register EUD driver\n",
			__func__);
		uart_unregister_driver(&eud_uart_driver);
		return ret;
	}

	return 0;
}
module_init(msm_eud_init);

static void __exit msm_eud_exit(void)
{
	platform_driver_unregister(&msm_eud_driver);
	uart_unregister_driver(&eud_uart_driver);
}
module_exit(msm_eud_exit);

MODULE_DESCRIPTION("QTI EUD driver");
MODULE_LICENSE("GPL");
