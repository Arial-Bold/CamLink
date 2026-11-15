# camlink: the firmware for the USB-to-GameCube adapter (RP2040-Zero).
# Build:  mkdir build && cd build && cmake -DPICO_SDK_PATH=/path/to/pico-sdk -DPICO_BOARD=waveshare_rp2040_zero .. && make
cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(camlink C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()
add_executable(camlink main.c joybus-pio/src/joybus.c)
pico_generate_pio_header(camlink ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)
target_include_directories(camlink PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../shared ${CMAKE_CURRENT_LIST_DIR}/joybus-pio/include)
target_link_libraries(camlink pico_stdlib pico_multicore tinyusb_host tinyusb_board hardware_pio)
pico_add_extra_outputs(camlink)
