/* SPDX-License-Identifier: GPL-2.0 */
#ifndef IPHONE_AUDIO_H
#define IPHONE_AUDIO_H
#include <linux/types.h>
struct usb_function;
struct usb_ctrlrequest;
struct usb_endpoint_descriptor;
struct iphone_audio;
struct iphone_audio *iphone_audio_bind(struct usb_function *, const struct usb_endpoint_descriptor *);
int iphone_audio_set_alt(struct iphone_audio *, unsigned int alt);
int iphone_audio_setup(struct iphone_audio *, const struct usb_ctrlrequest *);
void iphone_audio_disable(struct iphone_audio *);
void iphone_audio_unbind(struct iphone_audio *);
#endif
