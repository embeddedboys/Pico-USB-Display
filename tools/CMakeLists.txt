# pudcodec -- host-side image/video converter for the Pico USB Display.
#
# This is a standalone host project: it links the same codec sources the
# firmware does, so the streams it writes are the ones the device decodes, but
# it is never part of the firmware build (build-pico2/ does not add this
# directory).  Build it out of tree:
#
#   cmake -S tools -B tools/build && cmake --build tools/build -j
#
# stb_image.h / stb_image_write.h are vendored next to this file, so
# configuring needs no network.

cmake_minimum_required(VERSION 3.13)
project(pudcodec C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(PUD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(pudcodec
	pudcodec.c
	codecs.c
	codec_utils.c
	${PUD_ROOT}/src/decoders/qoi/rgb565_qoi.c
	${PUD_ROOT}/src/decoders/rle/rgb565_rle.c
	${PUD_ROOT}/lib/lz4/lib/lz4.c
)

target_include_directories(pudcodec PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${PUD_ROOT}/src/decoders/qoi
	${PUD_ROOT}/src/decoders/rle
	${PUD_ROOT}/lib/lz4/lib
)

target_compile_options(pudcodec PRIVATE -Wall -Wextra)

# stb's JPEG writer needs the maths library; the RGB565 codecs and the JPEG
# decoder do not.
target_link_libraries(pudcodec PRIVATE m)
