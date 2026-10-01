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

# Keep clangd pointed at the compilation database of whichever build directory
# was configured last, without naming one anywhere.  clangd looks for
# compile_commands.json in the source tree itself (and in a "build"
# subdirectory), which is why build-pico2 -- the RP2350 target the firmware is
# flashed from -- needs a link at the top level to be found at all.  Only a link
# is ever replaced here: a real file is someone else's.
if(CMAKE_EXPORT_COMPILE_COMMANDS AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    set(PUD_CDB_LINK "${CMAKE_SOURCE_DIR}/compile_commands.json")
    if(IS_SYMLINK "${PUD_CDB_LINK}")
        file(REMOVE "${PUD_CDB_LINK}")
    endif()
    if(NOT EXISTS "${PUD_CDB_LINK}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
                        "${CMAKE_BINARY_DIR}/compile_commands.json"
                        "${PUD_CDB_LINK}")
    endif()
endif()

SET(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -Wl,--print-memory-usage")
SET(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -Wl,--print-memory-usage")

set(PICO_DISPLAY_LIB_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-display-lib)
# The library's own copies of the panel configs.  The build includes this
# repository's configs/ instead (see PUD_CONFIG below) -- same files, but a local
# panel tweak then stays here rather than going into the library.
set(PICO_DISPLAY_LIB_CONFIG_PATH ${PICO_DISPLAY_LIB_PATH}/configs)
set(CHERRY_USB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/CherryUSB)
set(LZ4_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lib/lz4)
set(PUD_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/src)

# set the project name
project(pico-usb-display C CXX ASM)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# **********  select the suitable config file  **********
#
# The panel config comes from this repository's own configs/ directory -- the same
# set the library ships, but a local tweak of one panel then stays here instead of
# going into the library.  `./build.sh lunch` records the choice in .pud-config and
# passes it as PUD_CONFIG; a plain `cmake ..` (or a checkout that has never run
# lunch) builds the default below.
set(PUD_CONFIG "pico_dm_qd3503728" CACHE STRING
    "panel config from configs/ (see ./build.sh configs)")
set(PUD_CONFIG_DIR ${CMAKE_CURRENT_LIST_DIR}/configs CACHE PATH
    "directory the panel configs are read from")
include(${PUD_CONFIG_DIR}/${PUD_CONFIG}.cmake)

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
# a kernel bug -- and so a compositor can tell which output the touchscreen
# belongs to.  Mutter compares the size the input device's (integer!) resolution
# implies against the output's EDID size, which EDID only stores in whole
# centimeters, and wants them within 5%.  70x40 is a pair where both work out:
# the resolution becomes 7 and 8 units/mm, which reads back as 68.6x40 mm, and
# the EDID says 7x4 cm.  The 3.5" glass is closer to 74x49; the reported number
# only affects the device size and DPI estimates, never a coordinate.
set(PUD_PANEL_WIDTH_MM  70)
set(PUD_PANEL_HEIGHT_MM 40)

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

# Run the QOI/RLE per-pixel decode loops from SRAM instead of XIP flash, using
# the codec libraries' RGB565_*_SECTION hook (the vendored files stay identical
# to their upstream repositories).  Measured on the RP2350: 6~9% off the
# device-side decode for ~2.3 KB of RAM, and nothing end to end while the USB
# link is the bottleneck -- details in notes/architecture.md:
#   cmake .. -DPUD_CODEC_IN_RAM=0
# It is read by the codec subdirectories below, so it has to be set before them:
# defined after, a fresh build directory's first configure saw it empty and
# silently left the loops in flash (only a second configure picked it up).
set(PUD_CODEC_IN_RAM 1 CACHE STRING "run the QOI/RLE decode loops from SRAM")

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
#  5  |  qoiz    |  qoi + raw deflate (experimental, notes/decoders.md) |
# The numbers are a protocol field: the device reports the selected decoder
# through PUD_CMD_GET_CAPS, so do not renumber them.  3 is what the driver
# speaks; a different one for an experiment:
#   cmake .. -DDECODER_TYPE=5
set(DECODER_TYPE 3 CACHE STRING "decoder the firmware is built for (protocol field)")

# Which inflate DECODER_TYPE 5 uses.  Both decode the same raw deflate stream, so
# this is not a protocol field -- the host cannot tell them apart:
#   tinfl       miniz 3.0.2 (src/decoders/miniz/), small and portable
#   libdeflate  libdeflate v1.26 (src/decoders/libdeflate/), a faster one-shot
#               decoder that wants the whole input and output at once -- which is
#               how the transfer arrives anyway
#   cmake .. -DDECODER_TYPE=5 -DPUD_INFLATE=libdeflate
set(PUD_INFLATE tinfl CACHE STRING "QOI+deflate inflate: tinfl or libdeflate")
set_property(CACHE PUD_INFLATE PROPERTY STRINGS tinfl libdeflate)
if(PUD_INFLATE STREQUAL "libdeflate")
    set(_pud_inflate 2)
elseif(PUD_INFLATE STREQUAL "tinfl")
    set(_pud_inflate 1)
else()
    message(FATAL_ERROR "PUD_INFLATE must be tinfl or libdeflate, not '${PUD_INFLATE}'")
endif()

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

# Dictionary window for DECODER_TYPE 6, one per frame slot, laid out
# [history][output] -- so a single band's QOI stream, and the band it is diffed
# against, must each fit in half of it.  Measured desktop content puts a band's
# QOI stream at ~11.7 KB (full screen 93893 B over 8 bands), so 32 KB holds it
# with margin; a photo band is several times that and is refused (counted, not
# truncated).  RP2040 has only ~100 KB spare, hence the smaller default:
#   cmake .. -DDECODER_TYPE=6 -DPUD_DELTA_WIN=16384
if(PICO_RP2040)
    set(_pud_delta_win 16384)
else()
    set(_pud_delta_win 32768)
endif()
set(PUD_DELTA_WIN ${_pud_delta_win} CACHE STRING
    "DECODER_TYPE 6 dictionary window bytes, per frame slot (history + output)")

# EP1 sink: accept image transfers, count them and throw them away -- no decode,
# no panel write.  It exists so the USB link can be measured on its own: with it,
# tests/ep1_out_speed_test.py reports what the link and this interrupt path can
# carry, and the same run against a normal build shows whether the decoder is in
# the critical path at all.  The panel keeps whatever was already on it.  Off in
# every real build:
#   cmake .. -DPUD_EP1_SINK=1
set(PUD_EP1_SINK 0 CACHE STRING "EP1: accept and discard image transfers")
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
set_property(TARGET ${PROJECT_NAME} PROPERTY LINKER_LANGUAGE C)

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
    miniz
    libdeflate
    tinyd
    FreeRTOS-Kernel-Heap3
)

target_compile_definitions(${PROJECT_NAME} PUBLIC
    PUD_PANEL_WIDTH_MM=${PUD_PANEL_WIDTH_MM}
    PUD_PANEL_HEIGHT_MM=${PUD_PANEL_HEIGHT_MM})
target_compile_definitions(${PROJECT_NAME} PUBLIC DECODER_TYPE=${DECODER_TYPE})
target_compile_definitions(${PROJECT_NAME} PUBLIC PUD_INFLATE=${_pud_inflate})
target_compile_definitions(${PROJECT_NAME} PUBLIC DECODER_STATS=${DECODER_STATS})
target_compile_definitions(${PROJECT_NAME} PUBLIC PUD_DECODER_PINGPONG=${PUD_DECODER_PINGPONG})
target_compile_definitions(${PROJECT_NAME} PUBLIC PUD_DELTA_WIN=${PUD_DELTA_WIN})
target_compile_definitions(${PROJECT_NAME} PUBLIC PUD_EP1_SINK=${PUD_EP1_SINK})
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
