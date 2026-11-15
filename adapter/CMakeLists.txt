# CamLink: the firmware for the USB-to-GameCube adapter (RP2040-Zero).
# Build:  mkdir build && cd build
#         cmake -DPICO_SDK_PATH=/path/to/pico-sdk -DPICO_BOARD=waveshare_rp2040_zero ..
#         make
cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

# TinyUSB (the USB host library) can't receive a webcam's video stream, so
# CamLink does that itself on one of the USB controller's spare endpoints.
# TinyUSB stops with "Unhandled buffer" when that endpoint finishes a packet,
# so the build uses a copy of the SDK's TinyUSB with one hook added that hands
# those packets to CamLink first (cam.c, camlink_usb_buffers).
set(TU_COPY ${CMAKE_BINARY_DIR}/tinyusb)
set(TU_HCD ${TU_COPY}/src/portable/raspberrypi/rp2040/hcd_rp2040.c)
if (NOT EXISTS ${TU_HCD})
    file(COPY ${PICO_SDK_PATH}/lib/tinyusb/ DESTINATION ${TU_COPY})
    file(READ ${TU_HCD} HCD_SRC)
    set(HOOK_FROM "  if ( remaining_buffers )\n  {\n    panic(")
    set(HOOK_TO "  extern uint32_t camlink_usb_buffers(uint32_t remaining);\n  if ( remaining_buffers ) remaining_buffers = camlink_usb_buffers(remaining_buffers);\n  if ( remaining_buffers )\n  {\n    panic(")
    string(FIND "${HCD_SRC}" "${HOOK_FROM}" HOOK_AT)
    if (HOOK_AT EQUAL -1)
        message(FATAL_ERROR "This TinyUSB version doesn't match CamLink's hook (use Pico SDK 2.2.0)")
    endif()
    string(REPLACE "${HOOK_FROM}" "${HOOK_TO}" HCD_SRC "${HCD_SRC}")
    file(WRITE ${TU_HCD} "${HCD_SRC}")
endif()
set(PICO_TINYUSB_PATH ${TU_COPY})

project(camlink C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(camlink main.c cam.c joybus-pio/src/joybus.c)
pico_generate_pio_header(camlink ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)
target_include_directories(camlink PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/../shared ${CMAKE_CURRENT_LIST_DIR}/joybus-pio/include)
# TinyUSB may use interrupt endpoints 1-14; CamLink keeps 15 for the video
target_compile_definitions(camlink PRIVATE PICO_USB_HOST_INTERRUPT_ENDPOINTS=14)
target_link_libraries(camlink pico_stdlib pico_multicore tinyusb_host tinyusb_board hardware_pio hardware_sync)
pico_add_extra_outputs(camlink)
