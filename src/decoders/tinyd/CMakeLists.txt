# tinyd: the compact dictionary window decoder used by DECODER_TYPE 6.
#
# It is ours, not vendored, so it follows the project's conventions rather than
# an upstream file's.  It decodes stored blocks and *fixed Huffman* blocks only,
# which is what a host gets by asking zlib for strategy=Z_FIXED (+11.0% bytes on
# the measured band, and 15.5 vs 18.9 cycles/output byte on the device -- see
# notes/decoders.md), and it takes its history from the caller's buffer instead
# of carrying a 32 KB window of its own.
set(LIBRARY_NAME tinyd)

add_library(${LIBRARY_NAME})

target_sources(${LIBRARY_NAME} PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/tinyd.c
)

target_include_directories(${LIBRARY_NAME} PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
)
