/* link.h - how the GameCube and the CamLink adapter talk (shared by both).
 *
 * The adapter answers like a standard controller (so the console and games
 * are happy) and also understands a few commands of its own. Numbers in
 * replies are little-endian. */
#pragma once

#define LINK_MAGIC0 'C'
#define LINK_MAGIC1 'A'
#define LINK_MAGIC2 'M'
#define LINK_MAGIC3 '1'
#define LINK_VERSION 3

/* 0xE0: hello. Reply: 32 bytes, the info block below. */
#define LINK_CMD_HELLO 0xE0
/* 0xE1 n: page n (32 bytes) of the camera's configuration descriptor. */
#define LINK_CMD_DESC 0xE1
/* 0xE2 s: speed test. Reply: 64 bytes, byte 0 = s, byte i = (s + i) & 255
 * for 1..62, byte 63 = the sum of bytes 0..62 (mod 256). */
#define LINK_CMD_SPEED 0xE2
/* 0xE3 hi lo: piece n = hi * 256 + lo (128 bytes) of the frame taken with
 * 0xE4. */
#define LINK_CMD_CHUNK 0xE3
/* 0xE4: take the newest whole frame. Reply: 16 bytes, the frame block below.
 * The frame stays put (for 0xE3) until the next 0xE4. Asking keeps the
 * camera running: with no 0xE4 for 5 seconds it stops. */
#define LINK_CMD_FRAME 0xE4
/* 0xE5: start the camera (its light comes on). 0xE6: stop it. Reply: 1 byte,
 * the state. */
#define LINK_CMD_START 0xE5
#define LINK_CMD_STOP 0xE6
/* 0xE7 n: start the camera at test level n: 1 = its settings only, 2 = the
 * settings and its big-packet setting (nothing received), 3 = the same as
 * 0xE5. Reply: 1 byte, the state. */
#define LINK_CMD_START_LEVEL 0xE7

#define LINK_INFO_LEN 48
#define LINK_PAGE_LEN 32
#define LINK_SPEED_LEN 64
#define LINK_CHUNK_LEN 128
#define LINK_FRAME_LEN 16

/* States. */
enum {
    LINK_WAITING = 0,           /* no USB device */
    LINK_CAMERA_OK = 1,         /* the QuickCam, ready */
    LINK_CAMERA_NO_STREAM = 2,  /* the QuickCam, but no video stream found */
    LINK_OTHER_DEVICE = 3,      /* some other USB device */
    LINK_STARTING = 4,          /* setting the camera up */
    LINK_STREAMING = 5,         /* pictures coming */
    LINK_START_FAILED = 6       /* the camera didn't take its settings */
};

/* The info block (0xE0).
 * offset  size
 *  0      4   "CAM1"
 *  4      1   version
 *  5      1   state (above)
 *  6      2   USB vendor id of the device plugged in
 *  8      2   USB product id
 * 10      2   the camera's biggest video packet (bytes)
 * 12      1   1 if it has a microphone
 * 13      1   where starting failed (0 = it didn't), for diagnosis
 * 14      2   length of the configuration descriptor kept (for 0xE1)
 * 16      4   commands the adapter has answered
 * 20      4   video packets received from the camera
 * 24      4   whole frames received from the camera
 * 28      4   frames dropped (too big, or broken off)
 * 32      4   time since the adapter started (ms): if this goes back to a
 *             small number, the adapter restarted
 * 36      1   how far starting the camera got (1..6)
 * 37      1   the test level the camera was started at (0 = stopped)
 * 38      2   times the camera was unplugged (or dropped off USB)
 * 40      4   the chip's reset reasons at start-up (CHIP_RESET register)
 * 44      4   (0) */

/* The frame block (0xE4).
 * offset  size
 *  0      2   frame number (0 = no frame yet; it changes with each new one)
 *  2      4   length of the frame's JPEG data (bytes)
 *  6      2   width
 *  8      2   height
 * 10      1   JPEG quality (50, 75 or 87)
 * 11      1   state
 * 12      4   (0)
 *
 * The data is the camera's JPEG picture without its header: put a standard
 * baseline JPEG header in front (the JPEG standard's Annex K tables, scaled
 * to the quality; Y sampled 2x1, Cb and Cr 1x1) and it decodes. */
