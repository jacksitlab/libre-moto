# ESP-IDF v6 removed the legacy "driver" component (split into
# esp_driver_uart / esp_driver_ledc / ...). The uart/ledc/i2c headers keep
# their driver/... include paths, but the PROVIDING components must be on
# the requirements list. Resolve the driver-component set per IDF version
# BEFORE idf_component_register (list vars expand in the call).
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(LM_DRIVER_COMPONENTS esp_driver_uart esp_driver_ledc esp_driver_i2c esp_driver_gpio esp_driver_usb_serial_jtag)
else()
    set(LM_DRIVER_COMPONENTS driver esp_driver_usb_serial_jtag)
endif()

# ----------------------------------------------------------------------------
# Board selection: one subdirectory per board under hal/.
#   crowpanel — Elecrow CrowPanel 2.1" (ST7701 RGB, CST816, PCF8574)  [default]
#   waveshare — Waveshare ESP32-S3-Touch-AMOLED-1.75C (CO5300 QSPI, CST9217)
# Select with: idf.py build (default) or
#              idf.py -D LM_BOARD=waveshare build
#              (persistent: idf.py -D SDKCONFIG_DEFAULTS=... + menuconfig)
set(LM_BOARD "crowpanel" CACHE STRING "Target display board (crowpanel|waveshare)")
set_property(CACHE LM_BOARD PROPERTY STRINGS crowpanel waveshare)
message(STATUS "Libre-Moto board: ${LM_BOARD}")

set(LM_HAL_DIR "${CMAKE_CURRENT_LIST_DIR}/hal/${LM_BOARD}")

idf_component_register(
    SRCS
        "app_main.cpp"
        "ble_gatt.c"
        "serial_cmd.c"
        "splash.c"
        "${LM_HAL_DIR}/board.c"
        "${LM_HAL_DIR}/panel.c"
        "${LM_HAL_DIR}/touch.c"
    INCLUDE_DIRS
        "."
        "${LM_HAL_DIR}"
    PRIV_REQUIRES
        esp_lcd
        nvs_flash
        bt
        esp_timer
        lvgl
        ${LM_DRIVER_COMPONENTS}
)

# The CrowPanel board chain additionally needs the PCF8574 expander driver
# (backlight.c is compiled by every board contract; the crowpanel one uses
# LEDC, the waveshare one is a no-op).
if(LM_BOARD STREQUAL "crowpanel")
    target_sources(${COMPONENT_LIB} PRIVATE
        "${LM_HAL_DIR}/ioexp_pcf8574.c"
        "${LM_HAL_DIR}/backlight.c"
    )
endif()

# Shared protocol/render sources: single source of truth — include by
# relative path, do NOT copy. (nav_message.h, map_frame.h, map_render.h,
# map_reassembly.h, map_blit.h + impls)
target_include_directories(${COMPONENT_LIB} PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../../src/nav"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/map"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/ble"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libre-moto"
    "${CMAKE_CURRENT_LIST_DIR}/../../libraries"   # lv_conf.h (LV_CONF_INCLUDE_SIMPLE)
)

# The shared map_blit renderer + the LVGL-font text painter compile into
# this component directly (they are plain C/C++ with no Arduino deps;
# map_blit_lv.c needs lvgl which is a public dep of the lvgl component).
list(APPEND srcs_shared
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libre-moto/map_blit.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/libre-moto/map_blit_lv.c"
)
target_sources(${COMPONENT_LIB} PRIVATE ${srcs_shared})

# The generated asset arrays (icon/logo/maneuver/vehicle) also compile
# straight from the sketch directory. maneuver_icons.* contain a literal
# "/*" inside their header comment ("*.svg" glob) — legal C but tripped
# by IDF's -Werror=comment; relax that one warning for the generated files.
set(ASSET_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src/libre-moto")
set_source_files_properties(
    "${ASSET_DIR}/maneuver_icons.c"
    "${ASSET_DIR}/libremoto_icon.c"
    "${ASSET_DIR}/libremoto_logo.c"
    "${ASSET_DIR}/vehicle_icon.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-error=comment")
target_sources(${COMPONENT_LIB} PRIVATE
    "${ASSET_DIR}/libremoto_icon.c"
    "${ASSET_DIR}/libremoto_logo.c"
    "${ASSET_DIR}/maneuver_icons.c"
    "${ASSET_DIR}/vehicle_icon.c"
)

# -Wno-error=comment: the generated maneuver_icons.h carries a literal "/*"
# inside its header comment ("*.svg" glob) — legal C, tripped by -Werror=comment.
# -Wno-error=missing-field-initializers: GCC 15 (IDF v6 toolchain) enforces
# it; the shared header-only structs get new trailing fields across versions.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-format
    -Wno-error=comment
    -Wno-error=missing-field-initializers)
