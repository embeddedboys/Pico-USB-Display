#ifndef __USB_H
#define __USB_H

#include <hardware/structs/usb.h>

#include <stdbool.h>

#include "usbd_core.h"
#include "usbd_vendor.h"

void usb_device_init(void);
bool usb_is_configured(void);

void usbd_vendor_ep2_bulk_in_fsm(uint8_t cmd, uint32_t len);

#endif /* __USB_H */
