/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#ifndef _TMECOM_H_
#define _TMECOM_H_

#define MBOX_MAX_MSG_LEN	1024

/*
 * respsize is the response buffer capacity on entry and payload length on success.
 * On -EMSGSIZE it reports the required length without writing any payload.
 * Requests are at most MBOX_MAX_MSG_LEN minus the 8-byte native header.
 * Responses must echo the native transaction ID. Late responses cannot satisfy
 * a subsequent request. A mailbox TX timeout leaves the packet stable and the
 * session unavailable (-EPIPE) until the channel has been shut down/drained.
 */
int tmecom_process_request(const void *reqbuf, size_t reqsize, void *respbuf,
		size_t *respsize);
#endif  /*_TMECOM_H_ */
