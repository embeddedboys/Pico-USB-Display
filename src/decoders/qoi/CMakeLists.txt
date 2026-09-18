set(LIBRARY_NAME qoi)

add_library(${LIBRARY_NAME})

target_sources(${LIBRARY_NAME} PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/rgb565_qoi.c
)

target_include_directories(${LIBRARY_NAME} PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
)

if(PUD_CODEC_IN_RAM)
	# Emit the decode loops into SRAM.  The library's placement hook keeps the
	# vendored file identical to the upstream repository (see notes/architecture.md).
	target_compile_definitions(${LIBRARY_NAME} PRIVATE
		"RGB565_QOI_SECTION=__attribute__((section(\".time_critical.qoi\")))")
endif()
