#include "usbd_core.h"
#include "usbd_vendor.h"

#include "decoder.h"

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    return config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
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

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t ep1_read_buffer[EP1_RD_BUF_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t ep2_write_buffer[EP2_WR_BUF_SIZE];
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t ep4_write_buffer[EP4_WR_BUF_SIZE];

static int vendor_request_handler(uint8_t busid, struct usb_setup_packet *setup, uint8_t **data, uint32_t *len)
{
    // USB_LOG_WRN("%s, XXX Class request: "
    //              "bRequest 0x%02x\r\n",
    //              __func__,
    //              setup->bRequest);
    uint32_t transfer_size;
    int xs, ys, xe, ye;

    switch (setup->bRequest) {
        case REQ_EP1_OUT:
            // usb_hexdump(*data, *len);
            memcpy(&xs, *data, sizeof(xs));
            (*data)+=2;

            memcpy(&ys, *data, sizeof(ys));
            (*data)+=2;

            memcpy(&xe, *data, sizeof(xe));
            (*data)+=2;

            memcpy(&ye, *data, sizeof(ye));
            (*data)+=2;

            decoder_set_window(xs, ys, xe, ye);

            memcpy(&transfer_size, *data, sizeof(transfer_size));
            // USB_LOG_WRN("%s, req_ep1_out, x : 0x%04x, y : 0x%04x, size : %d\n",
            //                     __func__, decoder_x, decoder_y, transfer_size);
            return usbd_ep_start_read(busid, EP1_OUT_ADDR, ep1_read_buffer, transfer_size);
        case REQ_EP2_IN:
            // memcpy(&g_udd_data.cmd, *data, sizeof(uint8_t));
            // (*data)++;
            // memcpy(&transfer_size, *data, sizeof(transfer_size));
            // usbd_vendor_ep2_bulk_in_fsm(g_udd_data.cmd, transfer_size);
            return usbd_ep_start_write(busid, EP2_IN_ADDR, ep2_write_buffer, transfer_size);
            break;
        case REQ_EP4_IN:
            return usbd_ep_start_write(busid, EP4_IN_ADDR, ep4_write_buffer, 64);
        default:
            // USB_LOG_WRN("Unhandled XXX Class bRequest 0x%02x\r\n", setup->bRequest);
            return -1;
    }

    return 0;
}

static void vendor_notify_handler(uint8_t busid, uint8_t event, void *arg)
{
    switch (event) {
        case USBD_EVENT_RESET:
            // USB_LOG_WRN("USBD_EVENT_RESET\r\n");
            break;

        default:
            break;
    }
}

struct usbd_interface *usbd_vendor_init_intf(uint8_t busid, struct usbd_interface *intf)
{
    intf->class_interface_handler = NULL;
    intf->class_endpoint_handler = NULL;
    intf->vendor_handler = vendor_request_handler;
    intf->notify_handler = vendor_notify_handler;

    return intf;
}
