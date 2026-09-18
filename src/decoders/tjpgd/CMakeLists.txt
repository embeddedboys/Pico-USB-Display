# ChaN's TJpgDec R0.03, with Bodmer's JDEC.swap addition (see LICENSE).
# Vendored unmodified so it can be diffed against upstream: the only thing this
# project sets is the configuration in tjpgdcnf.h (JD_FORMAT 1 = RGB565,
# JD_FASTDECODE 2 -> a 9.4 KB static workspace).
set(LIBRARY_NAME tjpgd)

add_library(${LIBRARY_NAME})

target_sources(${LIBRARY_NAME} PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/tjpgd.c
)

target_include_directories(${LIBRARY_NAME} PUBLIC
	${CMAKE_CURRENT_LIST_DIR}
)
