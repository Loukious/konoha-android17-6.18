// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/phy.h>
#include <linux/usb/dwc3-msm.h>
#include <linux/clk.h>
#include <linux/extcon.h>
#include <linux/reset.h>
#include <linux/mutex.h>

enum core_ldo_levels {
	CORE_LEVEL_NONE = 0,
	CORE_LEVEL_MIN,
	CORE_LEVEL_MAX,
};

#define INIT_MAX_TIME_USEC			1000

/* default CORE votlage and load values */
#define USB_SSPHY_1P2_VOL_MIN		1200000 /* uV */
#define USB_SSPHY_1P2_VOL_MAX		1200000 /* uV */
#define USB_SSPHY_HPM_LOAD		30000	/* uA */

/* defining load value for Refgen */
#define USB3PHY_REFGEN_HPM_LOAD		1200000  /* uA */

/* USB3PHY_PCIE_USB3_PCS_PCS_STATUS bit */
#define PHYSTATUS				BIT(6)

/* PCIE_USB3_PHY_AUTONOMOUS_MODE_CTRL bits */
#define ARCVR_DTCT_EN		BIT(0)
#define ALFPS_DTCT_EN		BIT(1)
#define ARCVR_DTCT_EVENT_SEL	BIT(4)

/*
 * register bits
 * PCIE_USB3_PHY_PCS_MISC_TYPEC_CTRL - for QMP USB PHY
 * USB3_DP_COM_PHY_MODE_CTRL - for QMP USB DP Combo PHY
 */

/* 0 - selects Lane A. 1 - selects Lane B */
#define SW_PORTSELECT		BIT(0)
/* port select mux: 1 - sw control. 0 - HW control*/
#define SW_PORTSELECT_MX	BIT(1)

/* USB3_DP_PHY_USB3_DP_COM_SWI_CTRL bits */

/* LANE related register read/write with USB3 */
#define USB3_SWI_ACT_ACCESS_EN	BIT(0)
/* LANE related register read/write with DP */
#define DP_SWI_ACT_ACCESS_EN	BIT(1)

/* USB3_DP_COM_RESET_OVRD_CTRL bits */

/* DP PHY soft reset */
#define SW_DPPHY_RESET		BIT(0)
/* mux to select DP PHY reset control, 0:HW control, 1: software reset */
#define SW_DPPHY_RESET_MUX	BIT(1)
/* USB3 PHY soft reset */
#define SW_USB3PHY_RESET	BIT(2)
/* mux to select USB3 PHY reset control, 0:HW control, 1: software reset */
#define SW_USB3PHY_RESET_MUX	BIT(3)

/* USB3_DP_COM_PHY_MODE_CTRL bits */
#define USB3_MODE		BIT(0) /* enables USB3 mode */
#define DP_MODE			BIT(1) /* enables DP mode */
#define USB3_DP_COMBO_MODE	(USB3_MODE | DP_MODE) /*enables combo mode */

/* USB3_DP_COM_TYPEC_STATUS */
#define PORTSELECT_RAW		BIT(0)

enum qmp_phy_rev_reg {
	USB3_PHY_PCS_STATUS,
	USB3_PHY_AUTONOMOUS_MODE_CTRL,
	USB3_PHY_LFPS_RXTERM_IRQ_CLEAR,
	USB3_PHY_POWER_DOWN_CONTROL,
	USB3_PHY_SW_RESET,
	USB3_PHY_START,

	/* TypeC port select configuration (optional) */
	USB3_PHY_PCS_MISC_TYPEC_CTRL,

	/* USB DP Combo PHY related */
	USB3_DP_COM_POWER_DOWN_CTRL,
	USB3_DP_COM_SW_RESET,
	USB3_DP_COM_RESET_OVRD_CTRL,
	USB3_DP_COM_PHY_MODE_CTRL,
	USB3_DP_COM_TYPEC_CTRL,
	USB3_PCS_MISC_CLAMP_ENABLE,
	USB3_DP_COM_TYPEC_STATUS,
	USB3_PHY_REG_MAX,
};
#define PHY_REG_SIZE (USB3_PHY_REG_MAX * sizeof(u32))

enum qmp_phy_type {
	USB3,
	USB3_OR_DP,
	USB3_AND_DP,
};

/* reg values to write */
struct qmp_reg_val {
	u32 offset;
	u32 val;
};

struct msm_ssphy_qmp {
	struct usb_phy		phy;
	void __iomem		*base;
	void __iomem		*vls_clamp_reg;
	void __iomem		*pcs_clamp_enable_reg;
	void __iomem		*tcsr_usb3_dp_phymode;

	struct regulator	*vdd;
	int			vdd_levels[3]; /* none, low, high */
	int			refgen_levels[3]; /* 0, REFGEN_VOL_MIN, REFGEN_VOL_MAX */
	int			vdd_max_uA;
	struct regulator	*core_ldo;
	int			core_voltage_levels[3];
	int			core_max_uA;
	struct regulator	*usb3_dp_phy_gdsc;
	struct regulator	*refgen;
	struct clk		*ref_clk_src;
	struct clk		*ref_clk;
	struct clk		*aux_clk;
	struct clk		*com_aux_clk;
	struct clk		*cfg_ahb_clk;
	struct clk		*pipe_clk;
	struct clk		*pipe_clk_mux;
	struct clk		*pipe_clk_ext_src;
	struct reset_control	*phy_reset;
	struct reset_control	*phy_phy_reset;
	struct reset_control	*global_phy_reset;
	struct mutex		lock;
	resource_size_t		base_size;
	unsigned int		supply_votes[3];
	unsigned long		clock_votes;
	bool			gdsc_enabled;
	bool			removing;
	bool			transition_failed;
	bool			power_enabled;
	bool			clk_enabled;
	bool			cable_connected;
	bool			in_suspend;
	u32			*phy_reg; /* revision based offset */
	int			reg_offset_cnt;
	u32			*qmp_phy_init_seq;
	int			init_seq_len;
	enum qmp_phy_type	phy_type;
};

static const struct of_device_id msm_usb_id_table[] = {
	{
		.compatible = "qcom,usb-ssphy-qmp",
	},
	{
		.compatible = "qcom,usb-ssphy-qmp-v1",
	},
	{
		.compatible = "qcom,usb-ssphy-qmp-v2",
	},
	{
		.compatible = "qcom,usb-ssphy-qmp-dp-combo",
	},
	{
		.compatible = "qcom,usb-ssphy-qmp-usb3-or-dp",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, msm_usb_id_table);

static void usb_qmp_powerup_phy(struct msm_ssphy_qmp *phy);
static int msm_ssphy_qmp_enable_clks(struct msm_ssphy_qmp *phy, bool on);
static int msm_ssphy_qmp_reset(struct usb_phy *uphy);
static int msm_ssphy_qmp_dp_combo_reset(struct usb_phy *uphy);

static inline char *get_cable_status_str(struct msm_ssphy_qmp *phy)
{
	return phy->cable_connected ? "connected" : "disconnected";
}

static void msm_ssusb_qmp_clr_lfps_rxterm_int(struct msm_ssphy_qmp *phy)
{
	writel_relaxed(1, phy->base +
			phy->phy_reg[USB3_PHY_LFPS_RXTERM_IRQ_CLEAR]);
	/* flush the previous write before next write */
	wmb();
	writel_relaxed(0, phy->base +
			phy->phy_reg[USB3_PHY_LFPS_RXTERM_IRQ_CLEAR]);
}

static void msm_ssusb_qmp_clamp_enable(struct msm_ssphy_qmp *phy, bool val)
{
	switch (phy->phy_type) {
	case USB3_AND_DP:
		writel_relaxed(!val, phy->base +
			phy->phy_reg[USB3_PCS_MISC_CLAMP_ENABLE]);
		break;
	case USB3_OR_DP:
	case USB3:
		if (phy->vls_clamp_reg)
			writel_relaxed(!!val, phy->vls_clamp_reg);
		if (phy->pcs_clamp_enable_reg)
			writel_relaxed(!val, phy->pcs_clamp_enable_reg);
		break;
	default:
		break;
	}
}

static void msm_ssusb_qmp_enable_autonomous(struct msm_ssphy_qmp *phy,
		int enable)
{
	u32 val;
	unsigned int autonomous_mode_offset =
			phy->phy_reg[USB3_PHY_AUTONOMOUS_MODE_CTRL];

	dev_dbg(phy->phy.dev, "enabling QMP autonomous mode with cable %s\n",
			get_cable_status_str(phy));

	if (enable) {
		msm_ssusb_qmp_clr_lfps_rxterm_int(phy);
		val = readl_relaxed(phy->base + autonomous_mode_offset);
		val |= ARCVR_DTCT_EN;
		if (phy->phy.flags & DEVICE_IN_SS_MODE) {
			val |= ALFPS_DTCT_EN;
			val &= ~ARCVR_DTCT_EVENT_SEL;
		} else {
			val &= ~ALFPS_DTCT_EN;
			val |= ARCVR_DTCT_EVENT_SEL;
		}
		writel_relaxed(val, phy->base + autonomous_mode_offset);
		msm_ssusb_qmp_clamp_enable(phy, true);
	} else {
		msm_ssusb_qmp_clamp_enable(phy, false);
		writel_relaxed(0, phy->base + autonomous_mode_offset);
		msm_ssusb_qmp_clr_lfps_rxterm_int(phy);
	}
}

static int msm_ssusb_qmp_gdsc(struct msm_ssphy_qmp *phy, bool on)
{
	int ret;

	if (!phy->usb3_dp_phy_gdsc || phy->gdsc_enabled == on)
		return 0;
	ret = on ? regulator_enable(phy->usb3_dp_phy_gdsc) :
		   regulator_disable(phy->usb3_dp_phy_gdsc);
	if (!ret)
		phy->gdsc_enabled = on;
	return ret;
}

/* Keep each vote until its provider confirms release. */
static int qmp_supply_disable(struct regulator *reg, const int levels[3], unsigned int *votes)
{
	int ret;

	if (*votes & BIT(2)) {
		ret = regulator_disable(reg);
		if (ret)
			return ret;
		*votes &= ~BIT(2);
	}
	if (*votes & BIT(1)) {
		ret = regulator_set_voltage(reg, levels[0], levels[2]);
		if (ret)
			return ret;
		*votes &= ~BIT(1);
	}
	if (*votes & BIT(0)) {
		ret = regulator_set_load(reg, 0);
		if (ret < 0)
			return ret;
		*votes &= ~BIT(0);
	}
	return 0;
}

static int qmp_supply_enable(struct regulator *reg, const int levels[3], int load,
			     unsigned int *votes)
{
	int ret;

	ret = regulator_set_load(reg, load);
	if (ret < 0)
		return ret;
	*votes |= BIT(0);
	ret = regulator_set_voltage(reg, levels[1], levels[2]);
	if (ret)
		return ret;
	*votes |= BIT(1);
	ret = regulator_enable(reg);
	if (!ret)
		*votes |= BIT(2);
	return ret;
}

static int qmp_power_off(struct msm_ssphy_qmp *phy)
{
	int ret;

	phy->power_enabled = false;
	ret = qmp_supply_disable(phy->refgen, phy->refgen_levels, &phy->supply_votes[2]);
	if (ret)
		return ret;
	ret = qmp_supply_disable(phy->core_ldo, phy->core_voltage_levels, &phy->supply_votes[1]);
	if (ret)
		return ret;
	ret = qmp_supply_disable(phy->vdd, phy->vdd_levels, &phy->supply_votes[0]);
	if (ret)
		return ret;
	return msm_ssusb_qmp_gdsc(phy, false);
}

static int msm_ssusb_qmp_ldo_enable(struct msm_ssphy_qmp *phy, int on)
{
	int ret, cleanup;

	if (!on)
		return qmp_power_off(phy);
	if (phy->power_enabled)
		return 0;
	/* Finish any failed unwind before acquiring fresh votes. */
	ret = qmp_power_off(phy);
	if (ret)
		return ret;
	ret = msm_ssusb_qmp_gdsc(phy, true);
	if (ret)
		return ret;
	ret = qmp_supply_enable(phy->vdd, phy->vdd_levels, phy->vdd_max_uA,
				&phy->supply_votes[0]);
	if (ret)
		goto unwind;
	ret = qmp_supply_enable(phy->core_ldo, phy->core_voltage_levels, phy->core_max_uA,
				&phy->supply_votes[1]);
	if (ret)
		goto unwind;
	if (phy->refgen) {
		ret = qmp_supply_enable(phy->refgen, phy->refgen_levels, USB3PHY_REFGEN_HPM_LOAD,
					&phy->supply_votes[2]);
		if (ret)
			goto unwind;
	}
	phy->power_enabled = true;
	return 0;
unwind:
	cleanup = qmp_power_off(phy);
	if (cleanup)
		dev_err(phy->phy.dev, "Supply unwind failed: %d (original %d)\n", cleanup, ret);
	return ret;
}

static bool qmp_offset_valid(resource_size_t size, u32 offset)
{
	return size >= sizeof(u32) && !(offset & 3) && offset <= size - sizeof(u32);
}

static int qmp_validate_tables(struct msm_ssphy_qmp *phy)
{
	int i, required = USB3_PHY_START + 1;

	if (phy->phy_type == USB3_AND_DP)
		required = USB3_PCS_MISC_CLAMP_ENABLE + 1;
	else if (phy->phy_type == USB3_OR_DP)
		required = USB3_PHY_PCS_MISC_TYPEC_CTRL + 1;
	if (phy->reg_offset_cnt < required || phy->reg_offset_cnt > USB3_PHY_REG_MAX ||
	    phy->init_seq_len <= 0 || (phy->init_seq_len & 1))
		return -EINVAL;
	for (i = 0; i < phy->reg_offset_cnt; i++) {
		if (i == USB3_PHY_PCS_MISC_TYPEC_CTRL && phy->phy_type != USB3_OR_DP)
			continue;
		if (!qmp_offset_valid(phy->base_size, phy->phy_reg[i]))
			return -EINVAL;
	}
	for (i = 0; i < phy->init_seq_len; i += 2)
		if (!qmp_offset_valid(phy->base_size, phy->qmp_phy_init_seq[i]))
			return -EINVAL;
	return 0;
}

static int configure_phy_regs(struct usb_phy *uphy,
				const struct qmp_reg_val *reg)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);
	int i;

	if (!reg) {
		dev_err(uphy->dev, "NULL PHY configuration\n");
		return -EINVAL;
	}

	for (i = 0; i < phy->init_seq_len/2; i++) {
		writel_relaxed(reg->val, phy->base + reg->offset);
		reg++;
	}
	return 0;
}

static void msm_ssphy_qmp_setmode(struct msm_ssphy_qmp *phy, u32 mode)
{
	mode = mode & USB3_DP_COMBO_MODE;

	writel_relaxed(mode,
			phy->base + phy->phy_reg[USB3_DP_COM_PHY_MODE_CTRL]);

	/* flush the write by reading it */
	readl_relaxed(phy->base + phy->phy_reg[USB3_DP_COM_PHY_MODE_CTRL]);
}

static void usb_qmp_update_portselect_phymode(struct msm_ssphy_qmp *phy)
{
	int val;

	/* perform lane selection */
	val = -EINVAL;
	if (phy->phy.flags & PHY_LANE_A)
		val = SW_PORTSELECT_MX;
	else if (phy->phy.flags & PHY_LANE_B)
		val = SW_PORTSELECT | SW_PORTSELECT_MX;

	/* PHY must be powered up before updating portselect and phymode. */
	usb_qmp_powerup_phy(phy);

	switch (phy->phy_type) {
	case USB3_AND_DP:
		writel_relaxed(0x01,
			phy->base + phy->phy_reg[USB3_DP_COM_SW_RESET]);
		writel_relaxed(0x00,
			phy->base + phy->phy_reg[USB3_DP_COM_SW_RESET]);

		if (phy->phy_reg[USB3_DP_COM_TYPEC_STATUS]) {
			u32 status = readl_relaxed(phy->base +
					phy->phy_reg[USB3_DP_COM_TYPEC_STATUS]);
			dev_dbg(phy->phy.dev, "hw port select %s\n",
					status & PORTSELECT_RAW ? "CC2" : "CC1");
		}

		if (!(phy->phy.flags & PHY_USB_DP_CONCURRENT_MODE))
			/* override hardware control for reset of qmp phy */
			writel_relaxed(SW_DPPHY_RESET_MUX | SW_DPPHY_RESET |
				SW_USB3PHY_RESET_MUX | SW_USB3PHY_RESET,
				phy->base + phy->phy_reg[USB3_DP_COM_RESET_OVRD_CTRL]);

		/* update port select */
		if (val > 0) {
			dev_err(phy->phy.dev,
				"USB DP QMP PHY: Update TYPEC CTRL(%d)\n", val);
			writel_relaxed(val, phy->base +
				phy->phy_reg[USB3_DP_COM_TYPEC_CTRL]);
		}

#ifdef CONFIG_PINCTRL
		/*
		 * if there is no default pinctrl state for orientation,
		 * it need external module provide SW orientation info,
		 * report an error if there is no such info.
		 */
		if (val < 0 && !phy->phy.dev->pins)
			dev_err(phy->phy.dev,
				"USB DP QMP PHY: NO SW PORTSELECT\n");
#endif

		if (!(phy->phy.flags & PHY_USB_DP_CONCURRENT_MODE)) {
			msm_ssphy_qmp_setmode(phy, USB3_DP_COMBO_MODE);

			/* bring both USB and DP PHYs PCS block out of reset */
			writel_relaxed(0x00, phy->base +
				phy->phy_reg[USB3_DP_COM_RESET_OVRD_CTRL]);
		}
		break;
	case  USB3_OR_DP:
		if (val > 0) {
			dev_err(phy->phy.dev,
				"USB QMP PHY: Update TYPEC CTRL(%d)\n", val);
			writel_relaxed(val, phy->base +
				phy->phy_reg[USB3_PHY_PCS_MISC_TYPEC_CTRL]);
		}
		break;
	default:
		dev_dbg(phy->phy.dev, "no portselect for phy type %d\n",
					phy->phy_type);
		break;
	}

	/* Make sure above selection and reset sequence is gone through */
	mb();
}

static void usb_qmp_powerup_phy(struct msm_ssphy_qmp *phy)
{
	switch (phy->phy_type) {
	case USB3_AND_DP:
		/* power up USB3 and DP common logic block */
		writel_relaxed(0x01,
			phy->base + phy->phy_reg[USB3_DP_COM_POWER_DOWN_CTRL]);

		/*
		 * Don't write 0x0 to DP_COM_SW_RESET here as portselect and
		 * phymode operation needs DP_COM_SW_RESET as 0x1.
		 * msm_ssphy_qmp_init() writes 0x0 to DP_COM_SW_RESET before
		 * initializing PHY.
		 */
		fallthrough;
	case USB3_OR_DP:
	case USB3:
		/* power up USB3 PHY */
		writel_relaxed(0x01,
			phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
		break;
	default:
		dev_err(phy->phy.dev, "phy_powerup: Unknown USB QMP PHY type\n");
		break;
	}

	/* Make sure that above write completed to power up PHY */
	mb();
}

/* SSPHY Initialization */
static int msm_ssphy_qmp_init_locked(struct usb_phy *uphy)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);
	int ret;
	unsigned int init_timeout_usec = INIT_MAX_TIME_USEC;
	const struct qmp_reg_val *reg = NULL;

	dev_dbg(uphy->dev, "Initializing QMP phy\n");

	if (uphy->flags & PHY_DP_MODE) {
		dev_info(uphy->dev, "QMP PHY currently in DP mode\n");
		return -EBUSY;
	}

	ret = msm_ssusb_qmp_ldo_enable(phy, 1);
	if (ret) {
		dev_err(phy->phy.dev,
		"msm_ssusb_qmp_ldo_enable(1) failed, ret=%d\n",
		ret);
		return ret;
	}

	ret = msm_ssphy_qmp_enable_clks(phy, true);
	if (ret)
		goto disable_power;

	if (phy->phy_type == USB3_AND_DP)
		ret = msm_ssphy_qmp_dp_combo_reset(&phy->phy);
	else
		ret = msm_ssphy_qmp_reset(&phy->phy);
	if (ret)
		goto disable_clocks;

	/* select appropriate port select and PHY mode if applicable */
	usb_qmp_update_portselect_phymode(phy);

	/* power up PHY */
	usb_qmp_powerup_phy(phy);

	reg = (struct qmp_reg_val *)phy->qmp_phy_init_seq;

	/* Main configuration */
	ret = configure_phy_regs(uphy, reg);
	if (ret) {
		dev_err(uphy->dev, "Failed the main PHY configuration\n");
		goto fail;
	}

	/* perform software reset of PCS/Serdes */
	writel_relaxed(0x00, phy->base + phy->phy_reg[USB3_PHY_SW_RESET]);
	/* start PCS/Serdes to operation mode */
	writel_relaxed(0x03, phy->base + phy->phy_reg[USB3_PHY_START]);

	/* Make sure above write completed to bring PHY out of reset */
	mb();

	/* Wait for PHY initialization to be done */
	do {
		if (readl_relaxed(phy->base +
			phy->phy_reg[USB3_PHY_PCS_STATUS]) & PHYSTATUS)
			usleep_range(1, 2);
		else
			break;
	} while (--init_timeout_usec);

	if (!init_timeout_usec) {
		dev_err(uphy->dev, "QMP PHY initialization timeout\n");
		dev_err(uphy->dev, "USB3_PHY_PCS_STATUS:%x\n",
				readl_relaxed(phy->base +
					phy->phy_reg[USB3_PHY_PCS_STATUS]));
		ret = -EBUSY;
		goto fail;
	}

	phy->in_suspend = false;
	return 0;
fail:
	phy->in_suspend = true;
	writel_relaxed(0x00,
		phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
disable_clocks:
	if (msm_ssphy_qmp_enable_clks(phy, false))
		dev_err(uphy->dev, "Clock unwind failed\n");
disable_power:
	if (!phy->clock_votes && msm_ssusb_qmp_ldo_enable(phy, 0))
		dev_err(uphy->dev, "Power unwind failed\n");
	phy->in_suspend = true;
	return ret;
}

static int msm_ssphy_qmp_dp_combo_reset(struct usb_phy *uphy)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);
	int ret = 0, cleanup;

	if (phy->phy.flags & PHY_USB_DP_CONCURRENT_MODE) {
		dev_dbg(uphy->dev, "Resetting USB part of QMP phy\n");

		/* Assert USB3 PHY CSR reset */
		ret = reset_control_assert(phy->phy_reset);
		if (ret) {
			dev_err(uphy->dev, "phy_reset assert failed\n");
			goto exit;
		}

		/* Deassert USB3 PHY CSR reset */
		ret = reset_control_deassert(phy->phy_reset);
		if (ret) {
			dev_err(uphy->dev, "phy_reset deassert failed\n");
			goto exit;
		}
		return 0;
	}

	dev_dbg(uphy->dev, "Global reset of QMP DP combo phy\n");
	/* Assert global PHY reset */
	ret = reset_control_assert(phy->global_phy_reset);
	if (ret) {
		dev_err(uphy->dev, "global_phy_reset assert failed\n");
		goto exit;
	}

	/* Assert QMP USB PHY reset */
	ret = reset_control_assert(phy->phy_reset);
	if (ret) {
		dev_err(uphy->dev, "phy_reset assert failed\n");
		goto deassert_global;
	}

	/* De-Assert QMP USB PHY reset */
	ret = reset_control_deassert(phy->phy_reset);
	if (ret)
		dev_err(uphy->dev, "phy_reset deassert failed\n");

deassert_global:
	/* Preserve the first error while releasing the outer reset. */
	cleanup = reset_control_deassert(phy->global_phy_reset);
	if (cleanup) {
		dev_err(uphy->dev, "global_phy_reset deassert failed\n");
		if (!ret)
			ret = cleanup;
	}

exit:
	return ret;
}

static int msm_ssphy_qmp_reset(struct usb_phy *uphy)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);
	int ret, cleanup;

	dev_dbg(uphy->dev, "Resetting QMP phy\n");

	/* Assert USB3 PHY reset */
	ret = reset_control_assert(phy->phy_phy_reset);
	if (ret) {
		dev_err(uphy->dev, "phy_phy_reset assert failed\n");
		goto exit;
	}

	/* Assert USB3 PHY CSR reset */
	ret = reset_control_assert(phy->phy_reset);
	if (ret) {
		dev_err(uphy->dev, "phy_reset assert failed\n");
		goto deassert_phy_phy_reset;
	}

	/* select usb3 phy mode */
	if (phy->tcsr_usb3_dp_phymode)
		writel_relaxed(0x0, phy->tcsr_usb3_dp_phymode);

	/* Deassert USB3 PHY CSR reset */
	ret = reset_control_deassert(phy->phy_reset);
	if (ret) {
		dev_err(uphy->dev, "phy_reset deassert failed\n");
		goto deassert_phy_phy_reset;
	}

	/* Deassert USB3 PHY reset */
	ret = reset_control_deassert(phy->phy_phy_reset);
	if (ret) {
		dev_err(uphy->dev, "phy_phy_reset deassert failed\n");
		goto exit;
	}

	return 0;

deassert_phy_phy_reset:
	cleanup = reset_control_deassert(phy->phy_phy_reset);
	if (cleanup)
		dev_err(uphy->dev, "phy_phy_reset deassert failed\n");
exit:

	return ret;
}

static int msm_ssphy_power_enable(struct msm_ssphy_qmp *phy, bool on)
{
	bool host = phy->phy.flags & PHY_HOST_MODE;
	int ret = 0;

	/*
	 * Turn off the phy's LDOs when cable is disconnected for device mode
	 * with external vbus_id indication.
	 */
	if (!host && !phy->cable_connected) {
		if (on) {
			ret = msm_ssusb_qmp_ldo_enable(phy, 1);
			if (ret)
				dev_err(phy->phy.dev,
				"msm_ssusb_qmp_ldo_enable(1) failed, ret=%d\n",
				ret);
		} else {
			ret = msm_ssusb_qmp_ldo_enable(phy, 0);
			if (ret)
				dev_err(phy->phy.dev,
					"msm_ssusb_qmp_ldo_enable(0) failed, ret=%d\n",
					ret);
		}
	}

	return ret;
}

/**
 * Performs QMP PHY suspend/resume functionality.
 *
 * @uphy - usb phy pointer.
 * @suspend - to enable suspend or not. 1 - suspend, 0 - resume
 *
 */
static int msm_ssphy_qmp_set_suspend_locked(struct usb_phy *uphy, int suspend)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);
	int ret;

	dev_dbg(uphy->dev, "QMP PHY set_suspend for %s called with cable %s\n",
			(suspend ? "suspend" : "resume"),
			get_cable_status_str(phy));

	if (phy->in_suspend == suspend && !phy->transition_failed) {
		dev_dbg(uphy->dev, "%s: USB PHY is already %s.\n",
			__func__, (suspend ? "suspended" : "resumed"));
		return 0;
	}

	if (suspend) {
		if (!phy->clk_enabled)
			goto disable_resources;
		if (phy->cable_connected) {
			msm_ssusb_qmp_enable_autonomous(phy, 1);
		} else {
			/* Reset phy mode to USB only if DP not connected */
			if (phy->phy_type == USB3_AND_DP &&
				!((uphy->flags & PHY_DP_MODE) ||
				(uphy->flags & PHY_USB_DP_CONCURRENT_MODE)))
				msm_ssphy_qmp_setmode(phy, USB3_MODE);
			writel_relaxed(0x00,
			phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
		}

		/* Make sure above write completed with PHY */
		wmb();

disable_resources:
		ret = msm_ssphy_qmp_enable_clks(phy, false);
		if (ret)
			return ret;
		ret = msm_ssphy_power_enable(phy, 0);
		if (ret)
			return ret;
		phy->in_suspend = true;
		dev_dbg(uphy->dev, "QMP PHY is suspend\n");
	} else {
		if (uphy->flags & PHY_DP_MODE) {
			dev_info(uphy->dev, "QMP PHY currently in DP mode\n");
			return -EBUSY;
		}

		ret = msm_ssphy_power_enable(phy, 1);
		if (ret)
			return ret;
		ret = msm_ssphy_qmp_enable_clks(phy, true);
		if (ret)
			return ret;
		if (!phy->cable_connected) {
			writel_relaxed(0x01,
			phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
		} else  {
			msm_ssusb_qmp_enable_autonomous(phy, 0);
		}

		/* Make sure that above write completed with PHY */
		wmb();

		phy->in_suspend = false;
		dev_dbg(uphy->dev, "QMP PHY is resumed\n");
	}

	return 0;
}

static int msm_ssphy_qmp_init(struct usb_phy *uphy)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp, phy);
	int ret;

	mutex_lock(&phy->lock);
	ret = phy->removing ? -ENODEV : msm_ssphy_qmp_init_locked(uphy);
	phy->transition_failed = ret != 0;
	mutex_unlock(&phy->lock);
	return ret;
}

static int msm_ssphy_qmp_set_suspend(struct usb_phy *uphy, int suspend)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp, phy);
	int ret;

	mutex_lock(&phy->lock);
	ret = phy->removing ? -ENODEV : msm_ssphy_qmp_set_suspend_locked(uphy, suspend);
	phy->transition_failed = ret != 0;
	mutex_unlock(&phy->lock);
	return ret;
}

static int msm_ssphy_qmp_notify_connect(struct usb_phy *uphy,
				       enum usb_device_speed speed)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);

	dev_dbg(uphy->dev, "QMP phy connect notification\n");
	WRITE_ONCE(phy->cable_connected, true);
	atomic_notifier_call_chain(&uphy->notifier, 1, uphy);

	return 0;
}

static int msm_ssphy_qmp_notify_disconnect(struct usb_phy *uphy,
				       enum usb_device_speed speed)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);

	atomic_notifier_call_chain(&uphy->notifier, 0, uphy);
	if ((phy->phy.flags & PHY_HOST_MODE) && READ_ONCE(phy->clk_enabled)) {
		writel_relaxed(0x00,
			phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
		readl_relaxed(phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
	}

	dev_dbg(uphy->dev, "QMP phy disconnect notification\n");
	dev_dbg(uphy->dev, " cable_connected=%d\n", phy->cable_connected);
	WRITE_ONCE(phy->cable_connected, false);

	return 0;
}

static int msm_ssphy_qmp_get_clks(struct msm_ssphy_qmp *phy, struct device *dev)
{
	struct clk **optional[] = { &phy->cfg_ahb_clk, &phy->pipe_clk_mux,
		&phy->pipe_clk_ext_src, &phy->ref_clk_src, &phy->ref_clk, &phy->com_aux_clk };
	const char * const names[] = { "cfg_ahb_clk", "pipe_clk_mux", "pipe_clk_ext_src",
		"ref_clk_src", "ref_clk", "com_aux_clk" };
	long rate;
	int ret, i;

	phy->aux_clk = devm_clk_get(dev, "aux_clk");
	if (IS_ERR(phy->aux_clk))
		return PTR_ERR(phy->aux_clk);
	rate = clk_round_rate(phy->aux_clk, ULONG_MAX);
	if (rate < 0)
		return rate;
	ret = clk_set_rate(phy->aux_clk, rate);
	if (ret)
		return ret;
	phy->pipe_clk = devm_clk_get(dev, "pipe_clk");
	if (IS_ERR(phy->pipe_clk))
		return PTR_ERR(phy->pipe_clk);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		*optional[i] = devm_clk_get_optional(dev, names[i]);
		if (IS_ERR(*optional[i]))
			return PTR_ERR(*optional[i]);
	}
	if (phy->pipe_clk_mux && (!phy->pipe_clk_ext_src || !phy->ref_clk_src))
		return -EINVAL;
	return 0;
}

static int msm_ssphy_qmp_enable_clks(struct msm_ssphy_qmp *phy, bool on)
{
	struct clk *clocks[] = { phy->ref_clk_src, phy->ref_clk, phy->com_aux_clk,
		phy->aux_clk, phy->cfg_ahb_clk, phy->pipe_clk };
	int ret = 0, cleanup, i;

	if (on && phy->clk_enabled)
		return 0;
	if (on) {
		for (i = 0; i < ARRAY_SIZE(clocks) - 1; i++) {
			if (!clocks[i] || (phy->clock_votes & BIT(i)))
				continue;
			ret = clk_prepare_enable(clocks[i]);
			if (ret)
				goto unwind;
			phy->clock_votes |= BIT(i);
		}
		ret = clk_set_parent(phy->pipe_clk_mux, phy->pipe_clk_ext_src);
		if (ret)
			goto unwind;
		ret = clk_prepare_enable(phy->pipe_clk);
		if (ret)
			goto restore_parent;
		phy->clock_votes |= BIT(5);
		phy->clk_enabled = true;
		return 0;
	}
	phy->clk_enabled = false;
	if (phy->clock_votes & BIT(5)) {
		clk_disable_unprepare(phy->pipe_clk);
		phy->clock_votes &= ~BIT(5);
restore_parent:
		cleanup = clk_set_parent(phy->pipe_clk_mux, phy->ref_clk_src);
		if (cleanup) {
			dev_err(phy->phy.dev, "Could not restore pipe parent: %d\n", cleanup);
			if (!ret)
				ret = cleanup;
		}
	}
unwind:
	for (i = ARRAY_SIZE(clocks) - 2; i >= 0; i--) {
		if (phy->clock_votes & BIT(i)) {
			clk_disable_unprepare(clocks[i]);
			phy->clock_votes &= ~BIT(i);
		}
	}
	return ret;
}

static int usb3_get_regulators(struct msm_ssphy_qmp  *phy)
{
	struct device *dev = phy->phy.dev;
	int ret = 0;

	phy->refgen = NULL;

	phy->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(phy->vdd)) {
		ret = PTR_ERR(phy->vdd);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "fail to get vdd supply\n");
		return ret;
	}

	phy->core_ldo = devm_regulator_get(dev, "core");
	if (IS_ERR(phy->core_ldo)) {
		ret = PTR_ERR(phy->core_ldo);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "fail to get core ldo supply\n");
		return ret;
	}

	phy->usb3_dp_phy_gdsc = devm_regulator_get_optional(dev, "usb3_dp_phy_gdsc");
	if (IS_ERR(phy->usb3_dp_phy_gdsc)) {
		ret = PTR_ERR(phy->usb3_dp_phy_gdsc);
		if (ret != -ENODEV) {
			dev_err(dev, "fail to get usb3_dp_phy_gdsc(%d)\n", ret);
			return ret;
		}
		phy->usb3_dp_phy_gdsc = NULL;
	}

	if (of_property_read_bool(dev->of_node, "refgen-supply")) {
		phy->refgen = devm_regulator_get_optional(dev, "refgen");
		if (IS_ERR(phy->refgen))
			return PTR_ERR(phy->refgen);
	}

	return 0;
}
static int msm_ssphy_qmp_probe(struct platform_device *pdev)
{
	struct msm_ssphy_qmp *phy;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret = 0, size = 0, len;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	phy->phy_type = USB3;
	if (of_device_is_compatible(dev->of_node,
			"qcom,usb-ssphy-qmp-dp-combo"))
		phy->phy_type = USB3_AND_DP;

	if (of_device_is_compatible(dev->of_node,
			"qcom,usb-ssphy-qmp-usb3-or-dp"))
		phy->phy_type = USB3_OR_DP;

	ret = msm_ssphy_qmp_get_clks(phy, dev);
	if (ret)
		goto err;

	phy->phy_reset = devm_reset_control_get(dev, "phy_reset");
	if (IS_ERR(phy->phy_reset)) {
		ret = PTR_ERR(phy->phy_reset);
		dev_dbg(dev, "failed to get phy_reset\n");
		goto err;
	}

	if (phy->phy_type == USB3_AND_DP) {
		phy->global_phy_reset = devm_reset_control_get(dev,
						"global_phy_reset");
		if (IS_ERR(phy->global_phy_reset)) {
			ret = PTR_ERR(phy->global_phy_reset);
			dev_dbg(dev, "failed to get global_phy_reset\n");
			goto err;
		}
	} else {
		phy->phy_phy_reset = devm_reset_control_get(dev,
						"phy_phy_reset");
		if (IS_ERR(phy->phy_phy_reset)) {
			ret = PTR_ERR(phy->phy_phy_reset);
			dev_dbg(dev, "failed to get phy_phy_reset\n");
			goto err;
		}
	}

	of_get_property(dev->of_node, "qcom,qmp-phy-reg-offset", &size);
	if (size > 0 && !(size % sizeof(u32))) {
		phy->phy_reg = devm_kzalloc(dev, PHY_REG_SIZE, GFP_KERNEL);
		if (phy->phy_reg) {
			phy->reg_offset_cnt = (size / sizeof(*phy->phy_reg));
			if (phy->reg_offset_cnt > USB3_PHY_REG_MAX) {
				dev_err(dev, "invalid reg offset count\n");
				return -EINVAL;
			}

			ret = of_property_read_u32_array(dev->of_node,
				"qcom,qmp-phy-reg-offset",
				phy->phy_reg, phy->reg_offset_cnt);
			if (ret)
				return ret;
		} else {
			dev_err(dev, "err mem alloc for qmp_phy_reg_offset\n");
			return -ENOMEM;
		}
	} else {
		dev_err(dev, "err provide qcom,qmp-phy-reg-offset\n");
		return -EINVAL;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"qmp_phy_base");
	if (!res) {
		dev_err(dev, "failed getting qmp_phy_base\n");
		return -ENODEV;
	}

	/*
	 * For USB QMP DP combo PHY, common set of registers shall be accessed
	 * by DP driver as well.
	 */
	phy->base_size = resource_size(res);
	phy->base = devm_ioremap(dev, res->start, phy->base_size);
	if (!phy->base) {
		ret = -ENOMEM;
		goto err;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
			"vls_clamp_reg");
	if (res) {
		phy->vls_clamp_reg = devm_ioremap_resource(dev, res);
		if (IS_ERR(phy->vls_clamp_reg)) {
			dev_err(dev, "err getting vls_clamp_reg address\n");
			return PTR_ERR(phy->vls_clamp_reg);
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
			"pcs_clamp_enable_reg");
	if (res) {
		phy->pcs_clamp_enable_reg = devm_ioremap_resource(dev, res);
		if (IS_ERR(phy->pcs_clamp_enable_reg)) {
			dev_err(dev, "err getting pcs_clamp_enable_reg address.\n");
			return PTR_ERR(phy->pcs_clamp_enable_reg);
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
			"tcsr_usb3_dp_phymode");
	if (res) {
		phy->tcsr_usb3_dp_phymode = devm_ioremap_resource(dev, res);
		if (IS_ERR(phy->tcsr_usb3_dp_phymode)) {
			dev_err(dev, "err getting tcsr_usb3_dp_phymode addr\n");
			return PTR_ERR(phy->tcsr_usb3_dp_phymode);
		}
	}

	of_get_property(dev->of_node, "qcom,qmp-phy-init-seq", &size);
	if (size) {
		if (size % (2 * sizeof(*phy->qmp_phy_init_seq))) {
			dev_err(dev, "invalid init_seq_len\n");
			return -EINVAL;
		}

		phy->qmp_phy_init_seq = devm_kzalloc(dev, size, GFP_KERNEL);
		if (!phy->qmp_phy_init_seq)
			return -ENOMEM;

		phy->init_seq_len = (size / sizeof(*phy->qmp_phy_init_seq));
		ret = of_property_read_u32_array(dev->of_node,
				"qcom,qmp-phy-init-seq",
				phy->qmp_phy_init_seq,
				phy->init_seq_len);
		if (ret)
			return ret;
	} else {
		dev_err(dev, "error need qmp-phy-init-seq\n");
		return -EINVAL;
	}

	ret = qmp_validate_tables(phy);
	if (ret)
		return ret;

	/* Set default core voltage values */
	phy->core_voltage_levels[CORE_LEVEL_NONE] = 0;
	phy->core_voltage_levels[CORE_LEVEL_MIN] = USB_SSPHY_1P2_VOL_MIN;
	phy->core_voltage_levels[CORE_LEVEL_MAX] = USB_SSPHY_1P2_VOL_MAX;

	if (of_get_property(dev->of_node, "qcom,core-voltage-level", &len)) {
		if (len != sizeof(phy->core_voltage_levels))
			return -EINVAL;
		ret = of_property_read_u32_array(dev->of_node,
				"qcom,core-voltage-level",
				(u32 *)phy->core_voltage_levels,
				len / sizeof(u32));
		if (ret) {
			dev_err(dev, "err qcom,core-voltage-level property\n");
			goto err;
		}
	}

	if (of_property_read_s32(dev->of_node, "qcom,core-max-load-uA",
				&phy->core_max_uA) || !phy->core_max_uA)
		phy->core_max_uA = USB_SSPHY_HPM_LOAD;

	if (of_get_property(dev->of_node, "qcom,vdd-voltage-level", &len) &&
		len == sizeof(phy->vdd_levels)) {
		ret = of_property_read_u32_array(dev->of_node,
				"qcom,vdd-voltage-level",
				(u32 *) phy->vdd_levels,
				len / sizeof(u32));
		if (ret) {
			dev_err(dev, "err qcom,vdd-voltage-level property\n");
			goto err;
		}
	} else {
		ret = -EINVAL;
		dev_err(dev, "error invalid inputs for vdd-voltage-level\n");
		goto err;
	}

	if (of_get_property(dev->of_node, "qcom,refgen-voltage-level", &len)) {
		if (len != sizeof(phy->refgen_levels))
			return -EINVAL;
		ret = of_property_read_u32_array(dev->of_node,
				"qcom,refgen-voltage-level",
				(u32 *) phy->refgen_levels,
				len / sizeof(u32));
		if (ret)
			return ret;
	}

	if (of_property_read_s32(dev->of_node, "qcom,vdd-max-load-uA",
				&phy->vdd_max_uA) || !phy->vdd_max_uA)
		phy->vdd_max_uA = USB_SSPHY_HPM_LOAD;

	mutex_init(&phy->lock);
	platform_set_drvdata(pdev, phy);

	phy->phy.dev			= dev;
	phy->phy.init			= msm_ssphy_qmp_init;
	phy->phy.set_suspend		= msm_ssphy_qmp_set_suspend;
	phy->phy.notify_connect		= msm_ssphy_qmp_notify_connect;
	phy->phy.notify_disconnect	= msm_ssphy_qmp_notify_disconnect;

	ret = usb3_get_regulators(phy);
	if (ret)
		goto err;

	if (phy->vdd_max_uA < 0 || phy->core_max_uA < 0 ||
	    phy->vdd_levels[0] < 0 || phy->vdd_levels[1] <= 0 ||
	    phy->vdd_levels[2] < phy->vdd_levels[1] ||
	    phy->core_voltage_levels[0] < 0 || phy->core_voltage_levels[1] <= 0 ||
	    phy->core_voltage_levels[2] < phy->core_voltage_levels[1] ||
	    (phy->refgen && (phy->refgen_levels[0] < 0 || phy->refgen_levels[1] <= 0 ||
			     phy->refgen_levels[2] < phy->refgen_levels[1])))
		return -EINVAL;

	/* Placed at the end to ensure the probe is complete */
	ret = usb_add_phy_dev(&phy->phy);

err:
	return ret;
}

static void msm_ssphy_qmp_remove(struct platform_device *pdev)
{
	struct msm_ssphy_qmp *phy = platform_get_drvdata(pdev);
	int ret;

	usb_remove_phy(&phy->phy);
	mutex_lock(&phy->lock);
	phy->removing = true;
	ret = msm_ssphy_qmp_enable_clks(phy, false);
	if (ret)
		dev_err(phy->phy.dev, "Clock release failed: %d\n", ret);
	ret = msm_ssusb_qmp_ldo_enable(phy, 0);
	if (ret)
		dev_err(phy->phy.dev, "Supply release failed: %d\n", ret);
	mutex_unlock(&phy->lock);
}

static struct platform_driver msm_ssphy_qmp_driver = {
	.probe		= msm_ssphy_qmp_probe,
	.remove		= msm_ssphy_qmp_remove,
	.driver = {
		.name	= "msm-usb-ssphy-qmp",
		.of_match_table = of_match_ptr(msm_usb_id_table),
	},
};

module_platform_driver(msm_ssphy_qmp_driver);

MODULE_DESCRIPTION("MSM USB SS QMP PHY driver");
MODULE_LICENSE("GPL");
