/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __QTI_PMIC_GLINK_H
#define __QTI_PMIC_GLINK_H

#include <linux/types.h>
#include <linux/err.h>
#include <linux/kconfig.h>

struct pmic_glink_client;
struct device;

enum pmic_glink_state {
	PMIC_GLINK_STATE_DOWN,
	PMIC_GLINK_STATE_UP,
};

/**
 * struct pmic_glink_client_data - pmic_glink client data
 * @name:	Client name
 * @id:		Unique id for client for communication
 * @priv:	private data for client
 * @msg_cb:	callback function for client to receive the messages that
 *		are intended to be delivered to it over PMIC Glink
 * Callbacks may sleep. Do not unregister a client from either callback;
 * unregister waits for pending callbacks. Serialize writes with unregister.
 *
 * @state_cb:	callback function to notify pmic glink state in the event of
 *		a subsystem restart (SSR) or a protection domain restart (PDR)
 */
struct pmic_glink_client_data {
	const char	*name;
	u32		id;
	void		*priv;
	int		(*msg_cb)(void *priv, void *data, size_t len);
	void		(*state_cb)(void *priv, enum pmic_glink_state state);
};

/**
 * struct pmic_glink_hdr - PMIC Glink message header
 * @owner:	message owner for a client
 * @type:	message type
 * @opcode:	message opcode
 */
struct pmic_glink_hdr {
	u32 owner;
	u32 type;
	u32 opcode;
};

#if IS_ENABLED(CONFIG_QTI_PMIC_GLINK)
struct pmic_glink_client *pmic_glink_register_client(struct device *dev,
			const struct pmic_glink_client_data *client_data);
int pmic_glink_unregister_client(struct pmic_glink_client *client);
int pmic_glink_write(struct pmic_glink_client *client, void *data,
			size_t len);
/* Replies have no native transaction ID. Call begin before a synchronous
 * request; end after a validated reply or a failed send that did not submit.
 * A timeout leaves the record pending. Client unregister preserves it until
 * its late reply is consumed or a drained RPMSG channel removal clears it.
 * Serialize request_begin/end/writes with unregister, as for ordinary writes.
 */
int pmic_glink_request_begin(struct pmic_glink_client *client, u32 opcode,
			    u32 type, size_t min_len, size_t max_len);
void pmic_glink_request_end(struct pmic_glink_client *client, u32 opcode);
/* Reject a matching envelope whose client-specific payload is invalid.
 * Keep its unanswered record across unregister. Call from the RX callback.
 */
void pmic_glink_request_reject(struct pmic_glink_client *client, u32 opcode);
/* Match an echoed u32 reply field before releasing an orphaned request. */
int pmic_glink_request_begin_keyed(struct pmic_glink_client *client, u32 opcode,
			    u32 type, size_t min_len, size_t max_len,
			    size_t key_offset, u32 key);
#else
static inline struct pmic_glink_client *pmic_glink_register_client(
			struct device *dev,
			const struct pmic_glink_client_data *client_data)
{
	return ERR_PTR(-ENODEV);
}

static inline int pmic_glink_unregister_client(struct pmic_glink_client *client)
{
	return -ENODEV;
}

static inline int pmic_glink_write(struct pmic_glink_client *client, void *data,
				size_t len)
{
	return -ENODEV;
}
static inline int pmic_glink_request_begin(struct pmic_glink_client *client,
		u32 opcode, u32 type, size_t min_len, size_t max_len)
{
	return -ENODEV;
}
static inline void pmic_glink_request_end(struct pmic_glink_client *client, u32 opcode)
{ }
static inline void pmic_glink_request_reject(struct pmic_glink_client *client, u32 opcode)
{ }
static inline int pmic_glink_request_begin_keyed(struct pmic_glink_client *client,
		u32 opcode, u32 type, size_t min_len, size_t max_len,
		size_t key_offset, u32 key)
{ return -ENODEV; }
#endif

#endif
