# RGB565 RLE, vendored from the rgb565-rle repository (MIT), including the
# run-body optimization: a run is filled or copied four pixels at a time
# instead of one 16-bit store per pixel (see notes/decoders.md).
# Zero static state: it decodes into the caller's buffers, so it costs no RAM
# of its own (unlike the JPEG decoders' workspaces).
set(LIBRARY_NAME rle)

add_library(${LIBRARY_NAME})

target_sources(${LIBRARY_NAME} PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/rgb565_rle.c
)

target_include_directories(${LIBRARY_NAME} PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
)
