set(LIBRARY_NAME cherryusb_device)

add_library(${LIBRARY_NAME})

target_sources(${LIBRARY_NAME} PRIVATE
	usb.c
	usbd_vendor.c
	${CHERRY_USB_PATH}/port/rp2040/usb_dc_rp2040.c
	${CHERRY_USB_PATH}/core/usbd_core.c
)

target_include_directories(${LIBRARY_NAME} PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
	${CHERRY_USB_PATH}/common
	${CHERRY_USB_PATH}/core
	${CHERRY_USB_PATH}/port/rp2040
)

target_link_libraries(${LIBRARY_NAME} PRIVATE
	pico_stdlib
	hardware_irq
)
