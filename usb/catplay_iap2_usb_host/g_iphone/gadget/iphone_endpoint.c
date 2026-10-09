// SPDX-License-Identifier: GPL-2.0
#include <linux/ctype.h>
#include <linux/usb/composite.h>
#include "iphone_endpoint.h"

/* The iPhone descriptor fixes endpoint numbers. A UDC with numbered
 * endpoints must match that number; configurable endpoints may adopt it.
 */
struct usb_ep *iphone_claim_fixed_endpoint(
	struct usb_gadget *gadget,
	const struct usb_endpoint_descriptor *desc)
{
	struct usb_ep *ep;

	list_for_each_entry(ep, &gadget->ep_list, ep_list) {
		struct usb_endpoint_descriptor candidate = *desc;

		if (strlen(ep->name) > 2 &&
		    ep->name[0] == 'e' && ep->name[1] == 'p' &&
		    isdigit(ep->name[2]) &&
		    simple_strtoul(ep->name + 2, NULL, 10) !=
			usb_endpoint_num(desc))
			continue;
		if (!usb_gadget_ep_match_desc(gadget, ep, &candidate, NULL))
			continue;
		ep->address = desc->bEndpointAddress;
		ep->desc = NULL;
		ep->comp_desc = NULL;
		ep->claimed = true;
		return ep;
	}
	return NULL;
}
