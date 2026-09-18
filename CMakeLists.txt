cmake_minimum_required(VERSION 3.13)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)
include(pico_extras_import_optional.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "2.0.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 2.0.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

# If you want debug output from USB (pass -DPICO_STDIO_USB=1) this ensures you don't lose any debug output while USB is set up
if (NOT DEFINED PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS)
    set(PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS 3000)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
SET(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -Wl,--print-memory-usage")
SET(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -Wl,--print-memory-usage")

set(PICO_DISPLAY_LIB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-display-lib)
set(PICO_DISPLAY_LIB_CONFIG_PATH ${PICO_DISPLAY_LIB_PATH}/configs)
set(CHERRY_USB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/CherryUSB)
set(LZ4_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/lz4)
set(PUD_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

# set the project name
project(pico-usb-display C CXX ASM)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# **********  select the suitable config file  **********
include(${PICO_DISPLAY_LIB_CONFIG_PATH}/pico_dm_qd3503728.cmake)
# include(${PICO_DISPLAY_LIB_CONFIG_PATH}/pico_dm_qd3503728_8bit.cmake)
# include(${PICO_DISPLAY_LIB_CONFIG_PATH}/pico_dm_yt350s006.cmake)
# include(${PICO_DISPLAY_LIB_CONFIG_PATH}/generic-ili9341.cmake)
# include(${PICO_DISPLAY_LIB_CONFIG_PATH}/generic-st7789v.cmake)

# Touch sampling. The library's board config polls the controller every 33 ms
# (~30 Hz), which is coarse for a pointer: a drag visibly steps, and measured
# points arrive 32 ms apart. 10 ms (100 Hz) is in step with what the FT6236
# scans at by itself (PERIODACTIVE defaults to 12 ms); paired with the EP4
# endpoint's 8 ms bInterval the host collects ~120 reports per second.
# Overridden here rather than in the shared board config so the other boards
# using it keep their own value.
set(INDEV_POLLING_PERIOD_MS 10)

# Panel active area in mm, reported to the host so it can give the input device
# a resolution in units/mm -- libinput treats an absolute device without one as
# a kernel bug, and Mutter matches the device size against the outputs when it
# decides where a touchscreen belongs. 3.5" 480x320 module: 3.5 in = 88.9 mm
# diagonal at 3:2 -> about 74 x 49 mm of active area. This only scales the
# reported device size (and calibration), never a coordinate, so a rough value
# is harmless -- measure the glass if the exact number ever matters.
set(PUD_PANEL_WIDTH_MM  74)
set(PUD_PANEL_HEIGHT_MM 49)

# Run the board config's overclock profile 1: RP2350 at 225 MHz with a 75 MHz
# QSPI clock and the default 1.10V core voltage (the library's recommended,
# best tested profile -- see the table in
# lib/pico-display-lib/configs/pico_dm_qd3503728.cmake).
set(OVERCLOCK_ENABLED 1)

# Use the PIO DMA path for the I8080 TFT writes. It was disabled because the
# DMA DREQ interaction with the PIO appeared to stall under load; re-enabled to
# re-measure now that back-pressure on EP1 keeps the pipeline calmer.
set(PIO_USE_DMA 1)

# All the public source files of this project
set(PUD_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/main.c
    ${PUD_SOURCE_DIR}/pud.c
)

# Include config files if exist
include(${PUD_SOURCE_DIR}/decoders/config.cmake)

# Add libs or source files in subdirs
add_subdirectory(${PICO_DISPLAY_LIB_PATH})
add_subdirectory(${PUD_SOURCE_DIR}/cherryusb)
add_subdirectory(${PUD_SOURCE_DIR}/decoders)


# Decoder selection
# Note: This affects the decoding rate.
#     |  Name    |  Format |
#  0  |  tjpgd   |  jpeg   |
#  1  | JPEGDEC  |  jpeg   |
#  2  |  lz4     |  lz4    |
#  3  |  qoi     |  qoi    |
#  4  |  rle     |  rle    |
# The numbers are a protocol field: the device reports the selected decoder
# through PUD_CMD_GET_CAPS, so do not renumber them.
set(DECODER_TYPE 3)

# Decode / display-path timing counters (g_qoi_stat_* in src/decoders/decoder.c).
# Cost is two time_us_32() reads per flush batch, so it is off by default;
# enable it when you want to split a frame into QOI decode and panel write time:
#   cmake .. -DDECODER_STATS=1
set(DECODER_STATS 0 CACHE STRING "collect g_qoi_stat_* decode/display timings")

# Batch ping-pong for the QOI/RLE callback decoders: with a second accumulation
# buffer the next batch is decoded while the previous one is still going to the
# panel.  Off trades that overlap for 7680 bytes less RAM per decoder, which is
# how its value was measured (see notes/decoders.md):
#   cmake .. -DPUD_DECODER_PINGPONG=0
set(PUD_DECODER_PINGPONG 1 CACHE STRING "overlap QOI/RLE batch decode with the panel write")

# Run the QOI/RLE per-pixel decode loops from SRAM instead of XIP flash, using
# the codec libraries' RGB565_*_SECTION hook (the vendored files stay identical
# to their upstream repositories).  Measured on the RP2350: 6~9% off the
# device-side decode for ~2.3 KB of RAM, and nothing end to end while the USB
# link is the bottleneck -- details in notes/architecture.md:
#   cmake .. -DPUD_CODEC_IN_RAM=0
set(PUD_CODEC_IN_RAM 1 CACHE STRING "run the QOI/RLE decode loops from SRAM")

# QOI decode path.  The non-callback API decodes a whole rectangle into one
# band buffer and is ~40% faster per pixel than the streaming callback API; the
# band ping-pong then overlaps the panel write with the next decode.  It costs
# two band buffers (2 x (PUD_MAX_TRANSFER-16)/3 x 2 B = 87 KB at 64 KB
# transfers), so it is for RP2350 only -- RP2040 (256 KB SRAM) keeps the
# streaming path.  0 also forces the callback path on RP2350, which is what the
# oversized-rectangle fallback uses anyway.  Measurements in notes/decoders.md:
#   cmake .. -DQOI_NONCALLBACK=0
if(PICO_PLATFORM MATCHES "^rp2350" OR PICO_BOARD STREQUAL "pico2")
    set(_qoi_noncallback_default 2)
else()
    set(_qoi_noncallback_default 0)
endif()
set(QOI_NONCALLBACK ${_qoi_noncallback_default} CACHE STRING
    "QOI: 0=callback, 1=non-callback, 2=non-callback + band ping-pong")
# Batch height of the callback path (fallback for oversized rectangles).
set(QOI_BUF_ROWS 8 CACHE STRING "QOI callback-path accumulation buffer height in rows")

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
    ${PUD_SOURCES}
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(${PROJECT_NAME}
    pico_stdlib
    # pico_multicore
    pico_unique_id
    pico_bootsel_via_double_reset
    # hardware_i2c
    # hardware_spi
    # hardware_pwm
    hardware_clocks
    pico-display-lib
    cherryusb_device
    jpegdec
    tjpgd
    lz4
    qoi
    rle
    FreeRTOS-Kernel-Heap3
)

target_compile_definitions(${PROJECT_NAME} PUBLIC
    PUD_PANEL_WIDTH_MM=${PUD_PANEL_WIDTH_MM}
    PUD_PANEL_HEIGHT_MM=${PUD_PANEL_HEIGHT_MM})
target_compile_definitions(${PROJECT_NAME} PUBLIC DECODER_TYPE=${DECODER_TYPE})
target_compile_definitions(${PROJECT_NAME} PUBLIC DECODER_STATS=${DECODER_STATS})
target_compile_definitions(${PROJECT_NAME} PUBLIC PUD_DECODER_PINGPONG=${PUD_DECODER_PINGPONG})
target_compile_definitions(${PROJECT_NAME} PUBLIC QOI_BUF_ROWS=${QOI_BUF_ROWS})
target_compile_definitions(${PROJECT_NAME} PUBLIC QOI_NONCALLBACK=${QOI_NONCALLBACK})

pico_enable_stdio_usb(${PROJECT_NAME} 0)
pico_enable_stdio_uart(${PROJECT_NAME} 1)
pico_add_extra_outputs(${PROJECT_NAME})

# math(EXPR FLASH_CLK_KHZ "${FLASH_CLK_KHZ}")
# math(EXPR FLASH_CLK_MHZ "${FLASH_CLK_KHZ} / 1000")
# math(EXPR SYS_CLK_MHZ "${SYS_CLK_KHZ} / 1000")
math(EXPR TFT_BUS_CLK_MHZ "${TFT_BUS_CLK_KHZ} / 1000")
message(STATUS "
    CPU speed   : ${SYS_CLK_MHZ} MHz
    Flash speed : ${FLASH_CLK_MHZ} MHz
    BUS speed   : ${TFT_BUS_CLK_MHZ} MHz"
)

# show target size info
add_custom_target(
    print-memory-usage ALL
    COMMAND arm-none-eabi-size -G ${CMAKE_PROJECT_NAME}.elf
    DEPENDS ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf
    COMMENT "Print target size info"
    DEPENDS ${PROJECT_NAME}
)

# FIXME: Temporary manual operation:
# First, You need to add `set auto-load safe-path .` in your `~/.config/gdb/gdbinit`
# Then you have to run `openocd -f interface/cmsis-dap.cfg -f target/rp2350.cfg` etc.
# in another window to start the debugger. Finally, in the build directory, run
# `gdb-multiarch ./rp2040-freertos-template.elf` to attach gdb to the debugger.
add_custom_target(
    copy-gdbinit ALL
    COMMAND cp ${CMAKE_CURRENT_LIST_DIR}/.gdbinit ${CMAKE_BINARY_DIR}/
    DEPENDS ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf
    COMMENT "Copy .gdbinit to build directory"
    DEPENDS ${PROJECT_NAME}
)

# add a firmware flash target
if(${PICO_BOARD} STREQUAL "pico" OR ${PICO_PLATFORM} STREQUAL "rp2040")
        add_custom_target(
            flash
            COMMAND openocd -f interface/cmsis-dap.cfg -c "adapter speed 10000"
                            -f target/rp2040.cfg -s tcl -c "program ${CMAKE_PROJECT_NAME}.elf verify reset exit"
            DEPENDS ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf
            COMMENT "Flashing firmware using CMSIS-DAP Debugger..."
            DEPENDS ${PROJECT_NAME}
        )
elseif(${PICO_BOARD} STREQUAL  "pico2" OR ${PICO_PLATFORM} STREQUAL  "rp2350")
        add_custom_target(
            flash
            COMMAND openocd -f interface/cmsis-dap.cfg -c "adapter speed 10000"
                            -f target/rp2350.cfg -s tcl -c "program ${CMAKE_PROJECT_NAME}.elf verify reset exit"
            DEPENDS ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf
            COMMENT "Flashing firmware using CMSIS-DAP Debugger..."
            DEPENDS ${PROJECT_NAME}
        )
endif()
