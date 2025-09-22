set(LIBRARY_NAME cherryusb_device)

add_library(${LIBRARY_NAME} INTERFACE)

target_sources(${LIBRARY_NAME} INTERFACE
	usb.c
	usbd_vendor.c
	${CHERRY_USB_PATH}/port/rp2040/usb_dc_rp2040.c
	${CHERRY_USB_PATH}/core/usbd_core.c
)

target_include_directories(${LIBRARY_NAME} INTERFACE
	${CMAKE_CURRENT_LIST_DIR}
	${CHERRY_USB_PATH}/common
	${CHERRY_USB_PATH}/core
	${CHERRY_USB_PATH}/port/rp2040
)

target_link_libraries(${LIBRARY_NAME} INTERFACE
	pico_stdlib
	hardware_irq
)
