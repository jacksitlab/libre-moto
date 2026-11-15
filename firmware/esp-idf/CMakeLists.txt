# Libre-Moto firmware — ESP-IDF build
#
# Native ESP-IDF port of the Arduino sketch (firmware/src/libre-moto).
# Build:  idf.py set-target esp32s3 && idf.py build
# Flash:  idf.py -p PORT flash monitor
#
# The Arduino sketch stays the reference build; this project shares the
# protocol/render sources via relative includes (../src/nav, ../src/map,
# ../src/ble) — single source of truth, no copies.

cmake_minimum_required(VERSION 3.16)

# Vendored LVGL 9.1.0 (firmware/libraries/lvgl) as an ESP-IDF component —
# the same library the Arduino build compiles. Its esp.cmake defines
# LV_CONF_INCLUDE_SIMPLE, so lv_conf.h (firmware/libraries/lv_conf.h)
# must be reachable via the main component's include dirs.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../libraries/lvgl")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(libre-moto-esp-idf)
