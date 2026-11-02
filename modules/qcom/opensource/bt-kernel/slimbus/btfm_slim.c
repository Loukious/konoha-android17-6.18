// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/debugfs.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include "btpower.h"
#include "btfm_slim.h"
#include "btfm_slim_slave.h"
#if IS_ENABLED(CONFIG_SLIM_BTFM_CODEC)
#include "btfm_slim_hw_interface.h"
#endif

#define DELAY_FOR_PORT_OPEN_MS (200)
#define SLIM_MANF_ID_QCOM	0x217
#define SLIM_PROD_CODE		0x221
#define BT_CMD_SLIM_TEST	0xbfac

struct class *btfm_slim_class;

struct btfmslim *btfm_slim_drv_data;

static int btfm_num_ports_open;

static DEFINE_MUTEX(btfm_slim_lifecycle_lock);

int btfm_slim_write(struct btfmslim *btfmslim,
		uint16_t reg, uint8_t reg_val, uint8_t pgd)
{
	int ret = -1;
	uint32_t reg_addr;
	int slim_write_tries = SLIM_SLAVE_RW_MAX_TRIES;

	BTFMSLIM_INFO("Write to %s", pgd?"PGD":"IFD");
	reg_addr = SLIM_SLAVE_REG_OFFSET + reg;

	for ( ; slim_write_tries != 0; slim_write_tries--) {
		mutex_lock(&btfmslim->xfer_lock);
		ret = slim_writeb(pgd ? btfmslim->slim_pgd :
			&btfmslim->slim_ifd, reg_addr, reg_val);
		mutex_unlock(&btfmslim->xfer_lock);
		if (ret) {
			BTFMSLIM_DBG("retrying to Write 0x%02x to reg 0x%x ret %d",
					 reg_val, reg_addr, ret);
		} else {
			BTFMSLIM_DBG("Written 0x%02x to reg 0x%x ret %d", reg_val, reg_addr, ret);
			break;
		}

		usleep_range(5000, 5100);
	}
	if (ret) {
		BTFMSLIM_DBG("retrying to Write 0x%02x to reg 0x%x ret %d",
				reg_val, reg_addr, ret);
	}
	return ret;
}

int btfm_slim_read(struct btfmslim *btfmslim, uint32_t reg, uint8_t pgd)
{
	int ret = -1;
	int slim_read_tries = SLIM_SLAVE_RW_MAX_TRIES;
	uint32_t reg_addr;
	BTFMSLIM_DBG("Read from %s", pgd?"PGD":"IFD");
	reg_addr = SLIM_SLAVE_REG_OFFSET + reg;

	for ( ; slim_read_tries != 0; slim_read_tries--) {
		mutex_lock(&btfmslim->xfer_lock);

		ret = slim_readb(pgd ? btfmslim->slim_pgd :
				&btfmslim->slim_ifd, reg_addr);
		BTFMSLIM_DBG("Read 0x%02x from reg 0x%x", ret, reg_addr);
		mutex_unlock(&btfmslim->xfer_lock);
		if (ret >= 0)
			break;
		usleep_range(5000, 5100);
	}
	return ret;
}

/* io_lock serializes DAI state and retryable cleanup. */
static int btfm_slim_cleanup_ch(struct btfmslim *priv, struct btfmslim_ch *ch)
{
	struct btfm_slim_codec_dai_data *dai = &ch->dai;
	int i, ret, chipset_ver;

	if (!dai->sruntime)
		return 0;
	if (dai->needs_disable) {
		/* These rates require the native disconnect-before-remove order. */
		if (dai->rxport && (dai->sconfig.rate == 44100 ||
				    dai->sconfig.rate == 88200)) {
			ret = slim_stream_unprepare_disconnect_port(dai->sruntime, true, false);
			if (ret)
				return ret;
		}
		ret = slim_stream_disable(dai->sruntime);
		if (ret)
			return ret;
		dai->needs_disable = false;
	}
	if (dai->prepared) {
		ret = slim_stream_unprepare_disconnect_port(dai->sruntime, true, true);
		if (ret)
			return ret;
		dai->prepared = false;
	}
	for (i = 0; i < dai->channels; i++) {
		if (!(dai->enabled_ports & BIT(i)))
			continue;
		ret = priv->vendor_port_en(priv, ch[i].port, dai->rxport, 0);
		if (ret)
			return ret;
		dai->enabled_ports &= ~BIT(i);
	}
	ret = slim_stream_free(dai->sruntime);
	if (ret)
		return ret;
	if (dai->active)
		btfm_num_ports_open--;
	kfree(dai->sconfig.chs);
	memset(dai, 0, sizeof(*dai));
	chipset_ver = btpower_get_chipset_version();

	if (btfm_num_ports_open == 0 && (chipset_ver == QCA_HSP_SOC_ID_0200 ||
		chipset_ver == QCA_HSP_SOC_ID_0210 ||
		chipset_ver == QCA_HSP_SOC_ID_1201 ||
		chipset_ver == QCA_HSP_SOC_ID_1211 ||
		chipset_ver == QCA_HAMILTON_SOC_ID_0100 ||
		chipset_ver == QCA_HAMILTON_SOC_ID_0101 ||
		chipset_ver == QCA_HAMILTON_SOC_ID_0200 ||
		chipset_ver == QCA_APACHE_SOC_ID_0100 ||
		chipset_ver == QCA_APACHE_SOC_ID_0110 ||
		chipset_ver == QCA_APACHE_SOC_ID_0121 ||
		chipset_ver == QCA_MOSELLE_SOC_ID_0100 ||
		chipset_ver == QCA_MOSELLE_SOC_ID_0110 ||
		chipset_ver == QCA_MOSELLE_SOC_ID_0120)) {
		BTFMSLIM_INFO("SB reset needed after all ports disabled, sleeping");
		msleep(DELAY_FOR_PORT_OPEN_MS);
	}

	return 0;
}

static int btfm_slim_check_channels(struct btfmslim *priv, struct btfmslim_ch *ch,
				    u8 rxport, u8 count)
{
	struct btfmslim_ch *table;
	unsigned int size, i, j;

	if (!priv || !ch || rxport > 1 || !count || count > 2)
		return -EINVAL;
	table = rxport ? priv->rx_chs : priv->tx_chs;
	size = rxport ? priv->num_rx_port : priv->num_tx_port;
	for (i = 0; i < size; i++) {
		if (&table[i] != ch)
			continue;
		if (count > size - i)
			return -EINVAL;
		for (j = 0; j < count; j++)
			if (ch[j].port >= 32 || ch[j].id != ch->id)
				return -EINVAL;
		return 0;
	}
	return -EINVAL;
}

int btfm_slim_enable_ch(struct btfmslim *priv, struct btfmslim_ch *ch,
			u8 rxport, u32 rates, u8 count)
{
	struct btfm_slim_codec_dai_data *dai;
	int i, ret, cleanup;

	ret = btfm_slim_check_channels(priv, ch, rxport, count);
	if (ret)
		return ret;
	if (priv->stopping)
		return -ENODEV;
	dai = &ch->dai;
	if (dai->sruntime) {
		if (dai->active)
			return -EISCONN;
		ret = btfm_slim_cleanup_ch(priv, ch);
		if (ret)
			return ret;
	}
	dai->sconfig = (struct slim_stream_config) {
		.bps = priv->bps, .direction = priv->direction,
		.rate = rates, .ch_count = count,
	};
	dai->sconfig.chs = kcalloc(count, sizeof(*dai->sconfig.chs), GFP_KERNEL);
	if (!dai->sconfig.chs)
		return -ENOMEM;
	dai->sruntime = slim_stream_allocate(priv->slim_pgd, "BTFM_SLIM");
	if (IS_ERR(dai->sruntime)) {
		ret = PTR_ERR(dai->sruntime);
		dai->sruntime = NULL;
		kfree(dai->sconfig.chs);
		dai->sconfig.chs = NULL;
		return ret;
	}
	dai->channels = count;
	dai->rxport = rxport;
	for (i = 0; i < count; i++) {
		dai->sconfig.chs[i] = ch[i].ch;
		dai->sconfig.port_mask |= BIT(ch[i].port);
		if (priv->vendor_port_en) {
			/* A failed multi-register enable may have changed hardware. */
			dai->enabled_ports |= BIT(i);
			ret = priv->vendor_port_en(priv, ch[i].port, rxport, 1);
			if (ret)
				goto error;
		}
	}
	ret = slim_stream_prepare(dai->sruntime, &dai->sconfig);
	/* Unprepare is idempotent, including partial connection rollback. */
	dai->prepared = true;
	if (ret)
		goto error;
	dai->needs_disable = true;
	ret = slim_stream_enable(dai->sruntime);
	if (ret)
		goto error;
	dai->active = true;
	btfm_num_ports_open++;
	WRITE_ONCE(btfm_feedback_ch_setting, 0);
	return 0;
error:
	cleanup = btfm_slim_cleanup_ch(priv, ch);
	if (cleanup)
		BTFMSLIM_ERR("channel cleanup retained for retry: %d", cleanup);
	return ret;
}

int btfm_slim_disable_ch(struct btfmslim *priv, struct btfmslim_ch *ch,
			 u8 rxport, u8 count)
{
	int ret = btfm_slim_check_channels(priv, ch, rxport, count);

	if (ret)
		return ret;
	return btfm_slim_cleanup_ch(priv, ch);
}

static int btfm_slim_alloc_port(struct btfmslim *btfmslim)
{
	int ret = -EINVAL, i;
	int  chipset_ver;
	struct btfmslim_ch *rx_chs;
	struct btfmslim_ch *tx_chs;

	if (!btfmslim)
		return ret;

	chipset_ver = btpower_get_chipset_version();
	BTFMSLIM_INFO("chipset soc version:%x", chipset_ver);

	rx_chs = btfmslim->rx_chs;
	tx_chs = btfmslim->tx_chs;
	if (!rx_chs || !tx_chs)
		return -EINVAL;
	if ((chipset_ver >=  QCA_CHEROKEE_SOC_ID_0310) &&
		(chipset_ver <=  QCA_CHEROKEE_SOC_ID_0320_UMC)) {
		for (i = 0; (tx_chs->port != BTFM_SLIM_PGD_PORT_LAST) &&
		(i < BTFM_SLIM_NUM_CODEC_DAIS); i++, tx_chs++) {
			if (tx_chs->port == SLAVE_SB_PGD_PORT_TX1_FM)
				tx_chs->port = CHRKVER3_SB_PGD_PORT_TX1_FM;
			else if (tx_chs->port == SLAVE_SB_PGD_PORT_TX2_FM)
				tx_chs->port = CHRKVER3_SB_PGD_PORT_TX2_FM;
			BTFMSLIM_INFO("Tx port:%d", tx_chs->port);
		}
		tx_chs = btfmslim->tx_chs;
	}
	if (!rx_chs || !tx_chs)
		return ret;

	return 0;
}

static int btfm_slim_get_logical_addr(struct slim_device *slim)
{
	int ret = 0;
	const unsigned long timeout = jiffies +
			      msecs_to_jiffies(SLIM_SLAVE_PRESENT_TIMEOUT);
	BTFMSLIM_INFO("");

	do {

		ret = slim_get_logical_addr(slim);
		if (!ret)  {
			BTFMSLIM_DBG("Assigned l-addr: 0x%x", slim->laddr);
			break;
		}
		/* Give SLIMBUS time to report present and be ready. */
		usleep_range(1000, 1100);
		BTFMSLIM_DBG("retyring get logical addr");
	} while (time_before(jiffies, timeout));
	return ret;
}

int btfm_slim_hw_init(struct btfmslim *btfmslim)
{
	int ret = -1;
	int chipset_ver;
	struct slim_device *slim;
	struct slim_device *slim_ifd;

	BTFMSLIM_DBG("");
	if (!btfmslim)
		return -EINVAL;

	mutex_lock(&btfmslim->io_lock);
	if (btfmslim->stopping) {
		ret = -ENODEV;
		goto error;
	}
	if (btfmslim->enabled) {
		BTFMSLIM_DBG("Already enabled");
		ret = 0;
		goto error;
	}

	slim = btfmslim->slim_pgd;
	slim_ifd = &btfmslim->slim_ifd;

	BTFMSLIM_INFO(
		"PGD Enum Addr: mfr id:%.02x prod code:%.02x dev ind:%.02x ins:%.02x",
		slim->e_addr.manf_id, slim->e_addr.prod_code,
		slim->e_addr.dev_index, slim->e_addr.instance);

	chipset_ver = btpower_get_chipset_version();
	BTFMSLIM_INFO("chipset soc version:%x", chipset_ver);

	if (chipset_ver == QCA_HSP_SOC_ID_0100 ||
		chipset_ver == QCA_HSP_SOC_ID_0110 ||
		chipset_ver == QCA_HSP_SOC_ID_0200 ||
		chipset_ver == QCA_HSP_SOC_ID_0210 ||
		chipset_ver == QCA_HSP_SOC_ID_1201 ||
		chipset_ver == QCA_HSP_SOC_ID_1211) {
		BTFMSLIM_INFO("chipset is hastings prime, overwriting EA");
		slim->is_laddr_valid = false;
		slim->e_addr.manf_id = SLIM_MANF_ID_QCOM;
		slim->e_addr.prod_code = SLIM_PROD_CODE;
		slim->e_addr.dev_index = 0x01;
		slim->e_addr.instance = 0x0;
		/* we are doing this to indicate that this is not a child node
		 * (doesn't have call back functions). Needed only for querying
		 * logical address.
		 */
		slim_ifd->dev.driver = NULL;
		slim_ifd->ctrl = btfmslim->slim_pgd->ctrl; //slimbus controller structure.
		slim_ifd->is_laddr_valid = false;
		slim_ifd->e_addr.manf_id = SLIM_MANF_ID_QCOM;
		slim_ifd->e_addr.prod_code = SLIM_PROD_CODE;
		slim_ifd->e_addr.dev_index = 0x0;
		slim_ifd->e_addr.instance = 0x0;
		slim_ifd->laddr = 0x0;
	} else if (chipset_ver == QCA_MOSELLE_SOC_ID_0100 ||
		chipset_ver == QCA_MOSELLE_SOC_ID_0110 ||
		chipset_ver == QCA_MOSELLE_SOC_ID_0120) {
		BTFMSLIM_INFO("chipset is Moselle, overwriting EA");
		slim->is_laddr_valid = false;
		slim->e_addr.manf_id = SLIM_MANF_ID_QCOM;
		slim->e_addr.prod_code = 0x222;
		slim->e_addr.dev_index = 0x01;
		slim->e_addr.instance = 0x0;
		/* we are doing this to indicate that this is not a child node
		 * (doesn't have call back functions). Needed only for querying
		 * logical address.
		 */
		slim_ifd->dev.driver = NULL;
		slim_ifd->ctrl = btfmslim->slim_pgd->ctrl; //slimbus controller structure.
		slim_ifd->is_laddr_valid = false;
		slim_ifd->e_addr.manf_id = SLIM_MANF_ID_QCOM;
		slim_ifd->e_addr.prod_code = 0x222;
		slim_ifd->e_addr.dev_index = 0x0;
		slim_ifd->e_addr.instance = 0x0;
		slim_ifd->laddr = 0x0;
	} else if (chipset_ver == QCA_HAMILTON_SOC_ID_0100 ||
		chipset_ver ==  QCA_HAMILTON_SOC_ID_0101 ||
		chipset_ver ==  QCA_HAMILTON_SOC_ID_0200) {
		BTFMSLIM_INFO("chipset is Hamliton, overwriting EA");
		slim->is_laddr_valid = false;
		slim->e_addr.manf_id = SLIM_MANF_ID_QCOM;
		slim->e_addr.prod_code = 0x220;
		slim->e_addr.dev_index = 0x01;
		slim->e_addr.instance = 0x0;
		/* we are doing this to indicate that this is not a child node
		 * (doesn't have call back functions). Needed only for querying
		 * logical address.
		 */
		slim_ifd->dev.driver = NULL;
		slim_ifd->ctrl = btfmslim->slim_pgd->ctrl; //slimbus controller structure.
		slim_ifd->is_laddr_valid = false;
		slim_ifd->e_addr.manf_id = SLIM_MANF_ID_QCOM;
		slim_ifd->e_addr.prod_code = 0x220;
		slim_ifd->e_addr.dev_index = 0x0;
		slim_ifd->e_addr.instance = 0x0;
		slim_ifd->laddr = 0x0;
	} else if (chipset_ver == QCA_CHEROKEE_SOC_ID_0200 ||
		chipset_ver ==  QCA_CHEROKEE_SOC_ID_0201  ||
		chipset_ver ==  QCA_CHEROKEE_SOC_ID_0210  ||
		chipset_ver ==  QCA_CHEROKEE_SOC_ID_0211  ||
		chipset_ver ==  QCA_CHEROKEE_SOC_ID_0310  ||
		chipset_ver ==  QCA_CHEROKEE_SOC_ID_0320  ||
		chipset_ver ==  QCA_CHEROKEE_SOC_ID_0320_UMC  ||
		chipset_ver ==  QCA_APACHE_SOC_ID_0100  ||
		chipset_ver ==  QCA_APACHE_SOC_ID_0110  ||
		chipset_ver ==  QCA_APACHE_SOC_ID_0120 ||
		chipset_ver ==  QCA_APACHE_SOC_ID_0121 ||
		chipset_ver ==  QCA_COMANCHE_SOC_ID_0101 ||
		chipset_ver ==  QCA_COMANCHE_SOC_ID_0110 ||
		chipset_ver ==  QCA_COMANCHE_SOC_ID_0120 ||
		chipset_ver ==  QCA_COMANCHE_SOC_ID_0130 ||
		chipset_ver ==  QCA_COMANCHE_SOC_ID_4130 ||
		chipset_ver ==  QCA_COMANCHE_SOC_ID_5120 ||
		chipset_ver ==  QCA_COMANCHE_SOC_ID_5130 ) {
		BTFMSLIM_INFO("chipset is Chk/Apache/CMC, overwriting EA");
		slim->is_laddr_valid = false;
		slim->e_addr.manf_id = SLIM_MANF_ID_QCOM;
		slim->e_addr.prod_code = 0x220;
		slim->e_addr.dev_index = 0x01;
		slim->e_addr.instance = 0x0;
		/* we are doing this to indicate that this is not a child node
		 * (doesn't have call back functions). Needed only for querying
		 * logical address.
		 */
		slim_ifd->dev.driver = NULL;
		slim_ifd->ctrl = btfmslim->slim_pgd->ctrl; //slimbus controller structure.
		slim_ifd->is_laddr_valid = false;
		slim_ifd->e_addr.manf_id = SLIM_MANF_ID_QCOM;
		slim_ifd->e_addr.prod_code = 0x220;
		slim_ifd->e_addr.dev_index = 0x0;
		slim_ifd->e_addr.instance = 0x0;
		slim_ifd->laddr = 0x0;
	}
		BTFMSLIM_INFO(
			"PGD Enum Addr: manu id:%.02x prod code:%.02x dev idx:%.02x instance:%.02x",
			slim->e_addr.manf_id, slim->e_addr.prod_code,
			slim->e_addr.dev_index, slim->e_addr.instance);

		BTFMSLIM_INFO(
			"IFD Enum Addr: manu id:%.02x prod code:%.02x dev idx:%.02x instance:%.02x",
			slim_ifd->e_addr.manf_id, slim_ifd->e_addr.prod_code,
			slim_ifd->e_addr.dev_index, slim_ifd->e_addr.instance);

	/* Assign Logical Address for PGD (Ported Generic Device)
	 * enumeration address
	 */
	ret = btfm_slim_get_logical_addr(btfmslim->slim_pgd);
	if (ret) {
		BTFMSLIM_ERR("failed to get slimbus logical address: %d", ret);
		goto error;
	}

	/* Assign Logical Address for Ported Generic Device
	 * enumeration address
	 */
	ret = btfm_slim_get_logical_addr(&btfmslim->slim_ifd);
	if (ret) {
		BTFMSLIM_ERR("failed to get slimbus logical address: %d", ret);
		goto error;
	}

	ret = btfm_slim_alloc_port(btfmslim);
	if (ret != 0)
		goto error;
	/* Start vendor specific initialization and get port information */
	if (btfmslim->vendor_init)
		ret = btfmslim->vendor_init(btfmslim);

	/* Only when all registers read/write successfully, it set to
	 * enabled status
	 */
	if (!ret)
		btfmslim->enabled = 1;
error:
	mutex_unlock(&btfmslim->io_lock);
	return ret;
}

int btfm_slim_hw_deinit(struct btfmslim *priv)
{
	if (!priv)
		return -EINVAL;
	mutex_lock(&priv->io_lock);
	priv->enabled = false;
	mutex_unlock(&priv->io_lock);
	return 0;
}

#if IS_ENABLED (CONFIG_BTFM_SLIM)
static void btfm_slim_get_hwep_details(struct slim_device *dev, struct btfmslim *btfm_slim)
{
}
#else
static void btfm_slim_get_hwep_details(struct slim_device *slim, struct btfmslim *btfm_slim)
{
	struct device_node *np = slim->dev.of_node;
	const __be32 *prop;
	struct btfmslim_ch *rx_chs = btfm_slim->rx_chs;
	struct btfmslim_ch *tx_chs = btfm_slim->tx_chs;
	int len;

	prop = of_get_property(np, "qcom,btslim-address", &len);
	if (prop && len >= sizeof(*prop)) {
		btfm_slim->device_id = be32_to_cpup(&prop[0]);
		BTFMSLIM_DBG("hwep slim address define in dt %08x", btfm_slim->device_id);
	} else {
		BTFMSLIM_ERR("btslim-address is not defined in dt using default address");
		btfm_slim->device_id = 0;
	}

	if (!rx_chs || !tx_chs) {
		BTFMSLIM_ERR("either rx/tx channels are configured to null");
		return;
	}

	prop = of_get_property(np, "qcom,btslimrx-channels", &len);
	if (prop && len >= 2 * sizeof(*prop)) {
		/* Native DT uses two channel numbers. */
		rx_chs[0].ch = (uint8_t)be32_to_cpup(&prop[0]);
		rx_chs[1].ch = (uint8_t)be32_to_cpup(&prop[1]);
		BTFMSLIM_DBG("Rx: id\tname\tport\tch");
		BTFMSLIM_DBG("    %d\t%s\t%d\t%d", rx_chs[0].id,
				rx_chs[0].name, rx_chs[0].port,
				rx_chs[0].ch);
		BTFMSLIM_DBG("    %d\t%s\t%d\t%d", rx_chs[1].id,
				rx_chs[1].name, rx_chs[1].port,
				rx_chs[1].ch);
	} else {
		BTFMSLIM_ERR("btslimrx channels are missing in dt using default values");
	}

	prop = of_get_property(np, "qcom,btslimtx-channels", &len);
	if (prop && len >= 2 * sizeof(*prop)) {
		/* Native DT uses two channel numbers. */
		tx_chs[0].ch = (uint8_t)be32_to_cpup(&prop[0]);
		tx_chs[1].ch = (uint8_t)be32_to_cpup(&prop[1]);
		BTFMSLIM_DBG("Tx: id\tname\tport\tch");
		BTFMSLIM_DBG("    %d\t%s\t%d\t%x", tx_chs[0].id,
				tx_chs[0].name, tx_chs[0].port,
				tx_chs[0].ch);
		BTFMSLIM_DBG("    %d\t%s\t%d\t%x", tx_chs[1].id,
				tx_chs[1].name, tx_chs[1].port,
				tx_chs[1].ch);
	} else {
		BTFMSLIM_ERR("btslimtx channels are missing in dt using default values");
	}

}
#endif
static int btfm_slim_status(struct slim_device *sdev,
			    enum slim_device_status status)
{
	struct btfmslim *priv = dev_get_drvdata(&sdev->dev);

	if (status != SLIM_DEVICE_STATUS_DOWN)
		return 0;
	mutex_lock(&priv->io_lock);
	priv->enabled = false;
	mutex_unlock(&priv->io_lock);
	return 0;
}

static void btfm_slim_free(struct kref *ref)
{
	struct btfmslim *priv = container_of(ref, struct btfmslim, ref);

	kfree(priv->rx_chs);
	kfree(priv->tx_chs);
	kfree(priv);
}

static int btfm_slim_open(struct inode *inode, struct file *file)
{
	int ret = -ENODEV;

	mutex_lock(&btfm_slim_lifecycle_lock);
	if (btfm_slim_drv_data && !btfm_slim_drv_data->stopping &&
	    inode->i_cdev == btfm_slim_drv_data->cdev) {
		kref_get(&btfm_slim_drv_data->ref);
		file->private_data = btfm_slim_drv_data;
		ret = 0;
	}
	mutex_unlock(&btfm_slim_lifecycle_lock);
	return ret;
}

static int btfm_slim_release(struct inode *inode, struct file *file)
{
	struct btfmslim *priv = file->private_data;

	kref_put(&priv->ref, btfm_slim_free);
	return 0;
}

static long btfm_slim_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct btfmslim *priv = file->private_data;
	int ret = -ENOTTY;

	mutex_lock(&btfm_slim_lifecycle_lock);
	if (priv != btfm_slim_drv_data || priv->stopping)
		ret = -ENODEV;
	else if (cmd == BT_CMD_SLIM_TEST)
		ret = btfm_slim_hw_init(priv);
	mutex_unlock(&btfm_slim_lifecycle_lock);
	return ret;
}

static const struct file_operations bt_dev_fops = {
	.owner = THIS_MODULE,
	.open = btfm_slim_open,
	.release = btfm_slim_release,
	.unlocked_ioctl = btfm_slim_ioctl,
	.compat_ioctl = btfm_slim_ioctl,
};

static int btfm_slim_probe(struct slim_device *slim)
{
	struct btfmslim *priv;
	struct device *chardev;
	int ret;

	if (!slim->ctrl)
		return -EINVAL;
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	kref_init(&priv->ref);
	mutex_init(&priv->io_lock);
	mutex_init(&priv->xfer_lock);
	priv->slim_pgd = slim;
	priv->dev = &slim->dev;
	priv->stopping = true;
	priv->rx_chs = kmemdup(slave_rxport, sizeof(slave_rxport), GFP_KERNEL);
	priv->tx_chs = kmemdup(slave_txport, sizeof(slave_txport), GFP_KERNEL);
	if (!priv->rx_chs || !priv->tx_chs) {
		ret = -ENOMEM;
		goto free;
	}
	priv->num_rx_port = ARRAY_SIZE(slave_rxport) - 1;
	priv->num_tx_port = ARRAY_SIZE(slave_txport) - 1;
	priv->vendor_init = SLIM_SLAVE_INIT;
	priv->vendor_port_en = SLIM_SLAVE_PORT_EN;
	mutex_lock(&btfm_slim_lifecycle_lock);
	if (btfm_slim_drv_data) {
		mutex_unlock(&btfm_slim_lifecycle_lock);
		ret = -EBUSY;
		goto free;
	}
	btfm_slim_drv_data = priv;
	btfm_num_ports_open = 0;
	dev_set_drvdata(&slim->dev, priv);
	mutex_unlock(&btfm_slim_lifecycle_lock);
	/* This legacy forwarding path is absent from the active ROM config. */
#if IS_ENABLED(CONFIG_BT_SLIM)
	ret = btpower_register_slimdev(&slim->dev);
	if (ret)
		goto clear;
#endif
	btfm_slim_get_hwep_details(slim, priv);
	ret = alloc_chrdev_region(&priv->devt, 0, 1, "btfm_slim");
	if (ret)
		goto clear;
	priv->cdev = cdev_alloc();
	if (!priv->cdev) {
		ret = -ENOMEM;
		goto unregister_region;
	}
	priv->cdev->owner = THIS_MODULE;
	priv->cdev->ops = &bt_dev_fops;
	ret = cdev_add(priv->cdev, priv->devt, 1);
	if (ret) {
		kobject_put(&priv->cdev->kobj);
		goto unregister_region;
	}
	btfm_slim_class = class_create("btfmslim-dev");
	if (IS_ERR(btfm_slim_class)) {
		ret = PTR_ERR(btfm_slim_class);
		goto unregister;
	}
	chardev = device_create(btfm_slim_class, NULL, priv->devt,
				NULL, "btfmslim");
	if (IS_ERR(chardev)) {
		ret = PTR_ERR(chardev);
		goto destroy_class;
	}
	ret = btfm_slim_register_hw_ep(priv);
	if (ret)
		goto destroy_device;
	priv->endpoint_registered = true;
	/* Native startup resolves PGD/IFD addresses after Bluetooth is powered. */
	slim->is_laddr_valid = true;
	mutex_lock(&btfm_slim_lifecycle_lock);
	mutex_lock(&priv->io_lock);
	priv->stopping = false;
	mutex_unlock(&priv->io_lock);
	mutex_unlock(&btfm_slim_lifecycle_lock);
	return 0;
destroy_device:
	device_destroy(btfm_slim_class, priv->devt);
destroy_class:
	class_destroy(btfm_slim_class);
unregister:
	cdev_del(priv->cdev);
unregister_region:
	unregister_chrdev_region(priv->devt, 1);
clear:
	mutex_lock(&btfm_slim_lifecycle_lock);
	btfm_slim_drv_data = NULL;
	dev_set_drvdata(&slim->dev, NULL);
	mutex_unlock(&btfm_slim_lifecycle_lock);
free:
	kref_put(&priv->ref, btfm_slim_free);
	return ret;
}

static void btfm_slim_remove(struct slim_device *slim)
{
	struct btfmslim *priv = dev_get_drvdata(&slim->dev);
	struct btfmslim_ch *tables[] = { priv->rx_chs, priv->tx_chs };
	unsigned int counts[] = { priv->num_rx_port, priv->num_tx_port };
	int table, i, ret;

	mutex_lock(&btfm_slim_lifecycle_lock);
	mutex_lock(&priv->io_lock);
	priv->stopping = true;
	mutex_unlock(&priv->io_lock);
	mutex_unlock(&btfm_slim_lifecycle_lock);
	device_destroy(btfm_slim_class, priv->devt);
	cdev_del(priv->cdev);	unregister_chrdev_region(priv->devt, 1);
	if (priv->endpoint_registered)
		btfm_slim_unregister_hwep();
	for (table = 0; table < ARRAY_SIZE(tables); table++) {
		for (i = 0; i < counts[table]; i++) {
			struct btfmslim_ch *ch = &tables[table][i];

			if (!ch->dai.sruntime)
				continue;
			ret = btfm_slim_disable_ch(priv, ch, ch->dai.rxport,
						   ch->dai.channels);
			if (ret) {
				BTFMSLIM_ERR("channel %d teardown failed: %d", ch->id, ret);
				/* Controller teardown must quiesce outstanding bus traffic. */
				slim_stream_free(ch->dai.sruntime);
				ch->dai.sruntime = NULL;
				kfree(ch->dai.sconfig.chs);
				ch->dai.sconfig.chs = NULL;
			}
		}
	}
	mutex_lock(&btfm_slim_lifecycle_lock);
	class_destroy(btfm_slim_class);
	btfm_slim_drv_data = NULL;
	btfm_num_ports_open = 0;
	dev_set_drvdata(&slim->dev, NULL);
	mutex_unlock(&btfm_slim_lifecycle_lock);
	kref_put(&priv->ref, btfm_slim_free);
}

static const struct slim_device_id btfm_slim_id[] = {
	{
	.manf_id = SLIM_MANF_ID_QCOM,
	.prod_code = SLIM_PROD_CODE,
	.dev_index = 0x1,
	.instance = 0x0,
	},
	{
	.manf_id = SLIM_MANF_ID_QCOM,
	.prod_code = 0x220,
	.dev_index = 0x1,
	.instance = 0x0,
	},
	{ }
};

MODULE_DEVICE_TABLE(slim, btfm_slim_id);

static struct slim_driver btfm_slim_driver = {
	.driver = {
		.name = "btfmslim-driver",
		.suppress_bind_attrs = true,
		.owner = THIS_MODULE,
	},
	.probe = btfm_slim_probe,
	.device_status = btfm_slim_status,
	.remove = btfm_slim_remove,
	.id_table = btfm_slim_id
};

module_slim_driver(btfm_slim_driver);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("BTFM Slimbus Slave driver");
