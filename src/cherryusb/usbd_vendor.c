#include "usbd_core.h"
#include "usbd_vendor.h"
#include "usb.h"
/* struct pud_params / pud_param_state (the request struct embeds one by value) */
#include "pud.h"

#include "decoder.h"

/* the reset interface: reset_usb_boot() (bootrom) and watchdog_reboot() */
#include "hardware/watchdog.h"
#include "pico/bootrom.h"

static const u8 *device_descriptor_callback(u8 speed)
{
	return device_descriptor;
}

static const u8 *config_descriptor_callback(u8 speed)
{
	return config_descriptor;
}

static const u8 *device_quality_descriptor_callback(u8 speed)
{
	return device_quality_descriptor;
}

static const char *string_descriptor_callback(u8 speed, u8 index)
{
	if (index > 3)
		return NULL;

	return string_descriptors[index];
}

const struct usb_descriptor xxx_vendor_descriptor = {
	.device_descriptor_callback = device_descriptor_callback,
	.config_descriptor_callback = config_descriptor_callback,
	.device_quality_descriptor_callback =
	        device_quality_descriptor_callback,
	.string_descriptor_callback = string_descriptor_callback
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX u8 ep1_read_buffer[EP1_RD_BUF_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX u8 ep2_write_buffer[EP2_WR_BUF_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX u8 ep4_write_buffer[EP4_WR_BUF_SIZE];

struct req_ep2_in {
	u16 cmd;
	u16 size;
};

/* Runtime parameters: the request header the query path uses, followed by the
 * values being set.  4 + sizeof(struct pud_params) bytes, well inside one
 * full-speed control transfer. */
struct req_set_param {
	u16 cmd;
	u16 size;
	struct pud_params params;
};

/* The wire layout is shared with the host (struct pud_params in the driver's
 * pud.h, PARAMS_STRUCT / PARAM_STATE_STRUCT in scripts/pud_usb.py), so the
 * padding has to be what everyone assumes: no padding at all. */
_Static_assert(sizeof(struct pud_params) == 8, "pud_params wire size");
_Static_assert(sizeof(struct pud_param_state) == 12, "pud_param_state wire size");
_Static_assert(sizeof(struct req_set_param) == 12, "set_param wire size");

static int vendor_request_handler(uint8_t busid, struct usb_setup_packet *setup,
                                  uint8_t **data, uint32_t *len)
{
	static struct req_ep2_in *req_ep2_in;

	switch (setup->bRequest) {
	/* REQ_EP1_OUT (0x02) is gone: the rectangle now travels in the EP1 header
	 * (protocol v2, see notes/usb-protocol.md).  An old host gets a stall here,
	 * which is what we want -- silently misparsing its frames would be worse. */
	case REQ_EP2_IN:
		req_ep2_in = (struct req_ep2_in *)*data;
		/* The fsm returns how much it actually produced: writing the
		 * host's requested size instead would leak stale bytes out of
		 * ep2_write_buffer for a command with a short answer. */
		return usbd_ep_start_write(
		        busid, EP2_IN_ADDR, ep2_write_buffer,
		        usbd_vendor_ep2_bulk_in_fsm(req_ep2_in->cmd,
		                                    req_ep2_in->size));
	case REQ_SET_PARAM: {
		struct req_set_param *req;

		/* A short write means the host's framing was wrong, and so does
		 * a size field that does not cover the values -- stall rather
		 * than apply half of what it sent. */
		if (*len < sizeof(*req))
			return -1;

		req = (struct req_set_param *)*data;
		if (req->cmd != PUD_CMD_SET_PARAM ||
		    req->size < sizeof(req->params))
			return -1;

		/* Runs in the USB interrupt: the only parameter that is applied
		 * here (brightness) is a PWM level write, which is a few
		 * arithmetic ops and one register store.  Anything heavier has
		 * to be handed to a task, not done in this context -- see the
		 * decode-path note in the repository's AGENTS.md. */
		pud_params_apply(&req->params);
		return 0;
	}
	case REQ_EP4_IN:
		/* The device pushes touch reports by itself; this request is for
		 * a host that polls instead, and hands it the current report. */
		return usbd_vendor_ep4_request();
	default:
		return -1;
	}

	return 0;
}

static void vendor_notify_handler(u8 busid, u8 event, void *arg)
{
	switch (event) {
	case USBD_EVENT_RESET:
		/* the endpoint state is gone with the bus, so an in-flight
		 * report will never complete */
		usbd_vendor_ep4_reset();
		break;

	default:
		break;
	}
}

struct usbd_interface *usbd_vendor_init_intf(u8 busid,
                                             struct usbd_interface *intf)
{
	intf->class_interface_handler = NULL;
	intf->class_endpoint_handler = NULL;
	intf->vendor_handler = vendor_request_handler;
	intf->notify_handler = vendor_notify_handler;

	return intf;
}

/* picoboot's reset interface (see the descriptor comment in usbd_vendor.h).
 * picotool sends a *class* request addressed to the interface (bmRequestType
 * 0x21), which CherryUSB dispatches to the class_interface_handler of the
 * interface whose number is in wIndex -- hence a handler of its own here rather
 * than one more case in the vendor handler above (that one only ever sees
 * vendor-type requests).
 *
 * Both requests end in a chip reset, so neither returns: the EP0 status stage is
 * never reached and the host sees the device disappear instead, which is what
 * picotool expects (it then waits for the device to re-enumerate).
 *
 * This runs in the USB interrupt and is safe there: reset_usb_boot() is a store
 * into the watchdog scratch register plus a watchdog trigger, with no bus access
 * to wait for -- the reasoning behind AGENTS.md invariant 11. */
static int reset_request_handler(uint8_t busid, struct usb_setup_packet *setup,
                                 uint8_t **data, uint32_t *len)
{
	(void)busid;
	(void)data;
	(void)len;

	switch (setup->bRequest) {
	case RESET_REQUEST_BOOTSEL:
		/* wValue bits 0-6 are the set of interfaces to leave out of
		 * BOOTSEL mode; picotool also puts its optional --bootsel-led
		 * bits in wValue (bit 8 "pin specified", bits 9+ the pin), which
		 * this board has no use for and ignores. */
		reset_usb_boot(0, setup->wValue & 0x7f);
		break;
	case RESET_REQUEST_FLASH:
		/* "reboot back into the application": the same reset, without the
		 * bootrom's BOOTSEL detour */
		watchdog_reboot(0, 0, RESET_TO_FLASH_DELAY_MS);
		break;
	default:
		return -1;
	}

	return 0;
}

struct usbd_interface *usbd_reset_init_intf(u8 busid, struct usbd_interface *intf)
{
	(void)busid;

	intf->class_interface_handler = reset_request_handler;
	intf->class_endpoint_handler = NULL;
	intf->vendor_handler = NULL;
	intf->notify_handler = NULL;

	return intf;
}
