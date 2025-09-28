#ifndef _USBD_XXX_H_
#define _USBD_XXX_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "usbd_core.h"

#define VENDOR_ID  0x2E8A
#define PRODUCT_ID 0x0001

#define USBD_MAX_POWER 500

#define EP0_IN_ADDR  (USB_EP_DIR_IN  | 0)
#define EP0_OUT_ADDR (USB_EP_DIR_OUT | 0)
#define EP1_OUT_ADDR (USB_EP_DIR_OUT | 1)
#define EP2_IN_ADDR  (USB_EP_DIR_IN  | 2)
#define EP3_OUT_ADDR (USB_EP_DIR_OUT | 3)
#define EP4_IN_ADDR  (USB_EP_DIR_IN  | 4)

#define REQ_EP0_OUT     0X00
#define REQ_EP0_IN      0X01
#define REQ_EP1_OUT     0X02
#define REQ_EP2_IN      0X03
#define REQ_EP3_OUT     0X04
#define REQ_EP4_IN      0X05

#define EP1_RD_BUF_SIZE 131072
#define EP2_WR_BUF_SIZE 128
#define EP4_WR_BUF_SIZE 128

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0, 0, 0, VENDOR_ID, PRODUCT_ID, 0, 1),
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(9 + 9 + 7 + 7 + 7, 1, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(0, 0, 3, 0xFF, 0, 0, 0),
    USB_ENDPOINT_DESCRIPTOR_INIT(EP1_OUT_ADDR, USB_ENDPOINT_TYPE_BULK, USB_BULK_EP_MPS_FS, 0),
    USB_ENDPOINT_DESCRIPTOR_INIT(EP2_IN_ADDR, USB_ENDPOINT_TYPE_BULK, USB_BULK_EP_MPS_FS, 0),
    USB_ENDPOINT_DESCRIPTOR_INIT(EP4_IN_ADDR, USB_ENDPOINT_TYPE_INTERRUPT, 64, 33)
};

static const uint8_t device_quality_descriptor[] = {
    ///////////////////////////////////////
    /// device qualifier descriptor
    ///////////////////////////////////////
    0x0a,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00,
    0x02,
    0x00,
    0x00,
    0x00,
    0x40,
    0x00,
    0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 }, /* Langid */
    "embeddedboys",                  /* Manufacturer */
    "Pico USB Display",         	 /* Product */
    "20250920",                      /* Serial Number */
};

extern const struct usb_descriptor xxx_vendor_descriptor;

struct usbd_interface *usbd_vendor_init_intf(uint8_t busid, struct usbd_interface *intf);

#ifdef __cplusplus
}
#endif

#endif /* _USBD_XXX_H_ */
