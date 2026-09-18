#include <string.h>

#include "pud.h"
#include "usb.h"
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

extern uint8_t ep1_read_buffer[EP1_RD_BUF_SIZE];
extern uint8_t ep2_write_buffer[EP2_WR_BUF_SIZE];
extern uint8_t ep4_write_buffer[EP4_WR_BUF_SIZE];

/* Flow control: see usb.h.  EP1 is deliberately left un-armed while the
 * decoder is busy, which stalls the host's bulk write instead of dropping
 * the frame.  Only ever touched from the USB ISR and the decoder task.
 */
static volatile uint32_t s_ep1_pending_size;

/* Diagnostics, readable from a debugger: REQ_EP1_OUT requests whose declared
 * size did not fit ep1_read_buffer.  Should stay 0 with a matching host. */
volatile u32 g_ep1_stat_oversize;

/* The host declares the transfer length in the REQ_EP1_OUT control request and
 * it is used verbatim as the EP1 read length, so it has to fit
 * ep1_read_buffer.  A request that does not is refused at the control stage:
 * vendor_request_handler() returns -1, CherryUSB stalls EP0, and the host sees
 * an error on its flush instead of the bulk read overflowing the buffer. */
bool usbd_vendor_ep1_size_ok(uint32_t size)
{
	if (!size)
		return false;

	if (size <= EP1_RD_BUF_SIZE)
		return true;

	g_ep1_stat_oversize++;
	return false;
}

void usbd_vendor_ep1_arm(uint32_t size)
{
	/* Already validated by usbd_vendor_ep1_size_ok() at the control stage;
	 * this guards the only path that feeds the controller. */
	if (!size || size > EP1_RD_BUF_SIZE)
		return;

	usbd_ep_start_read(0, EP1_OUT_ADDR, ep1_read_buffer, size);
}

void usbd_vendor_ep1_defer(uint32_t size)
{
	s_ep1_pending_size = size;
}

/* Called by the decoder task after releasing a frame slot. */
void usbd_vendor_ep1_tick(void)
{
	uint32_t size = s_ep1_pending_size;

	if (size) {
		s_ep1_pending_size = 0;
		usbd_vendor_ep1_arm(size);
	}
}

void usbd_vendor_ep1_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
	if (!nbytes)
		return;

	/* Do not decode here: this runs on the USB interrupt stack. Hand the
	 * frame to the decoder task instead.
	 */
	decoder_submit_frame(decoder_xs, decoder_ys, decoder_xe, decoder_ye,
			     ep1_read_buffer, nbytes);
}

/* Fills ep2_write_buffer for one query and returns how many bytes to send.
 * The caller writes exactly that many, so a command that produces less than
 * the host asked for cannot leak stale buffer contents past its payload. */
uint32_t usbd_vendor_ep2_bulk_in_fsm(uint8_t cmd, uint32_t len)
{
	/* remember the last command; one byte, not the host's length */
	pud_set_rw_cmd(&cmd, sizeof(cmd));
	switch (cmd) {
	case PUD_CMD_GET_SN:
		pud_get_ro_sn(ep2_write_buffer, len);
		break;
	case PUD_CMD_GET_CAPS: {
		const struct pud_caps caps = {
			.magic = PUD_CAPS_MAGIC,
			.proto_ver = PUD_PROTO_VER,
			.frame_max = PUD_MAX_TRANSFER,
			.decoder_type = DECODER_TYPE,
		};

		if (len > sizeof(caps))
			len = sizeof(caps);
		memcpy(ep2_write_buffer, &caps, len);
		break;
	}
	default:
		/* Answer with nothing rather than the host's requested size:
		 * ep2_write_buffer still holds the previous answer, so replying
		 * with stale bytes makes an unknown command look like a valid
		 * response (measured: cmd 0x7f returned the previous caps
		 * report).  A zero-length write makes the host's bulk read come
		 * back short, which is what it should treat as unsupported. */
		len = 0;
		break;
	}

	return len;
}

/* TODO: ep3 */

void usbd_vendor_ep2_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
	// uint8_t cmd = g_udd_data.cmd;
	// USB_LOG_INFO("%s, cmd : %02x, len : %d\n", __func__, cmd, nbytes);
}

/*
 * EP4: touch reports.
 *
 * The device pushes one report per touch poll and the host keeps an interrupt
 * URB pending, so there is no per-sample handshake that can be lost -- the
 * first version armed the endpoint from a control request and a single failed
 * URB left the driver's input dead.  A report that arrives while the previous
 * one is still in flight replaces it: the host only needs the latest state, and
 * queueing stale samples would only add latency.
 */
static struct pud_touch_report s_ep4_report;
static volatile bool s_ep4_busy;	/* transfer in flight */
static volatile bool s_ep4_dirty;	/* a newer report arrived meanwhile */

static int usbd_vendor_ep4_arm(void)
{
	int rc;

	memcpy(ep4_write_buffer, &s_ep4_report, sizeof(s_ep4_report));
	s_ep4_dirty = false;
	s_ep4_busy = true;

	rc = usbd_ep_start_write(0, EP4_IN_ADDR, ep4_write_buffer,
				 sizeof(s_ep4_report));
	if (rc < 0)
		s_ep4_busy = false;

	return rc;
}

int usbd_vendor_ep4_submit(const struct pud_touch_report *report)
{
	if (report == NULL)
		return -1;

	s_ep4_report = *report;

	if (s_ep4_busy) {
		/* the host has not taken the previous one yet; the next
		 * completion re-arms with this one */
		s_ep4_dirty = true;
		return 0;
	}

	return usbd_vendor_ep4_arm();
}

int usbd_vendor_ep4_request(void)
{
	if (s_ep4_busy)
		return 0;	/* already on its way to the host */

	return usbd_vendor_ep4_arm();
}

void usbd_vendor_ep4_reset(void)
{
	s_ep4_busy = false;
	s_ep4_dirty = false;
	memset(&s_ep4_report, 0, sizeof(s_ep4_report));
	s_ep4_report.version = PUD_TOUCH_VERSION;
}

void usbd_vendor_ep4_int_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
	(void)busid;
	(void)ep;
	(void)nbytes;

	s_ep4_busy = false;

	/* a sample arrived while this one was in flight: send the newer one */
	if (s_ep4_dirty)
		usbd_vendor_ep4_arm();
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

	/* so a host can tell "touch is implemented" (version != 0) before
	 * anybody has touched the panel */
	usbd_vendor_ep4_reset();

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
