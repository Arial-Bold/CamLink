/* cam.h - the Logitech QuickCam for Notebooks (046d:08dd): starting and
 * stopping its video, and gathering the pictures it sends. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CAM_VID 0x046d
#define CAM_PID 0x08dd

/* Frames: the newest whole one is kept aside for the GameCube. */
#define CAM_FRAME_MAX 32768

typedef struct {
    uint16_t id;                /* 0 = none yet */
    uint32_t len;
    uint16_t w, h;
    const uint8_t *data;
} CamFrame;

/* (core 0) The camera was plugged in at USB address daddr, or unplugged. */
void cam_attach(uint8_t daddr);
void cam_detach(void);
/* (core 0) Starts or stops the video (the camera's light follows). Starting
 * takes a second or two of settings. Returns false if the camera didn't take
 * them; *fail_step says where. level 3 is the real thing; 1 (settings only)
 * and 2 (settings and the big-packet setting, nothing received) are for
 * finding out which part goes wrong. */
bool cam_start(int level, uint8_t *fail_step);
bool cam_started(void);
extern volatile uint8_t cam_progress;
void cam_stop(void);
bool cam_streaming(void);
/* (core 0) Now and then while streaming: eases the camera's data rate back if
 * it overflows. */
void cam_tick(void);

/* (core 1) Takes the newest whole frame, which then stays put until the next
 * call (the one before is free again). */
CamFrame cam_take_frame(void);

/* Counts, for the test program. */
uint32_t cam_packets(void);
uint32_t cam_frames(void);
uint32_t cam_dropped(void);
uint8_t cam_quality(void);
