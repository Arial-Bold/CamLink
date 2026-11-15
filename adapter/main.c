/* CamLink - the adapter firmware for CamLink (video calls on the GameCube).
 *
 * Runs on the USB-to-GameCube adapter (an RP2040-Zero). Core 0 looks after
 * the USB side: it finds the webcam plugged into the adapter and, when the
 * GameCube asks, starts its video and gathers the pictures (cam.c). Core 1
 * talks to the GameCube on the controller port's data wire (GP7): it answers
 * like a standard controller, and also answers CamLink's own commands
 * (link.h): who it is, what the camera reported, a speed test, and the
 * pictures themselves.
 *
 * The board's LED:
 *   slow white blink        no USB device
 *   green, steady           the QuickCam, ready (a blue flash too if its
 *                           microphone is there)
 *   green, blinking         the QuickCam, but no video stream found
 *   yellow, double blink    some other USB device
 *   cyan, blinking          starting the camera
 *   cyan, steady            pictures coming
 *   red, blinking           the camera didn't take its settings
 *
 * To go back to the adapter's controller firmware, flash the usb2gc
 * RP2040-Zero .uf2 from github.com/joypad-ai/joypad-os/releases.
 *
 * License: GPL-3.0-or-later (the camera settings come from Linux, see
 * cam.c). The Joybus code is joybus-pio (LGPL-3.0, joybus-pio/LICENSE). */
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/structs/vreg_and_chip_reset.h"
#include "hardware/pio.h"
#include "tusb.h"
#include "ws2812.pio.h"
#include "joybus.h"
#include "link.h"
#include "cam.h"

#define LED_PIN 16
#define GC_DATA_PIN 7
#define IDLE_STOP_US 5000000      /* stop the camera after 5 s with no frame asked for */

/* What the USB side found (written by core 0, read by core 1). */
static volatile uint8_t state = LINK_WAITING;
static volatile uint8_t fail_step;
static volatile uint16_t dev_vid, dev_pid, max_iso, desc_len;
static volatile uint8_t has_mic;
static uint8_t desc_buf[512];
static volatile uint32_t cmd_count;
static volatile uint8_t cam_daddr;

/* Asked by the GameCube (core 1), done by core 0. */
static volatile bool want_start, want_stop;
static volatile uint8_t want_level = 3;
static volatile uint16_t unplugs;
static uint32_t reset_reason;
static volatile uint32_t last_frame_ask;

/* ------------------------------------------------------------ the LED */

static PIO led_pio = pio0;
static uint led_sm;
static void led(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t grb = ((uint32_t)(g / 4) << 16) | ((uint32_t)(r / 4) << 8) | (b / 4);
    pio_sm_put_blocking(led_pio, led_sm, grb << 8u);
}

static void show_state(uint32_t ms) {
    uint32_t t = ms % 2000;
    switch (state) {
    case LINK_WAITING:
        if (t < 150) led(120, 120, 120); else led(0, 0, 0);
        break;
    case LINK_CAMERA_OK:
        if (has_mic && t >= 1700 && t < 1850) led(0, 0, 255); else led(0, 255, 0);
        break;
    case LINK_CAMERA_NO_STREAM:
        if ((t / 250) % 2 == 0) led(0, 255, 0); else led(0, 0, 0);
        break;
    case LINK_STARTING:
        if ((t / 125) % 2 == 0) led(0, 200, 255); else led(0, 0, 0);
        break;
    case LINK_STREAMING:
        led(0, 200, 255);
        break;
    case LINK_START_FAILED:
        if ((t / 250) % 2 == 0) led(255, 0, 0); else led(0, 0, 0);
        break;
    default:
        if (t < 150 || (t >= 300 && t < 450)) led(255, 180, 0); else led(0, 0, 0);
        break;
    }
}

/* ------------------------------------------------------------ USB (core 0) */

static uint8_t cfg_buf[512];
static tusb_desc_device_t dev_desc;

static void read_config(const uint8_t *p, uint16_t len, bool *video, bool *mic, uint16_t *biggest) {
    *video = false;
    *mic = false;
    *biggest = 0;
    uint8_t cur_class = 0;
    for (uint16_t i = 0; i + 2 <= len && p[i] >= 2;) {
        uint8_t dlen = p[i], type = p[i + 1];
        if (i + dlen > len) break;
        if (type == TUSB_DESC_INTERFACE && dlen >= 9) {
            cur_class = p[i + 5];
            if (cur_class == TUSB_CLASS_AUDIO) *mic = true;
        } else if (type == TUSB_DESC_ENDPOINT && dlen >= 7) {
            uint8_t addr = p[i + 2], attr = p[i + 3];
            uint16_t mps = (uint16_t)(p[i + 4] | (p[i + 5] << 8)) & 0x7ff;
            if ((attr & 3) == TUSB_XFER_ISOCHRONOUS && (addr & 0x80) && cur_class != TUSB_CLASS_AUDIO) {
                *video = true;
                if (mps > *biggest) *biggest = mps;
            }
        }
        i += dlen;
    }
}

static void got_config(tuh_xfer_t *xfer) {
    bool video = false, mic = false;
    uint16_t biggest = 0, total = 0;
    if (xfer->result == XFER_RESULT_SUCCESS) {
        const tusb_desc_configuration_t *c = (const tusb_desc_configuration_t *)cfg_buf;
        total = c->wTotalLength < sizeof(cfg_buf) ? c->wTotalLength : sizeof(cfg_buf);
        read_config(cfg_buf, total, &video, &mic, &biggest);
        memcpy(desc_buf, cfg_buf, total);
    }
    desc_len = total;
    max_iso = biggest;
    has_mic = mic;
    if (dev_vid == CAM_VID && dev_pid == CAM_PID) {
        state = video ? LINK_CAMERA_OK : LINK_CAMERA_NO_STREAM;
        if (video) { cam_attach(xfer->daddr); cam_daddr = xfer->daddr; }
    } else {
        state = LINK_OTHER_DEVICE;
    }
}

static void got_device(tuh_xfer_t *xfer) {
    if (xfer->result != XFER_RESULT_SUCCESS) { state = LINK_OTHER_DEVICE; return; }
    dev_vid = dev_desc.idVendor;
    dev_pid = dev_desc.idProduct;
    if (!(dev_vid == CAM_VID && dev_pid == CAM_PID)) state = LINK_OTHER_DEVICE;
    /* (its configuration is kept either way, for the test program to show) */
    tuh_descriptor_get_configuration(xfer->daddr, 0, cfg_buf, sizeof(cfg_buf), got_config, 0);
}

void tuh_mount_cb(uint8_t daddr) {
    tuh_descriptor_get_device(daddr, &dev_desc, sizeof(dev_desc), got_device, 0);
}

void tuh_umount_cb(uint8_t daddr) {
    if (daddr == cam_daddr) { cam_detach(); cam_daddr = 0; unplugs++; }
    state = LINK_WAITING;
    has_mic = 0;
    max_iso = 0;
    desc_len = 0;
    dev_vid = dev_pid = 0;
}

/* ------------------------------------------------------------ the GameCube (core 1) */

#define GC_BIT_US 5
#define GC_TIMEOUT_US (GC_BIT_US * 10)
#define GC_REPLY_DELAY_US (GC_BIT_US - 1)
#define GC_RESET_WAIT_US ((GC_BIT_US * 8) * 2 + GC_TIMEOUT_US)

static joybus_port_t gc;
static uint8_t gc_status[3] = { 0x09, 0x00, 0x03 };     /* a standard controller */
/* nothing pressed, sticks centred */
static uint8_t gc_origin[10] = { 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00 };
/* "Nothing pressed, sticks centred", in each of the console's reading modes.
 * The mode is the byte after a poll command, and it changes how bytes 4-7
 * are packed (joybus-pio's gamecube_definitions.h): sending the mode 3
 * layout to a program that reads in mode 1 or 2 turns the centred C-stick
 * into one pushed hard left, and an idle R into one half pressed. */
static uint8_t gc_report_mode[8][8] = {
    { 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 },   /* 0: L,R and A,B as nibbles */
    { 0x00, 0x80, 0x80, 0x80, 0x88, 0x00, 0x00, 0x00 },   /* 1: C-stick as nibbles */
    { 0x00, 0x80, 0x80, 0x80, 0x88, 0x00, 0x00, 0x00 },   /* 2: C-stick and L,R as nibbles */
    { 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 },   /* 3: the usual */
    { 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 },   /* 4: A,B instead of L,R */
    { 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 },   /* 5-7: as 0 */
    { 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 },
    { 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 },
};

static void __not_in_flash_func(reply)(uint8_t *data, uint len) {
    busy_wait_us(GC_REPLY_DELAY_US);
    joybus_send_bytes(&gc, data, len);
    cmd_count++;
}

static void __not_in_flash_func(put32)(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

static void __not_in_flash_func(make_info)(uint8_t *b) {
    memset(b, 0, LINK_INFO_LEN);
    b[0] = LINK_MAGIC0; b[1] = LINK_MAGIC1; b[2] = LINK_MAGIC2; b[3] = LINK_MAGIC3;
    b[4] = LINK_VERSION;
    b[5] = state;
    b[6] = dev_vid & 255; b[7] = dev_vid >> 8;
    b[8] = dev_pid & 255; b[9] = dev_pid >> 8;
    b[10] = max_iso & 255; b[11] = max_iso >> 8;
    b[12] = has_mic;
    b[13] = fail_step;
    b[14] = desc_len & 255; b[15] = desc_len >> 8;
    put32(b + 16, cmd_count);
    put32(b + 20, cam_packets());
    put32(b + 24, cam_frames());
    put32(b + 28, cam_dropped());
    put32(b + 32, to_ms_since_boot(get_absolute_time()));
    b[36] = cam_progress;
    b[37] = cam_started() ? want_level : 0;
    b[38] = unplugs & 255; b[39] = unplugs >> 8;
    put32(b + 40, reset_reason);
}

static CamFrame sending;   /* the frame the GameCube is reading */

static void __not_in_flash_func(gc_main)(void) {
    joybus_port_init(&gc, GC_DATA_PIN, pio1, -1, -1);
    static uint8_t out[LINK_CHUNK_LEN];
    for (;;) {
        uint8_t cmd[3];
        joybus_receive_bytes(&gc, cmd, 1, GC_TIMEOUT_US, false);
        switch (cmd[0]) {
        case 0x00:                               /* probe */
        case 0xFF:                               /* reset */
            reply(gc_status, sizeof(gc_status));
            break;
        case 0x41:                               /* origin */
            reply(gc_origin, sizeof(gc_origin));
            break;
        case 0x42:                               /* recalibrate */
            joybus_receive_bytes(&gc, cmd + 1, 2, GC_TIMEOUT_US, true);
            reply(gc_origin, sizeof(gc_origin));
            break;
        case 0x40:                               /* poll */
            if (joybus_receive_bytes(&gc, cmd + 1, 2, GC_TIMEOUT_US, true) == 2) reply(gc_report_mode[cmd[1] & 7], 8);
            else { busy_wait_us(GC_RESET_WAIT_US); joybus_port_reset(&gc); }
            break;
        case 0x1D: {                             /* game id: listen, no reply */
            uint8_t id[10];
            joybus_receive_bytes(&gc, id, 10, GC_TIMEOUT_US, true);
            break;
        }
        case LINK_CMD_HELLO:
            make_info(out);
            reply(out, LINK_INFO_LEN);
            break;
        case LINK_CMD_DESC: {
            if (joybus_receive_bytes(&gc, cmd + 1, 1, GC_TIMEOUT_US, true) != 1) break;
            uint off = (uint)cmd[1] * LINK_PAGE_LEN;
            memset(out, 0, LINK_PAGE_LEN);
            if (off < desc_len) memcpy(out, desc_buf + off, desc_len - off < LINK_PAGE_LEN ? desc_len - off : LINK_PAGE_LEN);
            reply(out, LINK_PAGE_LEN);
            break;
        }
        case LINK_CMD_SPEED: {
            if (joybus_receive_bytes(&gc, cmd + 1, 1, GC_TIMEOUT_US, true) != 1) break;
            uint8_t s = cmd[1], sum = 0;
            for (int i = 0; i < LINK_SPEED_LEN - 1; i++) { out[i] = (uint8_t)(s + i); sum += out[i]; }
            out[LINK_SPEED_LEN - 1] = sum;
            reply(out, LINK_SPEED_LEN);
            break;
        }
        case LINK_CMD_FRAME:
            last_frame_ask = time_us_32();
            sending = cam_take_frame();
            memset(out, 0, LINK_FRAME_LEN);
            out[0] = sending.id & 255; out[1] = sending.id >> 8;
            put32(out + 2, sending.len);
            out[6] = sending.w & 255; out[7] = sending.w >> 8;
            out[8] = sending.h & 255; out[9] = sending.h >> 8;
            out[10] = cam_quality();
            out[11] = state;
            reply(out, LINK_FRAME_LEN);
            break;
        case LINK_CMD_CHUNK: {
            if (joybus_receive_bytes(&gc, cmd + 1, 2, GC_TIMEOUT_US, true) != 2) break;
            uint32_t off = ((uint32_t)cmd[1] << 8 | cmd[2]) * LINK_CHUNK_LEN;
            memset(out, 0, LINK_CHUNK_LEN);
            if (sending.data && off < sending.len)
                memcpy(out, sending.data + off, sending.len - off < LINK_CHUNK_LEN ? sending.len - off : LINK_CHUNK_LEN);
            reply(out, LINK_CHUNK_LEN);
            break;
        }
        case LINK_CMD_START_LEVEL:
            if (joybus_receive_bytes(&gc, cmd + 1, 1, GC_TIMEOUT_US, true) != 1) break;
            last_frame_ask = time_us_32();
            want_level = cmd[1] >= 1 && cmd[1] <= 3 ? cmd[1] : 3;
            want_start = true;
            out[0] = state;
            reply(out, 1);
            break;
        case LINK_CMD_START:
            last_frame_ask = time_us_32();
            want_level = 3;
            want_start = true;
            out[0] = state;
            reply(out, 1);
            break;
        case LINK_CMD_STOP:
            want_stop = true;
            out[0] = state;
            reply(out, 1);
            break;
        default:
            busy_wait_us(GC_RESET_WAIT_US);
            joybus_port_reset(&gc);
            break;
        }
    }
}

/* ------------------------------------------------------------ main (core 0) */

int main(void) {
    reset_reason = vreg_and_chip_reset_hw->chip_reset;
    uint offset = pio_add_program(led_pio, &ws2812_program);
    led_sm = pio_claim_unused_sm(led_pio, true);
    ws2812_program_init(led_pio, led_sm, offset, LED_PIN, 800000);
    led(0, 0, 0);
    multicore_launch_core1(gc_main);
    tuh_init(BOARD_TUH_RHPORT);
    uint32_t last = 0;
    for (;;) {
        tuh_task();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last >= 20) { last = now; show_state(now); }

        /* the GameCube's requests */
        if (want_stop) {
            want_stop = false;
            want_start = false;
            cam_stop();
            if (cam_daddr) state = LINK_CAMERA_OK;
        }
        if (want_start) {
            want_start = false;
            if (cam_daddr && !cam_streaming() && !cam_started()) {
                state = LINK_STARTING;
                show_state(now);
                uint8_t step = 0;
                if (cam_start(want_level, &step)) state = LINK_STREAMING;
                else { fail_step = step; state = cam_daddr ? LINK_START_FAILED : LINK_WAITING; }
            }
        }
        /* nobody watching: switch the camera (and its light) off */
        if (cam_started() && want_level == 3 && time_us_32() - last_frame_ask > IDLE_STOP_US) {
            cam_stop();
            if (cam_daddr) state = LINK_CAMERA_OK;
        }
        cam_tick();
    }
}
