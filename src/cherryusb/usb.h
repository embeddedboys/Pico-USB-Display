#ifndef __USB_H
#define __USB_H

#include <hardware/structs/usb.h>

#include <stdbool.h>

#include "usbd_core.h"
#include "usbd_vendor.h"

void usb_device_init(void);
bool usb_is_configured(void);

void usbd_vendor_ep2_bulk_in_fsm(uint8_t cmd, uint32_t len);

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

#endif /* __USB_H */
