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

# message(WARNING ${PICO_DISPLAY_LIB_CONFIG_PATH}/pico_dm_qd3503728.cmake)
include(${PICO_DISPLAY_LIB_CONFIG_PATH}/pico_dm_qd3503728.cmake)

# All the public source files of this project
set(PUD_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/main.c
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
set(DECODER_TYPE 1)

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
    pico_bootsel_via_double_reset
    # hardware_i2c
    # hardware_spi
    # hardware_pwm
    hardware_clocks
    pico-display-lib
    cherryusb_device
    jpegdec
    lz4
    FreeRTOS-Kernel-Heap3
)

target_compile_definitions(${PROJECT_NAME} PUBLIC DECODER_TYPE=${DECODER_TYPE})

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
