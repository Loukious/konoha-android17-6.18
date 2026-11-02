#ifndef _MI_DISP_INTERNALS_H_
#define _MI_DISP_INTERNALS_H_

#include <linux/irq.h>
#include <linux/irqdesc.h>

#ifndef istate
#define istate core_internal_state__do_not_mess_with_it
#endif

#ifndef IRQS_PENDING
#define IRQS_PENDING 0x00000200
#endif

static inline bool irq_settings_is_level(struct irq_desc *desc)
{
	return desc->status_use_accessors & IRQ_LEVEL;
}

#endif /* _MI_DISP_INTERNALS_H_ */
