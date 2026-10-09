/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IPHONE_ENDPOINT_H
#define IPHONE_ENDPOINT_H

struct usb_ep;
struct usb_gadget;
struct usb_endpoint_descriptor;

struct usb_ep *iphone_claim_fixed_endpoint(
	struct usb_gadget *gadget,
	const struct usb_endpoint_descriptor *desc);

#endif
