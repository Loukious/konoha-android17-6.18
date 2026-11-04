/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NETHUNTER_NATIVE_USB_HELPERS_H
#define NETHUNTER_NATIVE_USB_HELPERS_H
#include <linux/usb.h>

/* Exact forwarding semantics from pinned drivers/usb/core/message.c.
 * Keep the caller's interrupt pipe; only the unexported wrapper is local.
 */
static inline int nethunter_usb_interrupt_msg(struct usb_device *usb_dev,
        unsigned int pipe, void *data, int len, int *actual_length, int timeout)
{
    return usb_bulk_msg(usb_dev, pipe, data, len, actual_length, timeout);
}
#endif
