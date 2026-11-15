/* tusb_config.h - TinyUSB set up as a USB host on the board's own USB-C port. */
#pragma once
#define CFG_TUSB_MCU OPT_MCU_RP2040
#define CFG_TUSB_OS OPT_OS_PICO
#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 0
#define BOARD_TUH_RHPORT 0
#define CFG_TUH_ENUMERATION_BUFSIZE 512
#define CFG_TUH_HUB 1
#define CFG_TUH_DEVICE_MAX 4
#define CFG_TUH_HID 0
#define CFG_TUH_CDC 0
#define CFG_TUH_MSC 0
#define CFG_TUH_VENDOR 0
