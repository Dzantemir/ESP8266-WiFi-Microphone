# ESP8266 RTOS SDK v3.4 project for esp8266-wifi-microphone.
#
# Top-level CMakeLists.txt — minimal project() + inclusion of the ESP-IDF
# build machinery. All component wiring happens via main/CMakeLists.txt.
#
# The project streams audio captured from an INMP441 I2S MEMS microphone,
# encodes it with IMA ADPCM (DVI4 / RFC 3551), and sends it over UDP to a
# companion server (EasspServerPB). A service UDP port (EASSP protocol,
# default 3950) handles discovery and streaming control.
#
# Build: idf.py build
# Flash: idf.py flash
#
# IMPORTANT: this project depends on a patched components/esp8266/driver/i2s.c
# that enables the BBPLL audio clock and uses /48 for 24-bit frame divider.
# Drop the patched i2s.c into the SDK tree (or use EXTRA_COMPONENT_DIRS).

cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp8266-wifi-microphone)
