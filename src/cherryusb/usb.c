#include "usb.h"

// #include "udd.h"
#include "decoder.h"

void usbd_event_handler(uint8_t busid, uint8_t event)
{
	switch (event) {
		case USBD_EVENT_RESET:
			break;
		case USBD_EVENT_CONNECTED:
			break;
		case USBD_EVENT_DISCONNECTED:
			break;
		case USBD_EVENT_RESUME:
			break;
		case USBD_EVENT_SUSPEND:
			break;
		case USBD_EVENT_CONFIGURED:
			break;
		case USBD_EVENT_SET_REMOTE_WAKEUP:
			break;
		case USBD_EVENT_CLR_REMOTE_WAKEUP:
			break;
		default:
			// USB_LOG_WRN("Unhandled event : %d, bus id : %d\n", event, busid);
			break;
	}
}

extern uint8_t ep1_read_buffer[65536];
extern uint8_t ep2_write_buffer[128];
extern uint8_t ep4_write_buffer[128];
extern uint32_t frame_counter;

void usbd_vendor_ep1_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
	if (!nbytes)
		return;

	mutex_enter_blocking(&decoder_mutex);
	decoder_drawimg(decoder_x, decoder_y, ep1_read_buffer, nbytes);
	mutex_exit(&decoder_mutex);
}

void usbd_vendor_ep2_bulk_in_fsm(uint8_t cmd, uint32_t len)
{
	switch (cmd) {
		case 0x01:
			// memcpy(ep2_write_buffer, g_udd_data.sn, len);
			break;
		default:
			break;
	}
}

/* TODO: ep3 */

void usbd_vendor_ep2_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
	// uint8_t cmd = g_udd_data.cmd;
	// USB_LOG_INFO("%s, cmd : %02x, len : %d\n", __func__, cmd, nbytes);
}

void usbd_vendor_ep4_int_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
	// USB_LOG_RAW("%s, len:%d\r\n", __func__, nbytes);
	// static struct tp_data *tp = &g_udd_data.tp;
	// uint8_t *buf = ep4_write_buffer;

	// buf[0] = tp->is_pressed;
	// buf[1] = tp->x >> 8;
	// buf[2] = tp->x & 0xFF;
	// buf[3] = tp->y >> 8;
	// buf[4] = tp->y & 0xFF;
}

struct usbd_endpoint vendor_out_ep1 = {
	.ep_addr = EP1_OUT_ADDR,
	.ep_cb = usbd_vendor_ep1_bulk_out,
};

struct usbd_endpoint vendor_in_ep2 = {
	.ep_addr = EP2_IN_ADDR,
	.ep_cb = usbd_vendor_ep2_bulk_in,
};

struct usbd_endpoint vendor_in_ep4 = {
	.ep_addr = EP4_IN_ADDR,
	.ep_cb = usbd_vendor_ep4_int_in,
};

struct usbd_interface intf0;

void usb_device_init()
{
	USB_LOG_WRN("%s\n", __func__);

	usbd_desc_register(0, &xxx_vendor_descriptor);

	usbd_add_interface(0, usbd_vendor_init_intf(0, &intf0));

	usbd_add_endpoint(0, &vendor_out_ep1);
	usbd_add_endpoint(0, &vendor_in_ep2);
	usbd_add_endpoint(0, &vendor_in_ep4);

	usbd_initialize(0, USBCTRL_REGS_BASE, usbd_event_handler);
}

bool usb_is_configured(void)
{
	return true;
}
