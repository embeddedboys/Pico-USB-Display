# Standalone entry for the decoder benchmarks.
#
# They need the codec libraries but none of the display path, so this project
# compiles the decoders directly instead of pulling in the firmware.  That keeps
# the number free of anything the display or USB side costs.
#
#   cmake -S bench -B build-bench -DPICO_BOARD=pico2 \
#         -DPUD_CONFIG=pico_dm_qd3503728 -DPICO_SDK_PATH=<sdk>
cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
    message(FATAL_ERROR "set PICO_SDK_PATH")
endif()
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico-usb-display-bench C CXX ASM)
pico_sdk_init()

# Only for TFT_* / DEBUG_UART_* -- the panel is not driven here.
set(PUD_CONFIG "pico_dm_qd3503728" CACHE STRING "panel config from configs/")
include(${CMAKE_CURRENT_LIST_DIR}/../configs/${PUD_CONFIG}.cmake)

set(DECODER_SRC
    ${CMAKE_CURRENT_LIST_DIR}/../src/decoders/qoi/rgb565_qoi.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/decoders/rle/rgb565_rle.c
    ${CMAKE_CURRENT_LIST_DIR}/../lib/lz4/lib/lz4.c
)

add_executable(decode_bench ${CMAKE_CURRENT_LIST_DIR}/decode_bench.c ${DECODER_SRC})
target_include_directories(decode_bench PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../src/decoders/qoi
    ${CMAKE_CURRENT_LIST_DIR}/../src/decoders/rle
    ${CMAKE_CURRENT_LIST_DIR}/../lib/lz4/lib
)
target_link_libraries(decode_bench pico_stdlib)
# The panel config sets these as CMake variables; normally the library's
# drivers/uart/CMakeLists.txt turns them into C defines, and this project
# does not include that, so do it here.
target_compile_definitions(decode_bench PRIVATE
    DEBUG_UART_SPEED=${DEBUG_UART_SPEED}
    DEBUG_UART_TX_PIN=${DEBUG_UART_TX_PIN}
    DEBUG_UART_RX_PIN=${DEBUG_UART_RX_PIN})
pico_enable_stdio_uart(decode_bench 1)
pico_enable_stdio_usb(decode_bench 0)
pico_add_extra_outputs(decode_bench)
