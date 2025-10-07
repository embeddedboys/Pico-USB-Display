#include "usbd_core.h"
#include "usbd_vendor.h"

#include "decoder.h"

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
	.device_quality_descriptor_callback = device_quality_descriptor_callback,
	.string_descriptor_callback = string_descriptor_callback
};

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX u8 ep1_read_buffer[EP1_RD_BUF_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX u8 ep2_write_buffer[EP2_WR_BUF_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX u8 ep4_write_buffer[EP4_WR_BUF_SIZE];

struct req_ep1_out {
	u16	xs;
	u16	ys;
	u16	xe;
	u16	ye;
	u32	size;
};

struct req_ep2_in {
	u8	cmd;
	u32	size;
};

static int vendor_request_handler(uint8_t busid, struct usb_setup_packet *setup, uint8_t **data, uint32_t *len)
{
	// USB_LOG_WRN("%s, XXX Class request: "
	//              "bRequest 0x%02x\r\n",
	//              __func__,
	//              setup->bRequest);
	static struct req_ep1_out *req_ep1_out;
	static struct req_ep2_in *req_ep2_in;

	switch (setup->bRequest) {
		case REQ_EP1_OUT:
			// usb_hexdump(*data, *len);
			req_ep1_out = (struct req_ep1_out *)*data;

			decoder_set_window(req_ep1_out->xs, req_ep1_out->ys,
					req_ep1_out->xe, req_ep1_out->ye);

			// USB_LOG_WRN("%s, req_ep1_out, x : 0x%04x, y : 0x%04x, size : %d\n",
			//                     __func__, decoder_x, decoder_y, transfer_size);
			return usbd_ep_start_read(busid, EP1_OUT_ADDR,
						ep1_read_buffer, req_ep1_out->size);
		case REQ_EP2_IN:
			req_ep2_in = (struct req_ep2_in *)*data;
			usbd_vendor_ep2_bulk_in_fsm(req_ep2_in->cmd, req_ep2_in->size);
			return usbd_ep_start_write(busid, EP2_IN_ADDR,
						ep2_write_buffer, req_ep2_in->size);
		case REQ_EP4_IN:
			return usbd_ep_start_write(busid, EP4_IN_ADDR, ep4_write_buffer, 64);
		default:
			// USB_LOG_WRN("Unhandled XXX Class bRequest 0x%02x\r\n", setup->bRequest);
			return -1;
	}

	return 0;
}

static void vendor_notify_handler(u8 busid, u8 event, void *arg)
{
	switch (event) {
		case USBD_EVENT_RESET:
		// USB_LOG_WRN("USBD_EVENT_RESET\r\n");
		break;

	default:
		break;
	}
}

struct usbd_interface *usbd_vendor_init_intf(u8 busid, struct usbd_interface *intf)
{
	intf->class_interface_handler = NULL;
	intf->class_endpoint_handler = NULL;
	intf->vendor_handler = vendor_request_handler;
	intf->notify_handler = vendor_notify_handler;

	return intf;
}
