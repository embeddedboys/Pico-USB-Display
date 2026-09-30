set(LIBRARY_NAME miniz)

add_library(${LIBRARY_NAME})

# Inflate only (tinfl, miniz 3.0.2).  It is the second stage of the QOI+deflate
# codec (DECODER_TYPE 5): the host deflates the QOI stream, the device inflates
# it back into a band-sized buffer and hands that to the QOI decoder.
target_sources(${LIBRARY_NAME} PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/miniz_tinfl.c
)

target_include_directories(${LIBRARY_NAME} PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
)

if(PUD_CODEC_IN_RAM)
	# Same placement as the QOI/RLE decode loops: tinfl_decompress() is the
	# whole per-byte loop, and XIP flash would put it behind the cache.
	target_compile_definitions(${LIBRARY_NAME} PRIVATE
		"MINIZ_EXPORT=__attribute__((section(\".time_critical.tinfl\")))")
endif()
