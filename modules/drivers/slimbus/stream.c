// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018, Linaro Limited
// Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/slimbus.h>
#include <uapi/sound/asound.h>
#include "slimbus.h"

/**
 * struct segdist_code - Segment Distributions code from
 *	Table 20 of SLIMbus Specs Version 2.0
 *
 * @ratem: Channel Rate Multipler(Segments per Superframe)
 * @seg_interval: Number of slots between the first Slot of Segment
 *		and the first slot of the next  consecutive Segment.
 * @segdist_code: Segment Distribution Code SD[11:0]
 * @seg_offset_mask: Segment offset mask in SD[11:0]
 * @segdist_codes: List of all possible Segmet Distribution codes.
 */
static const struct segdist_code {
	int ratem;
	int seg_interval;
	int segdist_code;
	u32 seg_offset_mask;

} segdist_codes[] = {
	{1,	1536,	0x200,	 0xdff},
	{2,	768,	0x100,	 0xcff},
	{4,	384,	0x080,	 0xc7f},
	{8,	192,	0x040,	 0xc3f},
	{16,	96,	0x020,	 0xc1f},
	{32,	48,	0x010,	 0xc0f},
	{64,	24,	0x008,	 0xc07},
	{128,	12,	0x004,	 0xc03},
	{256,	6,	0x002,	 0xc01},
	{512,	3,	0x001,	 0xc00},
	{3,	512,	0xe00,	 0x1ff},
	{6,	256,	0xd00,	 0x0ff},
	{12,	128,	0xc80,	 0x07f},
	{24,	64,	0xc40,	 0x03f},
	{48,	32,	0xc20,	 0x01f},
	{96,	16,	0xc10,	 0x00f},
	{192,	8,	0xc08,	 0x007},
	{364,	4,	0xc04,	 0x003},
	{768,	2,	0xc02,	 0x001},
};

/**
 * slim_stream_allocate() - Allocate a new SLIMbus Stream
 * @dev:Slim device to be associated with
 * @name: name of the stream
 *
 * This is very first call for SLIMbus streaming, this API will allocate
 * a new SLIMbus stream and return a valid stream runtime pointer for client
 * to use it in subsequent stream apis. state of stream is set to ALLOCATED
 *
 * Return: valid pointer on success and error code on failure.
 * From ASoC DPCM framework, this state is linked to startup() operation.
 */
struct slim_stream_runtime *slim_stream_allocate(struct slim_device *dev,
						 const char *name)
{
	struct slim_stream_runtime *rt;

	if (!dev || !dev->ctrl || !name)
		return ERR_PTR(-EINVAL);
	if (READ_ONCE(dev->ctrl->removing))
		return ERR_PTR(-ESHUTDOWN);
	rt = kzalloc(sizeof(*rt), GFP_KERNEL);
	if (!rt)
		return ERR_PTR(-ENOMEM);

	rt->name = kasprintf(GFP_KERNEL, "slim-%s", name);
	if (!rt->name) {
		kfree(rt);
		return ERR_PTR(-ENOMEM);
	}

	get_device(&dev->dev);
	rt->dev = dev;
	spin_lock(&dev->stream_list_lock);
	list_add_tail(&rt->node, &dev->stream_list);
	spin_unlock(&dev->stream_list_lock);

	return rt;
}
EXPORT_SYMBOL_GPL(slim_stream_allocate);

static int slim_connect_port_channel(struct slim_stream_runtime *stream,
				     struct slim_port *port)
{
	struct slim_device *sdev = stream->dev;
	int ret;
	u8 wbuf[2];
	struct slim_val_inf msg = {0, 2, NULL, wbuf, NULL};
	u8 mc = SLIM_MSG_MC_CONNECT_SOURCE;
	DEFINE_SLIM_LDEST_TXN(txn, mc, 6, stream->dev->laddr, &msg);

	if (port->direction == SLIM_PORT_SINK)
		txn.mc = SLIM_MSG_MC_CONNECT_SINK;

	wbuf[0] = port->id;
	wbuf[1] = port->ch.id;

	ret = slim_do_transfer(sdev->ctrl, &txn);
	if (!ret) {
		port->ch.state = SLIM_CH_STATE_ASSOCIATED;
		port->state = SLIM_PORT_UNCONFIGURED;
	}
	return ret;
}

static int slim_disconnect_port(struct slim_stream_runtime *stream,
				struct slim_port *port)
{
	struct slim_device *sdev = stream->dev;
	int ret;
	u8 wbuf[1];
	struct slim_val_inf msg = {0, 1, NULL, wbuf, NULL};
	u8 mc = SLIM_MSG_MC_DISCONNECT_PORT;
	DEFINE_SLIM_LDEST_TXN(txn, mc, 5, stream->dev->laddr, &msg);

	if (port->state == SLIM_PORT_DISCONNECTED)
		return 0;
	wbuf[0] = port->id;

	ret = slim_do_transfer(sdev->ctrl, &txn);
	if (!ret) {
		port->ch.state = SLIM_CH_STATE_DISCONNECTED;
		port->state = SLIM_PORT_DISCONNECTED;
	}
	return ret;
}

static int slim_deactivate_remove_channel(struct slim_stream_runtime *stream,
					  struct slim_port *port)
{
	struct slim_device *sdev = stream->dev;
	u8 wbuf[1];
	struct slim_val_inf msg = {0, 1, NULL, wbuf, NULL};
	u8 mc = SLIM_MSG_MC_NEXT_DEACTIVATE_CHANNEL;
	DEFINE_SLIM_LDEST_TXN(txn, mc, 5, stream->dev->laddr, &msg);
	int ret;

	wbuf[0] = port->ch.id;
	ret = slim_do_transfer(sdev->ctrl, &txn);
	if (ret)
		return ret;

	txn.mc = SLIM_MSG_MC_NEXT_REMOVE_CHANNEL;

	ret = slim_do_transfer(sdev->ctrl, &txn);
	if (!ret)
		port->ch.state = SLIM_CH_STATE_REMOVED;
	return ret;
}

static int slim_get_prate_code(int rate)
{
	int ratem, ratefam, pr, exp = 0;
	bool done = false, exact = true;

	if (rate < SLIM_BASE_FREQ_4)
		return -EINVAL;
	ratem = ((rate == SLIM_FREQ_441) || (rate == SLIM_FREQ_882)) ?
			(rate/SLIM_BASE_FREQ_11) : (rate/SLIM_BASE_FREQ_4);

	ratefam = ((rate == SLIM_FREQ_441) || (rate == SLIM_FREQ_882)) ?
			2 : 1;

	while (!done) {
		while ((ratem & 0x1) != 0x1) {
			ratem >>= 1;
			exp++;
		}
		if (ratem > 3) {
			ratem++;
			exact = false;
		} else {
			done = true;
		}
	}

	if (ratefam == 1) {
		if (ratem == 1) {
			pr = 0x10;
		} else {
			pr = 0;
			exp++;
		}
	} else {
		pr = 8;
		exp++;
	}

	if (exp <= 7) {
		pr |= exp;
		if (exact)
			pr |= 0x80;
	} else {
		pr = 0;
	}

	return pr;
}

/**
 * slim_stream_prepare() - Prepare a SLIMbus Stream
 *
 * @rt: instance of slim stream runtime to configure
 * @cfg: new configuration for the stream
 *
 * This API will configure SLIMbus stream with config parameters from cfg.
 * return zero on success and error code on failure. From ASoC DPCM framework,
 * this state is linked to hw_params() operation.
 */
static int slim_stream_prepare_locked(struct slim_stream_runtime *rt,
				      struct slim_stream_config *cfg)
{
	struct slim_controller *ctrl;
	struct slim_port *port;
	int num_ports, i, port_id, prrate, ret;

	if (!rt || !cfg) {
		pr_err("%s: Stream or cfg is NULL, Check from client side\n", __func__);
		return -EINVAL;
	}

	ctrl = rt->dev->ctrl;
	if (rt->ports) {
		dev_err(&rt->dev->dev, "Stream already Prepared\n");
		return -EINVAL;
	}

	if (!ctrl->a_framer || ctrl->a_framer->superfreq <= 0 ||
	    !cfg->port_mask || cfg->port_mask & ~GENMASK(31, 0) ||
	    !cfg->chs || !cfg->ch_count || cfg->ch_count > SLIM_DEVICE_MAX_PORTS ||
	    !cfg->bps || cfg->bps > 32 || cfg->bps % SLIM_SLOT_LEN_BITS ||
	    cfg->direction < SNDRV_PCM_STREAM_PLAYBACK ||
	    cfg->direction > SNDRV_PCM_STREAM_CAPTURE)
		return -EINVAL;
	num_ports = hweight32(cfg->port_mask);
	if (cfg->ch_count != num_ports)
		return -EINVAL;
	for (i = 0; i < num_ports; i++)
		if (cfg->chs[i] >= SLIM_DEVICE_MAX_CHANNELS)
			return -EINVAL;
	prrate = slim_get_prate_code(cfg->rate);
	if (prrate < 0)
		return prrate;
	rt->ports = kcalloc(num_ports, sizeof(*port), GFP_KERNEL);
	if (!rt->ports)
		return -ENOMEM;
	rt->num_ports = num_ports;
	rt->rate = cfg->rate;
	rt->bps = cfg->bps;
	rt->direction = cfg->direction;

	if (cfg->rate % ctrl->a_framer->superfreq) {
		/*
		 * data rate not exactly multiple of super frame,
		 * use PUSH/PULL protocol
		 */
		if (cfg->direction == SNDRV_PCM_STREAM_PLAYBACK)
			rt->prot = SLIM_PROTO_PUSH;
		else
			rt->prot = SLIM_PROTO_PULL;
	} else {
		rt->prot = SLIM_PROTO_ISO;
	}

	rt->ratem = cfg->rate/ctrl->a_framer->superfreq;

	i = 0;
	for_each_set_bit(port_id, &cfg->port_mask, SLIM_DEVICE_MAX_PORTS) {
		port = &rt->ports[i];
		port->state = SLIM_PORT_DISCONNECTED;
		port->id = port_id;
		port->ch.prrate = prrate;
		port->ch.id = cfg->chs[i];
		port->ch.data_fmt = SLIM_CH_DATA_FMT_NOT_DEFINED;
		port->ch.aux_fmt = SLIM_CH_AUX_FMT_NOT_APPLICABLE;
		port->ch.state = SLIM_CH_STATE_ALLOCATED;

		if (cfg->direction == SNDRV_PCM_STREAM_PLAYBACK)
			port->direction = SLIM_PORT_SINK;
		else
			port->direction = SLIM_PORT_SOURCE;

		ret = slim_connect_port_channel(rt, port);
		if (ret)
			goto disconnect;
		i++;
	}

	return 0;
disconnect:
	/* Preserve any connection whose rollback failed so teardown can retry. */
	for (i = 0; i < rt->num_ports; i++)
		slim_disconnect_port(rt, &rt->ports[i]);
	for (i = 0; i < rt->num_ports; i++)
		if (rt->ports[i].state != SLIM_PORT_DISCONNECTED)
			return ret;
	kfree(rt->ports);
	rt->ports = NULL;
	rt->num_ports = 0;
	return ret;
}

int slim_stream_prepare(struct slim_stream_runtime *stream, struct slim_stream_config *cfg)
{
	struct slim_controller *ctrl;
	struct slim_device *dev;
	int ret;

	if (!stream || !stream->dev || !stream->dev->ctrl)
		return -EINVAL;
	dev = stream->dev;
	ctrl = dev->ctrl;
	mutex_lock(&ctrl->stream_lock);
	if (READ_ONCE(ctrl->removing))
		ret = -ESHUTDOWN;
	else
		ret = slim_stream_prepare_locked(stream, cfg);
	mutex_unlock(&ctrl->stream_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(slim_stream_prepare);

static int slim_define_channel_content(struct slim_stream_runtime *stream,
				       struct slim_port *port)
{
	struct slim_device *sdev = stream->dev;
	int ret;
	u8 wbuf[4];
	struct slim_val_inf msg = {0, 4, NULL, wbuf, NULL};
	u8 mc = SLIM_MSG_MC_NEXT_DEFINE_CONTENT;
	DEFINE_SLIM_LDEST_TXN(txn, mc, 8, stream->dev->laddr, &msg);

	wbuf[0] = port->ch.id;
	wbuf[1] = port->ch.prrate;

	/* Frequency Locked for ISO Protocol */
	if (stream->prot != SLIM_PROTO_ISO)
		wbuf[1] |= SLIM_CHANNEL_CONTENT_FL;

	wbuf[2] = port->ch.data_fmt | (port->ch.aux_fmt << 4);
	wbuf[3] = stream->bps/SLIM_SLOT_LEN_BITS;

	ret = slim_do_transfer(sdev->ctrl, &txn);
	if (!ret)
		port->ch.state = SLIM_CH_STATE_CONTENT_DEFINED;
	return ret;
}

static int slim_get_segdist_code(int ratem)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(segdist_codes); i++) {
		if (segdist_codes[i].ratem == ratem)
			return segdist_codes[i].segdist_code;
	}

	return -EINVAL;
}

static int slim_define_channel(struct slim_stream_runtime *stream,
				       struct slim_port *port)
{
	struct slim_device *sdev = stream->dev;
	int ret;
	u8 wbuf[4];
	struct slim_val_inf msg = {0, 4, NULL, wbuf, NULL};
	u8 mc = SLIM_MSG_MC_NEXT_DEFINE_CHANNEL;
	DEFINE_SLIM_LDEST_TXN(txn, mc, 8, stream->dev->laddr, &msg);

	ret = slim_get_segdist_code(stream->ratem);
	if (ret < 0)
		return ret;
	port->ch.seg_dist = ret;

	wbuf[0] = port->ch.id;
	wbuf[1] = port->ch.seg_dist & 0xFF;
	wbuf[2] = (stream->prot << 4) | ((port->ch.seg_dist & 0xF00) >> 8);
	if (stream->prot == SLIM_PROTO_ISO)
		wbuf[3] = stream->bps/SLIM_SLOT_LEN_BITS;
	else
		wbuf[3] = stream->bps/SLIM_SLOT_LEN_BITS + 1;

	ret = slim_do_transfer(sdev->ctrl, &txn);
	if (!ret)
		port->ch.state = SLIM_CH_STATE_DEFINED;
	return ret;
}

static int slim_activate_channel(struct slim_stream_runtime *stream,
				 struct slim_port *port)
{
	struct slim_device *sdev = stream->dev;
	int ret;
	u8 wbuf[1];
	struct slim_val_inf msg = {0, 1, NULL, wbuf, NULL};
	u8 mc = SLIM_MSG_MC_NEXT_ACTIVATE_CHANNEL;
	DEFINE_SLIM_LDEST_TXN(txn, mc, 5, stream->dev->laddr, &msg);

	txn.msg->num_bytes = 1;
	txn.msg->wbuf = wbuf;
	wbuf[0] = port->ch.id;

	ret = slim_do_transfer(sdev->ctrl, &txn);
	if (!ret)
		port->ch.state = SLIM_CH_STATE_ACTIVE;
	return ret;
}

/**
 * slim_stream_enable() - Enable a prepared SLIMbus Stream
 *
 * @stream: instance of slim stream runtime to enable
 *
 * This API will enable all the ports and channels associated with
 * SLIMbus stream
 *
 * Return: zero on success and error code on failure. From ASoC DPCM framework,
 * this state is linked to trigger() start operation.
 */
static int slim_stream_enable_locked(struct slim_stream_runtime *stream)
{
	DEFINE_SLIM_BCAST_TXN(txn, SLIM_MSG_MC_BEGIN_RECONFIGURATION,
				3, SLIM_LA_MANAGER, NULL);
	struct slim_controller *ctrl;
	int ret, i;

	if (!stream) {
		pr_err("%s: Stream is NULL, Check from client side\n", __func__);
		return -EINVAL;
	}

	if (!stream->ports || !stream->num_ports)
		return -EINVAL;
	ctrl = stream->dev->ctrl;
	if (ctrl->enable_stream) {
		ret = ctrl->enable_stream(stream);
		if (ret) {
			return ret;
		}

		for (i = 0; i < stream->num_ports; i++)
			stream->ports[i].ch.state = SLIM_CH_STATE_ACTIVE;

		return ret;
	}

	ret = slim_do_transfer(ctrl, &txn);
	if (ret)
		return ret;

	/* define channels first before activating them */
	for (i = 0; i < stream->num_ports; i++) {
		struct slim_port *port = &stream->ports[i];

		ret = slim_define_channel(stream, port);
		if (ret)
			return ret;
		ret = slim_define_channel_content(stream, port);
		if (ret)
			return ret;
	}

	for (i = 0; i < stream->num_ports; i++) {
		struct slim_port *port = &stream->ports[i];

		ret = slim_activate_channel(stream, port);
		if (ret)
			return ret;
		port->state = SLIM_PORT_CONFIGURED;
	}
	txn.mc = SLIM_MSG_MC_RECONFIGURE_NOW;

	return slim_do_transfer(ctrl, &txn);
}

int slim_stream_enable(struct slim_stream_runtime *stream)
{
	struct slim_controller *ctrl;
	struct slim_device *dev;
	int ret;

	if (!stream || !stream->dev || !stream->dev->ctrl)
		return -EINVAL;
	dev = stream->dev;
	ctrl = dev->ctrl;
	mutex_lock(&ctrl->stream_lock);
	if (READ_ONCE(ctrl->removing))
		ret = -ESHUTDOWN;
	else
		ret = slim_stream_enable_locked(stream);
	mutex_unlock(&ctrl->stream_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(slim_stream_enable);

/**
 * slim_stream_disable() - Disable a SLIMbus Stream
 *
 * @stream: instance of slim stream runtime to disable
 *
 * This API will disable all the ports and channels associated with
 * SLIMbus stream
 *
 * Return: zero on success and error code on failure. From ASoC DPCM framework,
 * this state is linked to trigger() pause operation.
 */
static int slim_stream_disable_locked(struct slim_stream_runtime *stream)
{
	DEFINE_SLIM_BCAST_TXN(txn, SLIM_MSG_MC_BEGIN_RECONFIGURATION,
				3, SLIM_LA_MANAGER, NULL);
	struct slim_controller *ctrl;
	int ret, i;

	if (!stream) {
		pr_err("%s: Stream is NULL, Check from client side\n", __func__);
		return -EINVAL;
	}

	if (!stream->ports || !stream->num_ports) {
		pr_err("%s: Stream port is NULL %d\n", __func__, stream->num_ports);
		return -EINVAL;
	}

	ctrl = stream->dev->ctrl;
	if (ctrl->disable_stream) {
		ret = ctrl->disable_stream(stream);
		if (ret) {
			return ret;
		}
	}

	ret = slim_do_transfer(ctrl, &txn);
	if (ret)
		return ret;

	for (i = 0; i < stream->num_ports; i++) {
		ret = slim_deactivate_remove_channel(stream, &stream->ports[i]);
		if (ret)
			return ret;
	}

	txn.mc = SLIM_MSG_MC_RECONFIGURE_NOW;

	return slim_do_transfer(ctrl, &txn);
}

int slim_stream_disable(struct slim_stream_runtime *stream)
{
	struct slim_controller *ctrl;
	struct slim_device *dev;
	int ret;

	if (!stream || !stream->dev || !stream->dev->ctrl)
		return -EINVAL;
	dev = stream->dev;
	ctrl = dev->ctrl;
	mutex_lock(&ctrl->stream_lock);
	if (READ_ONCE(ctrl->removing))
		ret = -ESHUTDOWN;
	else
		ret = slim_stream_disable_locked(stream);
	mutex_unlock(&ctrl->stream_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(slim_stream_disable);

/**
 * slim_stream_unprepare() - Un-prepare a SLIMbus Stream
 *
 * @stream: instance of slim stream runtime to unprepare
 *
 * This API will un allocate all the ports and channels associated with
 * SLIMbus stream
 *
 * Return: zero on success and error code on failure. From ASoC DPCM framework,
 * this state is linked to trigger() stop operation.
 */
static int slim_stream_unprepare_locked(struct slim_stream_runtime *stream)
{
	int i, ret;

	if (!stream) {
		pr_err("%s: Stream is NULL, Check from client side\n", __func__);
		return -EINVAL;
	}

	if (!stream->ports || !stream->num_ports)
		return 0;

	for (i = 0; i < stream->num_ports; i++) {
		ret = slim_disconnect_port(stream, &stream->ports[i]);
		if (ret)
			return ret;
	}

	kfree(stream->ports);
	stream->ports = NULL;
	stream->num_ports = 0;

	return 0;
}

int slim_stream_unprepare(struct slim_stream_runtime *stream)
{
	struct slim_controller *ctrl;
	struct slim_device *dev;
	int ret;

	if (!stream || !stream->dev || !stream->dev->ctrl)
		return -EINVAL;
	dev = stream->dev;
	ctrl = dev->ctrl;
	mutex_lock(&ctrl->stream_lock);
	if (READ_ONCE(ctrl->removing))
		ret = -ESHUTDOWN;
	else
		ret = slim_stream_unprepare_locked(stream);
	mutex_unlock(&ctrl->stream_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(slim_stream_unprepare);

/**
 * slim_stream_unprepare_disconnect_port() - Un-prepare and disconnect
 * selected SLIMbus Stream ports
 *
 * @stream: instance of slim stream runtime to unprepare
 * @disconnect_ports: disconnect the ports of the stream
 * @is_session_completed: free the ports allocated to the stream
 *
 * This API will disconnect all the ports and un allocate all the ports and
 * channels associated with SLIMbus stream.
 *
 * Return: zero on success and error code on failure. From ASoC DPCM framework,
 * this state is linked to trigger() stop operation.
 */
static int slim_stream_unprepare_disconnect_port_locked(struct slim_stream_runtime *stream,
							bool disconnect_ports,
							bool is_session_completed)
{
	int i, ret;

	if (!stream) {
		pr_err("%s: Stream is NULL, Check from client side\n", __func__);
		return -EINVAL;
	}

	if (!stream->ports || !stream->num_ports)
		return 0;

	if (disconnect_ports) {
		for (i = 0; i < stream->num_ports; i++) {
			ret = slim_disconnect_port(stream, &stream->ports[i]);
			if (ret)
				return ret;
		}
	}

	if (is_session_completed) {
		for (i = 0; i < stream->num_ports; i++)
			if (stream->ports[i].state != SLIM_PORT_DISCONNECTED)
				return -EBUSY;
		kfree(stream->ports);
		stream->ports = NULL;
		stream->num_ports = 0;
	}

	return 0;
}

int slim_stream_unprepare_disconnect_port(struct slim_stream_runtime *stream,
					  bool disconnect_ports, bool is_session_completed)
{
	struct slim_controller *ctrl;
	struct slim_device *dev;
	int ret;

	if (!stream || !stream->dev || !stream->dev->ctrl)
		return -EINVAL;
	dev = stream->dev;
	ctrl = dev->ctrl;
	mutex_lock(&ctrl->stream_lock);
	if (READ_ONCE(ctrl->removing))
		ret = -ESHUTDOWN;
	else
		ret = slim_stream_unprepare_disconnect_port_locked(stream, disconnect_ports,
								   is_session_completed);
	mutex_unlock(&ctrl->stream_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(slim_stream_unprepare_disconnect_port);

/**
 * slim_stream_free() - Free a SLIMbus Stream
 *
 * @stream: instance of slim stream runtime to free
 *
 * This API will un allocate all the memory associated with
 * slim stream runtime, user is not allowed to make an dereference
 * to stream after this call.
 *
 * Return: zero on success and error code on failure. From ASoC DPCM framework,
 * this state is linked to shutdown() operation.
 */
static int slim_stream_free_locked(struct slim_stream_runtime *stream)
{
	struct slim_device *sdev;

	if (!stream) {
		pr_err("%s: Stream is NULL, Check from client side\n", __func__);
		return -EINVAL;
	}

	sdev = stream->dev;
	/* Final release also abandons state after a failed hardware cleanup. */
	kfree(stream->ports);
	spin_lock(&sdev->stream_list_lock);
	list_del(&stream->node);
	spin_unlock(&sdev->stream_list_lock);

	kfree(stream->name);
	kfree(stream);

	return 0;
}

int slim_stream_free(struct slim_stream_runtime *stream)
{
	struct slim_controller *ctrl;
	struct slim_device *dev;
	int ret;

	if (!stream || !stream->dev || !stream->dev->ctrl)
		return -EINVAL;
	dev = stream->dev;
	ctrl = dev->ctrl;
	mutex_lock(&ctrl->stream_lock);
	ret = slim_stream_free_locked(stream);
	mutex_unlock(&ctrl->stream_lock);
	if (!ret)
		put_device(&dev->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(slim_stream_free);
