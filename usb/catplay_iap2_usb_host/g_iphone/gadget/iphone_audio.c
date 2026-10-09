// SPDX-License-Identifier: GPL-2.0
/* Minimal iPhone UAC1 AudioStreaming emulator: silence over ISO IN. */
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/usb/composite.h>
#include <linux/usb/audio.h>
#include <linux/unaligned.h>
#include "iphone_audio.h"
#include "iphone_endpoint.h"

#define AUDIO_TX_REQUESTS 8
#define AUDIO_MAX_PACKET 192

struct iphone_audio;
struct audio_tx {
	struct usb_request *req;
	struct iphone_audio *audio;
};
struct iphone_audio {
	struct usb_function *func;
	struct usb_ep *ep;
	struct usb_request *ctrl;
	struct audio_tx tx[AUDIO_TX_REQUESTS];
	spinlock_t lock;
	wait_queue_head_t ctrl_idle;
	u32 rate;
	u32 remainder;
	bool streaming;
	bool ctrl_pending;
	bool ctrl_set_cur;
};

static bool rate_supported(u32 hz)
{
	switch (hz) {
	case 8000: case 11025: case 12000: case 16000: case 22050:
	case 24000: case 32000: case 44100: case 48000:
		return true;
	default:
		return false;
	}
}

/* One high-speed ISO service interval is 1 ms (bInterval=4). */
static unsigned int next_len(struct iphone_audio *a)
{
	unsigned int frames;
	a->remainder += a->rate;
	frames = a->remainder / 1000;
	a->remainder %= 1000;
	return frames * 4; /* S16_LE stereo */
}

static void tx_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct audio_tx *tx = req->context;
	struct iphone_audio *a = tx->audio;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&a->lock, flags);
	if (!a->streaming) {
		spin_unlock_irqrestore(&a->lock, flags);
		return;
	}
	if (req->status) {
		spin_unlock_irqrestore(&a->lock, flags);
		pr_warn_ratelimited("iPhone audio: ISO complete status=%d", req->status);
		return;
	}
	req->length = next_len(a);
	ret = usb_ep_queue(ep, req, GFP_ATOMIC);
	spin_unlock_irqrestore(&a->lock, flags);
	if (ret)
		pr_warn_ratelimited("iPhone audio: ISO queue failed=%d", ret);
}

static void ctrl_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct iphone_audio *a = req->context;
	unsigned long flags;
	u32 rate;

	spin_lock_irqsave(&a->lock, flags);
	if (a->ctrl_set_cur && !req->status && req->actual == 3) {
		rate = get_unaligned_le24(req->buf);
		if (rate_supported(rate)) {
			a->rate = rate;
			a->remainder = 0;
			pr_info("iPhone audio: sample rate %u Hz\n", rate);
		} else {
			pr_warn("iPhone audio: unsupported rate %u Hz\n", rate);
		}
	}
	a->ctrl_pending = false;
	a->ctrl_set_cur = false;
	spin_unlock_irqrestore(&a->lock, flags);
	wake_up_all(&a->ctrl_idle);
}

struct iphone_audio *iphone_audio_bind(struct usb_function *func,
				      const struct usb_endpoint_descriptor *desc)
{
	struct usb_composite_dev *cdev = func->config->cdev;
	struct iphone_audio *a;
	int i;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return ERR_PTR(-ENOMEM);
	a->func = func;
	a->rate = 48000;
	spin_lock_init(&a->lock);
	init_waitqueue_head(&a->ctrl_idle);
	a->ep = iphone_claim_fixed_endpoint(cdev->gadget, desc);
	if (!a->ep)
		goto fail;
	a->ctrl = usb_ep_alloc_request(cdev->gadget->ep0, GFP_KERNEL);
	if (!a->ctrl)
		goto fail;
	a->ctrl->buf = kzalloc(3, GFP_KERNEL);
	if (!a->ctrl->buf)
		goto fail;
	a->ctrl->context = a;
	a->ctrl->complete = ctrl_complete;
	for (i = 0; i < AUDIO_TX_REQUESTS; ++i) {
		a->tx[i].audio = a;
		a->tx[i].req = usb_ep_alloc_request(a->ep, GFP_KERNEL);
		if (!a->tx[i].req)
			goto fail;
		a->tx[i].req->buf = kzalloc(AUDIO_MAX_PACKET, GFP_KERNEL);
		if (!a->tx[i].req->buf)
			goto fail;
		a->tx[i].req->context = &a->tx[i];
		a->tx[i].req->complete = tx_complete;
	}
	return a;
fail:
	iphone_audio_unbind(a);
	return ERR_PTR(-ENOMEM);
}

void iphone_audio_disable(struct iphone_audio *a)
{
	unsigned long flags;
	bool active;
	if (!a)
		return;
	spin_lock_irqsave(&a->lock, flags);
	active = a->streaming;
	a->streaming = false;
	spin_unlock_irqrestore(&a->lock, flags);
	if (active)
		usb_ep_disable(a->ep);
}

int iphone_audio_set_alt(struct iphone_audio *a, unsigned int alt)
{
	struct usb_gadget *gadget;
	unsigned long flags;
	int i, ret = 0;
	if (!a)
		return -ENODEV;
	if (alt > 1)
		return -EINVAL;
	iphone_audio_disable(a);
	if (!alt)
		return 0;
	gadget = a->func->config->cdev->gadget;
	ret = config_ep_by_speed_and_alt(gadget, a->func, a->ep, 1);
	if (ret)
		return ret;
	ret = usb_ep_enable(a->ep);
	if (ret)
		return ret;
	spin_lock_irqsave(&a->lock, flags);
	a->streaming = true;
	a->remainder = 0;
	for (i = 0; i < AUDIO_TX_REQUESTS; ++i) {
		struct usb_request *req = a->tx[i].req;
		req->length = next_len(a);
		req->zero = 0;
		ret = usb_ep_queue(a->ep, req, GFP_ATOMIC);
		if (ret)
			break;
	}
	if (ret)
		a->streaming = false;
	spin_unlock_irqrestore(&a->lock, flags);
	if (ret) {
		usb_ep_disable(a->ep);
		return ret;
	}
	pr_info("iPhone audio: ISO IN started at %u Hz\n", READ_ONCE(a->rate));
	return 0;
}

int iphone_audio_setup(struct iphone_audio *a, const struct usb_ctrlrequest *ctrl)
{
	struct usb_composite_dev *cdev;
	struct usb_request *req;
	unsigned long flags;
	bool set_cur;
	int ret;
	u32 hz;
	if (!a)
		return -ENODEV;
	if (le16_to_cpu(ctrl->wIndex) != 0x0081 ||
	    le16_to_cpu(ctrl->wValue) != 0x0100 ||
	    le16_to_cpu(ctrl->wLength) != 3) {
        pr_info("iPhone: unknown audio request");
		return -EOPNOTSUPP;
    }
	if (ctrl->bRequestType == 0x22 && ctrl->bRequest == 0x01) {
		pr_info("iPhone: AUDIO_REQ_SET_CUR sampling frequency");
		set_cur = true;
	} else if (ctrl->bRequestType == 0xa2 && ctrl->bRequest == 0x81) {
		pr_info("iPhone: AUDIO_REQ_GET_CUR sampling frequency");
		set_cur = false;
	} else {
        pr_info("iPhone: unknown audio request #2");
		return -EOPNOTSUPP;
	}
	cdev = a->func->config->cdev;
	req = a->ctrl;
	spin_lock_irqsave(&a->lock, flags);
	if (a->ctrl_pending) {
		spin_unlock_irqrestore(&a->lock, flags);
		return -EBUSY;
	}
	a->ctrl_pending = true;
	a->ctrl_set_cur = set_cur;
	hz = a->rate;
	req->length = 3;
	req->zero = 0;
	if (!set_cur) {
		u8 *b = req->buf;
		b[0] = hz;
		b[1] = hz >> 8;
		b[2] = hz >> 16;
	}
	ret = usb_ep_queue(cdev->gadget->ep0, req, GFP_ATOMIC);
	if (ret) {
		a->ctrl_pending = false;
		a->ctrl_set_cur = false;
	}
	spin_unlock_irqrestore(&a->lock, flags);
	return ret;
}

void iphone_audio_unbind(struct iphone_audio *a)
{
	struct usb_composite_dev *cdev;
	int i;
	if (!a)
		return;
	iphone_audio_disable(a);
	cdev = a->func->config->cdev;
	if (a->ctrl && READ_ONCE(a->ctrl_pending)) {
		usb_ep_dequeue(cdev->gadget->ep0, a->ctrl);
		wait_event(a->ctrl_idle, !READ_ONCE(a->ctrl_pending));
	}
	for (i = 0; i < AUDIO_TX_REQUESTS; ++i) {
		if (!a->tx[i].req)
			continue;
		kfree(a->tx[i].req->buf);
		usb_ep_free_request(a->ep, a->tx[i].req);
	}
	if (a->ctrl) {
		kfree(a->ctrl->buf);
		usb_ep_free_request(cdev->gadget->ep0, a->ctrl);
	}
	if (a->ep)
		usb_ep_autoconfig_release(a->ep);
	kfree(a);
}
