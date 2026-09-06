cmake_minimum_required(VERSION 3.21)

# Must be set before project()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-ch59x.cmake)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(CH592F C ASM)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/BinaryKeyboardFirmware.cmake)

set(TARGET_ELF ${PROJECT_NAME}.elf)

bk_add_version_header(
    CHIP CH592F
    TARGET_NAME ch592f_version_header
    GENERATED_DIR_VAR GENERATED_DIR
    HEADER_VAR BK_VERSION_HEADER
)

# ---------------------------------------------------------------------------
# Keyboard selection and device naming (used by both USB and BLE)
# ---------------------------------------------------------------------------
set(KEYBOARD "5KEY" CACHE STRING "Keyboard (5KEY or KNOB)")
set_property(CACHE KEYBOARD PROPERTY STRINGS 5KEY KNOB)
set(KBD_NAME_PREFIX "BinaryKeyboard" CACHE STRING "Keyboard name prefix")
set(KBD_MODEL "AUTO" CACHE STRING "Keyboard model suffix for device name (AUTO uses KEYBOARD)")
set(KBD_DEVICE_NAME_OVERRIDE "" CACHE STRING "Override full USB/BLE device name, empty means auto compose")
option(KBD_SLEEP_TEST_MODE "Enable visible idle/sleep timing indicators" OFF)
string(TOUPPER "${KEYBOARD}" KEYBOARD_UPPER)
string(TOUPPER "${KBD_MODEL}" KBD_MODEL_UPPER_RAW)

if(KEYBOARD_UPPER STREQUAL "5KEY")
    set(KBD_LAYOUT_DEFINE "KBD_LAYOUT_5KEY")
elseif(KEYBOARD_UPPER STREQUAL "KNOB")
    set(KBD_LAYOUT_DEFINE "KBD_LAYOUT_KNOB")
else()
    message(FATAL_ERROR "Unsupported KEYBOARD='${KEYBOARD}'. Expected 5KEY or KNOB.")
endif()

if(KBD_MODEL_UPPER_RAW STREQUAL "" OR KBD_MODEL_UPPER_RAW STREQUAL "AUTO")
    set(KBD_MODEL_UPPER "${KEYBOARD_UPPER}")
else()
    set(KBD_MODEL_UPPER "${KBD_MODEL_UPPER_RAW}")
endif()

if(KBD_DEVICE_NAME_OVERRIDE STREQUAL "")
    set(KBD_DEVICE_NAME "${KBD_NAME_PREFIX}${KBD_MODEL_UPPER}")
else()
    set(KBD_DEVICE_NAME "${KBD_DEVICE_NAME_OVERRIDE}")
endif()

string(LENGTH "${KBD_DEVICE_NAME}" KBD_DEVICE_NAME_LEN)

message(STATUS "Keyboard: ${KEYBOARD_UPPER}")
message(STATUS "Keyboard model: ${KBD_MODEL_UPPER}")
message(STATUS "Unified device name: ${KBD_DEVICE_NAME}")
message(STATUS "Unified device name length: ${KBD_DEVICE_NAME_LEN}")

if(KBD_DEVICE_NAME_LEN GREATER 31)
    message(WARNING "Device name length > 31, USB product string will be truncated at runtime.")
endif()

if(KBD_DEVICE_NAME_LEN GREATER 29)
    message(WARNING "Device name length > 29, BLE scan response local name will be shortened.")
elseif(KBD_DEVICE_NAME_LEN GREATER 21)
    message(WARNING "Device name length > 21, BLE GATT Device Name will be truncated.")
endif()

# ---------------------------------------------------------------------------
# Source files
# Excluded (per CH592F.wvproj):
#   StdPeriphDriver/CH59x_usbhostClass.c  CH59x_usbhostBase.c
#   StdPeriphDriver/CH59x_uart2.c  CH59x_uart3.c  CH59x_spi0.c  CH59x_pwm.c
#   StdPeriphDriver/CH59x_i2c.c (project unused, avoid vendor warning noise)
#   StdPeriphDriver/CH59x_flash.c CH59x_lcd.c CH59x_timer2.c CH59x_timer3.c
#   StdPeriphDriver/CH59x_uart0.c CH59x_uart1.c (unused in current build; APIs resolved elsewhere / not enabled)
#   ble/core/src/KEY.c  LED.c
#   app/Main_Test.c  app/RGB_TEST.c
#   RVMSIS/core_riscv.c (current firmware only uses header inlines/macros)
# ---------------------------------------------------------------------------
set(CH592_ENTRY_SOURCES
    # Entry point
    app/Main.c
)

set(CH592_BOARD_SOURCES
    # HAL
    hal/src/debug.c
    hal/src/encoder.c
    hal/src/hal_utils.c
    hal/src/kbd_battery.c
    hal/src/key.c
    hal/src/ws2812.c
)

set(CH592_BLE_CORE_SOURCES
    # BLE – core
    ble/core/src/ble_mcu.c
    ble/core/src/ble_rtc.c
    ble/core/src/ble_sleep.c
)

set(CH592_BLE_HID_SOURCES
    # BLE – HID
    ble/hid/src/ble_hid.c
    ble/hid/src/ble_hid_service.c
    ble/hid/src/kbd_mode.c
)

set(CH592_BLE_PROFILE_SOURCES
    # BLE – GATT profiles
    ble/profile/src/battservice.c
    ble/profile/src/devinfoservice.c
    ble/profile/src/hiddev.c
    ble/profile/src/scanparamservice.c
)

set(CH592_KEYBOARD_SOURCES
    # Keyboard
    keyboard/src/kbd_command.c
    keyboard/src/kbd_core.c
    keyboard/src/kbd_iap.c
    keyboard/src/kbd_macro.c
    keyboard/src/kbd_log.c
    keyboard/src/kbd_rgb.c
    keyboard/src/kbd_storage.c
)

set(CH592_USB_SOURCES
    # USB
    usb/src/usb_descriptors.c
    usb/src/usb_device.c
    usb/src/usb_hid.c
)

set(CH592_VENDOR_DRIVER_SOURCES
    # WCH Standard Peripheral Driver
    SDK/StdPeriphDriver/CH59x_adc.c
    SDK/StdPeriphDriver/CH59x_clk.c
    SDK/StdPeriphDriver/CH59x_gpio.c
    SDK/StdPeriphDriver/CH59x_pwr.c
    SDK/StdPeriphDriver/CH59x_sys.c
    SDK/StdPeriphDriver/CH59x_timer0.c
    SDK/StdPeriphDriver/CH59x_timer1.c
    SDK/StdPeriphDriver/CH59x_uart1.c
    SDK/StdPeriphDriver/CH59x_usbdev.c
)

set(CH592_STARTUP_SOURCES
    # Startup (assembly)
    SDK/Startup/startup_CH592.S
    SDK/Startup/ble_task_scheduler.S
)

set(CH592_SOURCES
    ${CH592_ENTRY_SOURCES}
    ${CH592_BOARD_SOURCES}
    ${CH592_BLE_CORE_SOURCES}
    ${CH592_BLE_HID_SOURCES}
    ${CH592_BLE_PROFILE_SOURCES}
    ${CH592_KEYBOARD_SOURCES}
    ${CH592_USB_SOURCES}
    ${CH592_VENDOR_DRIVER_SOURCES}
    ${CH592_STARTUP_SOURCES}
)

add_executable(${TARGET_ELF} ${CH592_SOURCES})
add_dependencies(${TARGET_ELF} ch592f_version_header)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CH592_SOURCES})

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
set(CH592_INCLUDE_DIRS
    ${GENERATED_DIR}
    SDK/StdPeriphDriver/inc
    SDK/RVMSIS
    iap
    hal/include
    usb/include
    ble/lib
    ble/hid/include
    ble/profile/include
    ble/core/include
    keyboard/include
)

target_include_directories(${TARGET_ELF} PRIVATE ${CH592_INCLUDE_DIRS})

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------
set(CH592_COMPILE_DEFINITIONS
    ${KBD_LAYOUT_DEFINE}
    KBD_MODEL_NAME=\"${KBD_MODEL_UPPER}\"
    KBD_DEVICE_NAME=\"${KBD_DEVICE_NAME}\"
    HAL_SLEEP=TRUE
    DCDC_ENABLE=TRUE
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND CH592_COMPILE_DEFINITIONS
        DEBUG=Debug_UART1
        UART_LOG_ENABLE=1
        KBD_DEBUG_BUILD=1
        KBD_USB_LOG_ENABLE=1
    )
else()
    list(APPEND CH592_COMPILE_DEFINITIONS
        UART_LOG_ENABLE=0
        KBD_DEBUG_BUILD=0
        KBD_USB_LOG_ENABLE=0
        KBD_LOG_DEFAULT_ENABLED=0
    )
endif()

if(KBD_SLEEP_TEST_MODE)
    list(APPEND CH592_COMPILE_DEFINITIONS
        KBD_SLEEP_TEST_MODE=1
    )
endif()

set(CH592_C_COMPILE_OPTIONS
    -std=gnu99
    -Wall
    -Wunused
    -Wuninitialized
)

target_compile_definitions(${TARGET_ELF} PRIVATE ${CH592_COMPILE_DEFINITIONS})
target_compile_options(${TARGET_ELF} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${CH592_C_COMPILE_OPTIONS}>
)

# ---------------------------------------------------------------------------
# Linker options
# ---------------------------------------------------------------------------
set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/SDK/Ld/Link.ld)

target_link_options(${TARGET_ELF} PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.map
)

# Pre-compiled vendor libraries
target_link_libraries(${TARGET_ELF} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/SDK/StdPeriphDriver/libISP592.a
    ${CMAKE_CURRENT_SOURCE_DIR}/ble/lib/libCH59xBLE.a
)

# ---------------------------------------------------------------------------
# Post-build: generate .bin and .hex, print size
# ---------------------------------------------------------------------------
add_custom_command(TARGET ${TARGET_ELF} POST_BUILD
    COMMAND ${CROSS_OBJCOPY} -Obinary $<TARGET_FILE:${TARGET_ELF}>
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.bin
    COMMENT "Generating ${PROJECT_NAME}.bin"
    VERBATIM)

add_custom_command(TARGET ${TARGET_ELF} POST_BUILD
    COMMAND ${CROSS_OBJCOPY} -Oihex $<TARGET_FILE:${TARGET_ELF}>
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.hex
    COMMENT "Generating ${PROJECT_NAME}.hex"
    VERBATIM)

add_custom_command(TARGET ${TARGET_ELF} POST_BUILD
    COMMAND ${CROSS_SIZE} --format=berkeley $<TARGET_FILE:${TARGET_ELF}>
    COMMENT "Firmware size:"
    VERBATIM)
