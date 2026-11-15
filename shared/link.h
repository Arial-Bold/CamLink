/* link.h - how the GameCube and the adapter talk (shared by both sides).
 *
 * The adapter answers like a standard controller (so the console and games
 * are happy) and also understands a few commands of its own. Numbers in
 * replies are little-endian. */
#pragma once

#define LINK_MAGIC0 'C'
#define LINK_MAGIC1 'A'
#define LINK_MAGIC2 'M'
#define LINK_MAGIC3 '1'
#define LINK_VERSION 1

/* 0xE0: hello. Reply: 32 bytes, the info block below. */
#define LINK_CMD_HELLO 0xE0
/* 0xE1 n: page n (32 bytes) of the camera's configuration descriptor. */
#define LINK_CMD_DESC 0xE1
/* 0xE2 s: speed test. Reply: 64 bytes, byte 0 = s, byte i = (s + i) & 255
 * for 1..62, byte 63 = the sum of bytes 0..62 (mod 256). */
#define LINK_CMD_SPEED 0xE2

#define LINK_INFO_LEN 32
#define LINK_PAGE_LEN 32
#define LINK_SPEED_LEN 64

/* The info block. */
enum { LINK_WAITING = 0, LINK_CAMERA_OK = 1, LINK_CAMERA_NO_STREAM = 2, LINK_OTHER_DEVICE = 3 };
/* offset  size
 *  0      4   "CAM1"
 *  4      1   version
 *  5      1   state (above)
 *  6      2   USB vendor id of the device plugged in
 *  8      2   USB product id
 * 10      2   the camera's biggest video packet (bytes)
 * 12      1   1 if it has a microphone
 * 13      1   (0)
 * 14      2   length of the configuration descriptor kept (for 0xE1)
 * 16      4   commands the adapter has answered
 * 20      4   speed-test replies sent */
