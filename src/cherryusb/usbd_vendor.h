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

/* Largest single EP1 transfer the firmware accepts, in bytes.  The same number
 * sizes ep1_read_buffer (below) and the decoder frame slot (decoder.c); it is
 * the hard limit the host must respect when splitting a rectangle into bands,
 * and the device advertises it via PUD_CMD_GET_CAPS so one host build can serve
 * both boards instead of being rebuilt per board.  A transfer is a
 * struct pud_ep1_header plus the payload, so the payload itself may be
 * PUD_EP1_HEADER_SIZE bytes shorter than this.
 *
 * RP2040 has 256 KB of usable SRAM and does not fit the RP2350 values: built
 * with 128 KB + 2 x 64 KB the .data/.bss came out at 109% of RAM and did not
 * link.  32 KB per transfer fits with room for the heap; the cost is that a
 * full-screen refresh needs ~15 bands instead of 8, which is inherent to the
 * smaller part.
 */
#ifndef PUD_MAX_TRANSFER
#if defined(PICO_RP2040)
#define PUD_MAX_TRANSFER (32 * 1024)
#else
#define PUD_MAX_TRANSFER (64 * 1024) /* one USB_TRANS_MAX_SIZE (65535) transfer */
#endif
#endif

/* The staging buffer only has to hold one accepted transfer, so it is the same
 * number as the protocol limit.  Measured performance-neutral against the old
 * 128 KB (5.50 ms either way on a full-screen single-transfer frame), and it
 * saves 64 KB on RP2350 / 96 KB on RP2040. */
#define EP1_RD_BUF_SIZE PUD_MAX_TRANSFER

/* The first EP1 read asks for one max-size packet: the header is always inside
 * it, and its length is what tells the device how much payload to expect. */
#define EP1_FIRST_READ USB_BULK_EP_MPS_FS

#define EP2_WR_BUF_SIZE 128
#define EP4_WR_BUF_SIZE 128

/*
 * EP4 is an interrupt IN endpoint and bInterval caps how often the host comes
 * to collect a report: at 33 ms the panel could only ever deliver ~30 samples
 * per second no matter how fast the firmware polled it (measured: 32 ms
 * between consecutive points during a drag).  8 ms lets the 10 ms touch poll
 * through at ~120 Hz; the report itself is 8 bytes, so the periodic bandwidth
 * this reserves is noise next to the EP1 image stream.
 */
#define EP4_POLL_INTERVAL_MS 8

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0, 0, 0, VENDOR_ID, PRODUCT_ID, 0, 1),
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(9 + 9 + 7 + 7 + 7, 1, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(0, 0, 3, 0xFF, 0, 0, 0),
    USB_ENDPOINT_DESCRIPTOR_INIT(EP1_OUT_ADDR, USB_ENDPOINT_TYPE_BULK, USB_BULK_EP_MPS_FS, 0),
    USB_ENDPOINT_DESCRIPTOR_INIT(EP2_IN_ADDR, USB_ENDPOINT_TYPE_BULK, USB_BULK_EP_MPS_FS, 0),
    USB_ENDPOINT_DESCRIPTOR_INIT(EP4_IN_ADDR, USB_ENDPOINT_TYPE_INTERRUPT, 64, EP4_POLL_INTERVAL_MS)
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
/* usbd_vendor_ep2_bulk_in_fsm() is declared in usb.h, next to its definition
 * in usb.c -- it used to be declared here as well, and the two copies drifted
 * apart when its return type changed. */

#ifdef __cplusplus
}
#endif

#endif /* _USBD_XXX_H_ */
