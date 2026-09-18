#ifndef __USB_H
#define __USB_H

#include <hardware/structs/usb.h>

#include <stdbool.h>

#include "usbd_core.h"
#include "usbd_vendor.h"

void usb_device_init(void);
bool usb_is_configured(void);

/* Fills ep2_write_buffer for one query; returns the number of bytes the caller
 * must send on EP2 (never more than the host asked for). */
uint32_t usbd_vendor_ep2_bulk_in_fsm(uint8_t cmd, uint32_t len);

/* EP1 flow control.
 *
 * The host only starts the bulk transfer after its REQ_EP1_OUT control
 * request, so leaving EP1 un-armed applies back-pressure: the host's bulk
 * write just waits.  When the decoder has no free frame slot the read is
 * deferred (usbd_vendor_ep1_defer) and re-armed by the decoder task as soon
 * as it frees one (usbd_vendor_ep1_tick), so frames are never dropped.
 */
void usbd_vendor_ep1_arm(uint32_t size);
void usbd_vendor_ep1_defer(uint32_t size);
void usbd_vendor_ep1_tick(void);

/* True when a host-declared transfer length fits ep1_read_buffer.  Called from
 * the REQ_EP1_OUT control stage, which stalls EP0 when it returns false. */
bool usbd_vendor_ep1_size_ok(uint32_t size);

/* EP4 touch reports (the struct is in pud.h, which includes this file).
 *
 * The device pushes: submit() hands over the newest report and arms the
 * interrupt IN transfer unless the previous one is still in flight, in which
 * case the newer sample replaces it (coalescing -- the host only ever needs the
 * latest state, and queueing would add latency).  request() arms the current
 * report if the endpoint is idle, which is what a polling host uses. */
struct pud_touch_report;
int usbd_vendor_ep4_submit(const struct pud_touch_report *report);
int usbd_vendor_ep4_request(void);
void usbd_vendor_ep4_reset(void);

#endif /* __USB_H */
