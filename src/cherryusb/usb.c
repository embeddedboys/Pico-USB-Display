#include <string.h>

#include "pud.h"
#include "usb.h"
#include "decoder.h"

void usbd_event_handler(uint8_t busid, uint8_t event)
{
	switch (event) {
	case USBD_EVENT_RESET:
		usbd_vendor_ep1_reset();
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
		/* EP1 is open now: let the host start sending frames */
		usbd_vendor_ep1_tick();
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

/*
 * EP1 OUT, protocol v2 (see notes/usb-protocol.md): every transfer is a
 * struct pud_ep1_header followed by the payload it describes.  The first read
 * asks for one max-size packet, which always holds the whole header; the
 * header's size then says exactly how many more bytes belong to this transfer,
 * so the end of a transfer never depends on a short packet.
 *
 * Flow control: EP1 is armed only while a decoder slot is free -- an un-armed
 * endpoint NAKs, so the host's bulk write waits instead of the frame being
 * dropped.  Only touched from the USB ISR and the decoder task.
 */
static volatile bool s_ep1_armed;     /* a read is in flight */
static volatile bool s_ep1_stalled;   /* refused a transfer, stall not cleared */
static volatile uint32_t s_ep1_got;   /* bytes of this transfer received */
static volatile uint32_t s_ep1_total; /* header + payload; 0 until parsed */
static struct pud_ep1_header s_ep1_hdr;

/* Diagnostics, readable from a debugger; both stay 0 with a matching host. */
volatile u32 g_ep1_stat_oversize; /* declared more than ep1_read_buffer holds */
volatile u32 g_ep1_stat_bad;      /* no usable header, or a length mismatch */

void usbd_vendor_ep1_reset(void)
{
	s_ep1_armed = false;
	s_ep1_stalled = false;
	s_ep1_got = 0;
	s_ep1_total = 0;
}

/* Called by the decoder task after releasing a frame slot, and once the device
 * is configured. */
void usbd_vendor_ep1_tick(void)
{
	if (s_ep1_armed || !decoder_slot_free())
		return;

	if (s_ep1_stalled) {
		s_ep1_stalled = false;
		usbd_ep_clear_stall(0, EP1_OUT_ADDR);
	}

	s_ep1_armed = true;
	s_ep1_got = 0;
	s_ep1_total = 0;
	usbd_ep_start_read(0, EP1_OUT_ADDR, ep1_read_buffer, EP1_FIRST_READ);
}

/* Ask for whatever is still missing from this transfer.  The header packet
 * already put its bytes in place, so the payload stays contiguous behind it. */
static void ep1_read_more(void)
{
	uint32_t want = s_ep1_total ? s_ep1_total - s_ep1_got : EP1_FIRST_READ;

	s_ep1_armed = true;
	usbd_ep_start_read(0, EP1_OUT_ADDR, ep1_read_buffer + s_ep1_got, want);
}

/* One transfer is over: either hand the payload to the decoder task or just
 * take the next one. */
static void ep1_finish(bool submit)
{
	uint16_t xs = s_ep1_hdr.xs, ys = s_ep1_hdr.ys;
	uint16_t xe = s_ep1_hdr.xe, ye = s_ep1_hdr.ye;
	uint32_t size = s_ep1_hdr.size;

	s_ep1_got = 0;
	s_ep1_total = 0;

	if (submit && size) {
		/* Do not decode here: this runs on the USB interrupt stack. */
		decoder_submit_frame(xs, ys, xe, ye,
				     ep1_read_buffer + PUD_EP1_HEADER_SIZE,
				     size);
		/* The payload was copied into a frame slot, so ep1_read_buffer is
		 * free again: arm the next transfer right away if a slot is still
		 * free.  Waiting for the decoder task instead would leave EP1
		 * un-armed while the host is already writing the next band, and a
		 * NAK on a full-speed bulk pipe costs a whole frame. */
	}

	usbd_vendor_ep1_tick();
}

void usbd_vendor_ep1_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
	(void)busid;
	(void)ep;

	s_ep1_armed = false;

	if (!nbytes)
		return;

	s_ep1_got += nbytes;

	if (!s_ep1_total) {
		/* still reading the packet that carries the header */
		if (s_ep1_got < PUD_EP1_HEADER_SIZE) {
			if (nbytes < EP1_FIRST_READ) {
				/* short packet: the transfer is over and
				 * never held a whole header */
				g_ep1_stat_bad++;
				ep1_finish(false);
			} else {
				ep1_read_more();
			}
			return;
		}

		memcpy(&s_ep1_hdr, ep1_read_buffer, sizeof(s_ep1_hdr));
		s_ep1_total = PUD_EP1_HEADER_SIZE + s_ep1_hdr.size;

		if (s_ep1_total > EP1_RD_BUF_SIZE) {
			/* Refuse it loudly.  Stalling EP1 fails the host's
			 * write instead of dropping the transfer and then
			 * misparsing whatever follows it. */
			g_ep1_stat_oversize++;
			s_ep1_got = 0;
			s_ep1_total = 0;
			s_ep1_stalled = true;
			usbd_ep_set_stall(0, EP1_OUT_ADDR);
			return;
		}
	}

	if (s_ep1_got < s_ep1_total) {
		ep1_read_more();
		return;
	}

	if (s_ep1_got != s_ep1_total) {
		/* the host sent more than it declared */
		g_ep1_stat_bad++;
		ep1_finish(false);
		return;
	}

	ep1_finish(true);
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
			.xres = g_pud_data.disp.xres,
			.yres = g_pud_data.disp.yres,
			.pixelclock_khz = g_pud_data.disp.pixelclock_khz,
			.rotation = g_pud_data.disp.rotation,
			.bpp = g_pud_data.disp.bpp,
			.intf_type = g_pud_data.disp.intf_type,
			.tp_polling_period = INDEV_DRV_NOT_USED ? 0 : g_pud_data.tp.polling_period,
			.width_mm = g_pud_data.disp.width_mm,
			.height_mm = g_pud_data.disp.height_mm,
			/* the host uses this to decide whether to register an input
			 * device at all */
			.flags = INDEV_DRV_NOT_USED ? 0 : PUD_CAPS_TOUCH,
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
	/* version 0 tells a polling host that this build has no touch driver at
	 * all (most board configs set INDEV_DRV_NOT_USED); the capability reply
	 * carries the same fact as PUD_CAPS_TOUCH */
	s_ep4_report.version = INDEV_DRV_NOT_USED ? 0 : PUD_TOUCH_VERSION;
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
