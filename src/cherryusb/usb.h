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

/* Runtime parameters (PUD_CMD_SET_PARAM / PUD_CMD_GET_PARAM).
 *
 * Forward declared on purpose: include/pud.h includes this header, so the
 * protocol types cannot be pulled in here without a cycle.  Whoever needs the
 * definitions (they are passed by pointer here) includes "pud.h" -- usb.c and
 * usbd_vendor.c both do.
 *
 * apply() runs in the vendor request handler, i.e. in the USB interrupt, so it
 * has to stay a register-level operation (and it only does bookkeeping for the
 * panel rotation); read() fills an answer from whatever owns each value. */
struct pud_params;
struct pud_param_state;

void pud_params_apply(const struct pud_params *p);
void pud_params_read(struct pud_param_state *st);

/* Put a rotation the host asked for on the panel.  Called by the display task --
 * MADCTL goes out over the panel bus, which is not something to do in the USB
 * interrupt that accepted the parameter.  Returns true when it changed
 * something. */
bool pud_params_flush_display(void);

/* EP1 flow control.
 *
 * EP1 is armed only while the decoder has a free frame slot, and an un-armed
 * endpoint NAKs: the host's bulk write simply waits instead of the frame being
 * dropped.  tick() is called by the decoder task whenever it frees a slot (and
 * once the device is configured) and arms the next transfer if it can.
 */
void usbd_vendor_ep1_tick(void);

/* EP1 caretaker: called from the decoder task while it waits for work, to drop
 * a transfer whose host stopped making progress and re-arm the endpoint (see
 * usbd_vendor_ep1_poll()). */
void usbd_vendor_ep1_poll(void);

/* How often the decoder task calls the caretaker when it has no work. */
#define EP1_POLL_PERIOD_MS 200

/* Forget the framing state: the endpoint is gone with the bus. */
void usbd_vendor_ep1_reset(void);

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
