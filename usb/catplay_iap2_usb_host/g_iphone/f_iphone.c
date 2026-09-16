// SPDX-License-Identifier: GPL-2.0
#include "f_iphone.h"
#include "interfaces.h"
#include "hid.h"

#include <linux/ktime.h>
#include <linux/math64.h>

#define IPHONE_REQ_USBOOT 0x88
#define IPHONE_REQ_GADGET 0x99

static struct g_iphone *g_iphone_from_func(struct usb_function *f)
{
	struct f_iphone *iphone_func = container_of(f, struct f_iphone, func);
	return iphone_func->g;
}

static struct g_iphone *g_iphone_from_usb_config(struct usb_configuration *f)
{
	struct f_iphone_usb_config *iphone_func = container_of(f, struct f_iphone_usb_config, cfg);
	return iphone_func->g;
}

/* ----------------- Bind/Unbind ----------------- */
static int f_iphone_bind(struct usb_configuration *c, struct usb_function *f)
{
	int config = c->bConfigurationValue;
	pr_debug("iPhone: binding config %u", config);

	struct g_iphone *iphone_gadget = g_iphone_from_func(f);
	if (!iphone_gadget)
		return -ENODEV;

	if (config < 1 || config > 4)
	{
		pr_err("iPhone: no descriptors for config %u", config);
		return -EINVAL;
	}

	g_iphone_set_status(iphone_gadget, Bind);

	struct usb_descriptor_header **d;
	d = iphone_descs[config];
	int i = 0;
	for (; d && *d; d++)
	{
		pr_debug("  desc: type=0x%02x len=%u", (*d)->bDescriptorType, (*d)->bLength);
		if (i++ > 30)
			break;
	}

	int intfs = iphone_intf_counts[config];
	pr_debug("iPhone: adding %u intfs at config %u", intfs, config);

	for (int i = 0; i < intfs; i++)
	{
		int id = usb_interface_id(c, f);
		if (id < 0)
		{
			return id;
		}
	}

	return usb_assign_descriptors(f,
								  iphone_descs[config],
								  iphone_descs[config],
								  NULL, NULL);
}

static void f_iphone_unbind(struct usb_configuration *c, struct usb_function *f)
{
	pr_debug("iPhone: unbind()");
	usb_free_all_descriptors(f);

	struct f_iphone *iphone_func = container_of(f, struct f_iphone, func);
	kfree(iphone_func);
}

static int f_iphone_set_alt(struct usb_function *f, unsigned intf, unsigned alt)
{
	pr_info("iPhone: set_alt(intf=%u, alt=%u)", intf, alt);

	struct g_iphone *iphone_gadget = g_iphone_from_func(f);
	if (!iphone_gadget)
		return -ENODEV;

	g_iphone_set_status(iphone_gadget, Enabled);
	return 0;
}

static void f_iphone_disable(struct usb_function *f)
{
	pr_debug("iPhone: disable()");

	struct g_iphone *iphone_gadget = g_iphone_from_func(f);
	if (!iphone_gadget)
		return;

	g_iphone_set_status(iphone_gadget, Disabled);
}

static void f_iphone_suspend(struct usb_function *f)
{
	pr_debug("iPhone: suspend()");

	struct g_iphone *iphone_gadget = g_iphone_from_func(f);
	if (!iphone_gadget)
		return;

	g_iphone_set_status(iphone_gadget, Suspended);
}

static void f_iphone_resume(struct usb_function *f)
{
	pr_debug("iPhone: resume()");

	struct g_iphone *iphone_gadget = g_iphone_from_func(f);
	if (!iphone_gadget)
		return;

	g_iphone_set_status(iphone_gadget, Enabled);
}

static int ep0_send(struct usb_composite_dev *cdev, const void *src, u16 wLength, size_t real_len)
{
	size_t len;
	if (!cdev || !cdev->req || !cdev->gadget || !cdev->gadget->ep0 || !cdev->req->buf)
		return -ENODEV;

	len = min_t(size_t, real_len, wLength);

	memcpy(cdev->req->buf, src, len);
	cdev->req->length = len;
	cdev->req->zero = (len < wLength) ||
					  (len && (len % cdev->gadget->ep0->maxpacket) == 0);
	pr_info("iPhone: ep0_send -> len %zu", len);

	return usb_ep_queue(cdev->gadget->ep0, cdev->req, GFP_ATOMIC);
}
static int ep0_zlp(struct usb_composite_dev *cdev)
{
	cdev->req->length = 0;
	cdev->req->zero = 0;
	return usb_ep_queue(cdev->gadget->ep0, cdev->req, GFP_ATOMIC);
}

/*
 * Role switch (vendor request 0x51) handshake.
 *
 * 0x51 is a two-stage control transfer: SETUP, then a status-stage IN that
 * our zero-length packet answers. Only once the head unit has collected that
 * ZLP does it know we agreed to swap roles. Arm the RoleSwitch transition
 * from the ZLP's completion callback - the moment the host has acknowledged
 * us - and log how long that took, so a capture shows whether the ACK made
 * it out at all. (Measured on a NAC Wave 2: 16-74us.)
 *
 * There is one composite ep0 request and one gadget, so the pending state
 * is kept in file scope. If the host never runs the status stage and never
 * sends another SETUP either, the callback never fires and the role switch
 * does not happen - a head unit that does that would not have switched
 * roles anyway.
 */
static struct g_iphone *role_switch_ack_gadget;
static void (*role_switch_ack_orig_complete)(struct usb_ep *ep,
					     struct usb_request *req);
static u64 role_switch_ack_queued_ns;

static void f_iphone_role_switch_ack_complete(struct usb_ep *ep,
					      struct usb_request *req)
{
	struct g_iphone *iphone_gadget = role_switch_ack_gadget;
	u64 dt_us = div_u64(ktime_get_ns() - role_switch_ack_queued_ns, 1000);

	req->complete = role_switch_ack_orig_complete;
	if (role_switch_ack_orig_complete)
		role_switch_ack_orig_complete(ep, req);

	if (req->status)
		pr_warn("iPhone: role switch status stage ended with %d after %lluus - switching anyway\n",
			req->status, dt_us);
	else
		pr_info("iPhone: role switch acknowledged by host after %lluus\n",
			dt_us);

	if (iphone_gadget)
		g_iphone_set_status(iphone_gadget, RoleSwitch);
}

/* ----------------- Control requests ----------------- */
static int f_iphone_setup(struct usb_function *f,
						const struct usb_ctrlrequest *ctrl)
{

	u16 wIndex = le16_to_cpu(ctrl->wIndex);
	u16 wValue = le16_to_cpu(ctrl->wValue);
	u16 wLength = le16_to_cpu(ctrl->wLength);

	pr_info("iPhone: setup bmReq=0x%02x bReq=0x%02x wValue=0x%04x wIndex=0x%04x wLen=%u",
			ctrl->bRequestType, ctrl->bRequest, wValue, wIndex, wLength);

	struct usb_composite_dev *cdev = f->config->cdev;
	if (!cdev)
		return -ENODEV;

	struct g_iphone *iphone_gadget = g_iphone_from_func(f);
	if (!iphone_gadget)
		return -ENODEV;

	/* Magic HID reports */
	if ((ctrl->bRequestType == (USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE)) &&
		(ctrl->bRequest == USB_REQ_GET_DESCRIPTOR) &&
		(wIndex == 0x02 /* IPHONE_HID_INTF */))
	{
		u8 dtype = wValue >> 8;
		// u8 dindex = wValue & 0xff;
		switch (dtype)
		{
		case USB_DT_HID: /* 0x21 */
			/*if (dindex != 0) return -EOPNOTSUPP; */
			pr_info("iPhone: HID descriptor requested");
			return ep0_send(cdev, hid_2_2_bin, wLength, hid_2_2_bin_len);

		case USB_DT_REPORT: /* 0x22 */
			/*if (dindex != 0) return -EOPNOTSUPP; */
			pr_info("iPhone: HID report requested");
			return ep0_send(cdev, report_2_2_bin_1, wLength, report_2_2_bin_1_len);

		default:
			return -EOPNOTSUPP;
		}
	}

	/* Vendor-specific requests (device-to-host) */
	if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_VENDOR)
	{

		switch (ctrl->bRequest)
		{
		case 0x40: /* Power Capability */
		{
			if (!cdev->req || !cdev->gadget->ep0)
				return -ENODEV;

			pr_info("iPhone: Power Capability offered: %umA", wValue + 500);
			return ep0_zlp(cdev);
		}
		case 0x51: /* Role Switch */
		{
			int ret;

			pr_info("iPhone: Role Switch requested");
			if (!cdev->req || !cdev->gadget->ep0)
				return -ENODEV;

			iphone_gadget->role_switch_requested = true;

			/* The status transition happens once the host has our ZLP. */
			role_switch_ack_gadget = iphone_gadget;
			role_switch_ack_orig_complete = cdev->req->complete;
			role_switch_ack_queued_ns = ktime_get_ns();
			cdev->req->complete = f_iphone_role_switch_ack_complete;

			ret = ep0_zlp(cdev);
			if (ret) {
				cdev->req->complete = role_switch_ack_orig_complete;
				iphone_gadget->role_switch_requested = false;
				pr_warn("iPhone: role switch ZLP queue failed: %d\n", ret);
			}
			return ret;
		}

		case 0x53: /* Capabilities */
		{
			pr_info("iPhone: Capabilities requested");
			static const u8 caps[4] = {0x01, 0x00, 0x00, 0x00};
			return ep0_send(cdev, caps, wLength, sizeof(caps));
		}
		case IPHONE_REQ_USBOOT: /* Launch userspace OTA usboot flow */
		{
			int ret;

			if (!cdev->req || !cdev->gadget->ep0)
				return -ENODEV;

			ret = g_iphone_start_recovery(iphone_gadget);
			if (ret)
				pr_warn("iPhone: recovery request rejected: %d\n", ret);

			return ep0_zlp(cdev);
		}
		case IPHONE_REQ_GADGET: /* Launch userspace gadget flow */
		{
			int ret;

			if (!cdev->req || !cdev->gadget->ep0)
				return -ENODEV;

			ret = g_iphone_start_gadget(iphone_gadget);
			if (ret)
				pr_warn("iPhone: gadget request rejected: %d\n", ret);

			return ep0_zlp(cdev);
		}
		}
	}
	return -EOPNOTSUPP;
}

static bool f_iphone_req_match(struct usb_function *f,
							 const struct usb_ctrlrequest *ctrl,
							 bool config0)
{
	/* req_match is important to be able to capture this request before any interface(1-4) is selected
	   some headunits may select the interface before role switch, some may not */

	if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_VENDOR)
	{
		switch (ctrl->bRequest)
		{
		case 0x40: /* Power Capability */
		case 0x51: /* Role Switch */
		case 0x53: /* Capabilities */
		case IPHONE_REQ_USBOOT: /* Launch userspace OTA usboot flow */
		case IPHONE_REQ_GADGET: /* Launch userspace gadget flow */
			return true;
		}
	}
	return false;
}

int iphone_do_config(struct usb_configuration *c)
{
	struct g_iphone *g = g_iphone_from_usb_config(c);

	struct f_iphone *iphone = kzalloc(sizeof(*iphone), GFP_KERNEL);
	if (!iphone)
		return -ENOMEM;

	iphone->config = c->bConfigurationValue;
	iphone->g = g;

	struct usb_function *f = &iphone->func;
	f->name = "iphone";
	f->bind = f_iphone_bind;
	f->unbind = f_iphone_unbind;
	f->set_alt = f_iphone_set_alt;
	f->disable = f_iphone_disable;
	f->setup = f_iphone_setup;
	f->resume = f_iphone_resume;
	f->suspend = f_iphone_suspend;
	f->req_match = f_iphone_req_match;

	return usb_add_function(c, f);
}
