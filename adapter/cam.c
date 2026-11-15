/* cam.c - see cam.h.
 *
 * The camera is a Z-Star/Vimicro ZC0301-family bridge with an MC501CB
 * sensor, the same as the Linux gspca "zc3xx" driver supports; the settings
 * and the order they're sent in follow that driver (GPL-2.0-or-later,
 * (C) Jean-Francois Moine and Michel Xhaard), which is why this firmware is
 * GPL too. Its pictures are JPEG, 320x232, a frame being a run of packets that
 * starts with ff d8 (and an 18-byte header of the camera's own) and ends with
 * ff d9 and one more byte.
 *
 * The video arrives as isochronous packets on endpoint 0x81, up to 1023 bytes
 * every millisecond. TinyUSB doesn't do isochronous transfers on the RP2040,
 * so CamLink sets up one of the USB controller's polled ("interrupt")
 * endpoints, number 15, as isochronous itself: the controller then asks the
 * camera for a packet every frame on its own, and camlink_usb_buffers() (run
 * from TinyUSB's interrupt handler, see CMakeLists.txt) takes each one. */
#include "cam.h"
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/usb_dpram.h"
#include "hardware/sync.h"
#include "tusb.h"

/* ------------------------------------------------------------ talking to the camera */

static volatile uint8_t cam_addr;      /* USB address, 0 = not plugged in */
volatile uint8_t cam_progress;          /* how far cam_start has got (for diagnosis) */
static uint8_t started_level;          /* the test level it was started at, 0 = stopped */
static volatile bool streaming;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* Waits, keeping USB going. */
static void pause_ms(uint32_t ms) {
    uint32_t t0 = now_ms();
    while (now_ms() - t0 < ms) tuh_task();
}

static volatile bool ctl_done;
static volatile xfer_result_t ctl_result;
static void ctl_cb(tuh_xfer_t *x) { ctl_result = x->result; ctl_done = true; }

/* One control request, waited for. */
static bool ctl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint8_t *buf, uint16_t len) {
    if (!cam_addr) return false;
    tusb_control_request_t setup = {
        .bmRequestType = type, .bRequest = req, .wValue = value, .wIndex = index, .wLength = len };
    tuh_xfer_t x = { .daddr = cam_addr, .ep_addr = 0, .setup = &setup, .buffer = buf,
                     .complete_cb = ctl_cb, .user_data = 0 };
    ctl_done = false;
    if (!tuh_control_xfer(&x)) return false;
    uint32_t t0 = now_ms();
    while (!ctl_done) {
        tuh_task();
        if (!cam_addr || now_ms() - t0 > 500) return false;
    }
    return ctl_result == XFER_RESULT_SUCCESS;
}

static bool usb_ok;   /* false after any failed request (the rest are skipped) */

static void reg_w(uint8_t value, uint16_t index) {
    if (usb_ok && !ctl(0x40, 0xa0, value, index, NULL, 0)) usb_ok = false;
}

static uint8_t reg_r(uint16_t index) {
    static uint8_t b[1];
    if (usb_ok && !ctl(0xc0, 0xa1, 0x01, index, b, 1)) usb_ok = false;
    return b[0];
}

static void i2c_write(uint8_t reg, uint8_t lo, uint8_t hi) {
    reg_w(reg, 0x92);
    reg_w(lo, 0x93);
    reg_w(hi, 0x94);
    reg_w(0x01, 0x90);          /* write */
    pause_ms(1);
    reg_r(0x0091);              /* status */
}

typedef struct { uint8_t req, val; uint16_t idx; } CamAction;
#include "cam_tables.inc"

static void exchange(const CamAction *a) {
    for (; a->req && usb_ok; a++) {
        if (a->req == 0xa0) reg_w(a->val, a->idx);
        else if (a->req == 0xa1) reg_r(a->idx);
        else if (a->req == 0xaa) i2c_write(a->val, a->idx & 0xff, a->idx >> 8);
        else if (a->req == 0xbb) i2c_write(a->idx >> 8, a->idx & 0xff, a->val);
        else pause_ms(a->idx);
        pause_ms(1);
    }
}

/* Gamma and its gradient (the driver's setcontrast, at gamma 4 with
 * brightness and contrast in the middle). */
static void set_gamma(void) {
    static const uint8_t gamma4[16] = { 0x13, 0x38, 0x59, 0x79, 0x92, 0xa7, 0xb9, 0xc8,
                                        0xd4, 0xdf, 0xe7, 0xee, 0xf4, 0xf9, 0xfc, 0xff };
    uint8_t gr[16];
    int gp1 = 0, gp2 = 0;
    for (int i = 0; i < 16; i++) {
        int g = gamma4[i];
        reg_w((uint8_t)g, 0x0120 + i);
        if (i > 1) gr[i - 1] = (uint8_t)((g - gp2) / 2);
        else if (i != 0) gr[0] = gp1 == 0 ? 0 : (uint8_t)(g - gp1);
        gp2 = gp1;
        gp1 = g;
    }
    gr[15] = (uint8_t)((0xff - gp2) / 2);
    for (int i = 0; i < 16; i++) reg_w(gr[i], 0x0130 + i);
}

static volatile bool alt_done;
static volatile xfer_result_t alt_result;
static void alt_cb(tuh_xfer_t *x) { alt_result = x->result; alt_done = true; }

static bool set_alt(uint8_t alt) {
    if (!cam_addr) return false;
    alt_done = false;
    if (!tuh_interface_set(cam_addr, 0, alt, alt_cb, 0)) return false;
    uint32_t t0 = now_ms();
    while (!alt_done) {
        tuh_task();
        if (!cam_addr || now_ms() - t0 > 500) return false;
    }
    return alt_result == XFER_RESULT_SUCCESS;
}

/* ------------------------------------------------------------ frames */

/* Three buffers: one being filled, the newest whole frame, and the one the
 * GameCube is reading. Only the first two are swapped by the USB side, only
 * the last two by the GameCube side, under a spin lock. */
static uint8_t fb[3][CAM_FRAME_MAX];
static uint32_t fb_len[3];
static uint16_t fb_w[3], fb_h[3], fb_id[3];
static volatile int fill_i = 0, ready_i = 1, send_i = 2;
static volatile bool ready_valid;
static spin_lock_t *lock;

static bool filling;
static uint16_t cur_w = 320, cur_h = 232, next_id = 1;
static volatile uint32_t n_packets, n_frames, n_dropped;
static uint8_t reg08 = 1;       /* JPEG quality: 1 = 50% (smaller frames, more of them) */
static const uint8_t qualities[] = { 50, 75, 87 };

static void __not_in_flash_func(frame_done)(void) {
    int f = fill_i;
    fb_w[f] = cur_w;
    fb_h[f] = cur_h;
    fb_id[f] = next_id++;
    if (next_id == 0) next_id = 1;
    uint32_t s = spin_lock_blocking(lock);
    fill_i = ready_i;
    ready_i = f;
    ready_valid = true;
    spin_unlock(lock, s);
    n_frames++;
}

static void __not_in_flash_func(packet)(const uint8_t *p, uint32_t len) {
    n_packets++;
    if (len == 0) return;
    if (len >= 2 && p[0] == 0xff && p[1] == 0xd8) {
        /* a new frame: ff d8 ff fe 00 0e 00 00 ss ss 00 01 ww ww hh hh pp pp */
        if (filling) n_dropped++;          /* (the last one never ended) */
        filling = true;
        fb_len[fill_i] = 0;
        if (len >= 18) {
            cur_w = (uint16_t)(p[12] << 8 | p[13]);
            cur_h = (uint16_t)(p[14] << 8 | p[15]);
            p += 18;
            len -= 18;
        } else {
            return;
        }
    }
    if (!filling) return;
    bool end = len >= 3 && p[len - 3] == 0xff && p[len - 2] == 0xd9;
    if (end) len -= 1;                     /* (the last byte isn't JPEG) */
    uint32_t at = fb_len[fill_i];
    if (at + len > CAM_FRAME_MAX) {
        filling = false;
        n_dropped++;
        return;
    }
    memcpy(&fb[fill_i][at], p, len);
    fb_len[fill_i] = at + len;
    if (end) {
        filling = false;
        frame_done();
    }
}

CamFrame __not_in_flash_func(cam_take_frame)(void) {
    uint32_t s = spin_lock_blocking(lock);
    if (ready_valid) {
        int t = send_i;
        send_i = ready_i;
        ready_i = t;
        ready_valid = false;
    }
    int i = send_i;
    spin_unlock(lock, s);
    CamFrame f = { fb_id[i], fb_len[i], fb_w[i], fb_h[i], fb[i] };
    return f;
}

/* ------------------------------------------------------------ the video endpoint */

#define ISO_EP 15                 /* the controller's polled endpoint 15 */
#define ISO_SLOT (ISO_EP - 1)
#define ISO_BIT (1u << (ISO_EP * 2))  /* its "buffer done" bit (IN) */
#define ISO_DPRAM_OFFSET 0x800    /* its buffer, past TinyUSB's */
#define ISO_MAX 1023

static void __not_in_flash_func(iso_arm)(void) {
    io_rw_32 *bc = &usbh_dpram->int_ep_buffer_ctrl[ISO_SLOT].ctrl;
    *bc = ISO_MAX;                /* empty, DATA0 (isochronous is always DATA0) */
    __asm volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop");
    *bc = ISO_MAX | USB_BUF_CTRL_AVAIL;
}

uint32_t __not_in_flash_func(camlink_usb_buffers)(uint32_t remaining) {
    if (!(remaining & ISO_BIT)) return remaining;
    hw_clear_bits(&usb_hw->buf_status, ISO_BIT);   /* (as TinyUSB clears its own) */
    if (streaming) {
        uint32_t len = usbh_dpram->int_ep_buffer_ctrl[ISO_SLOT].ctrl & USB_BUF_CTRL_LEN_MASK;
        packet((const uint8_t *)usbh_dpram + ISO_DPRAM_OFFSET, len);
        iso_arm();
    }
    return remaining & ~ISO_BIT;
}

static void iso_open(uint8_t daddr) {
    /* device address, endpoint 1, IN */
    usb_hw->int_ep_addr_ctrl[ISO_SLOT] = daddr | (1u << USB_ADDR_ENDP1_ENDPOINT_LSB);
    usbh_dpram->int_ep_ctrl[ISO_SLOT].ctrl = EP_CTRL_ENABLE_BITS | EP_CTRL_INTERRUPT_PER_BUFFER |
                                             (TUSB_XFER_ISOCHRONOUS << EP_CTRL_BUFFER_TYPE_LSB) |
                                             (0u << EP_CTRL_HOST_INTERRUPT_INTERVAL_LSB) |  /* every frame */
                                             ISO_DPRAM_OFFSET;
    iso_arm();
    hw_set_bits(&usb_hw->int_ep_ctrl, 1u << ISO_EP);
}

static void iso_close(void) {
    hw_clear_bits(&usb_hw->int_ep_ctrl, 1u << ISO_EP);
    usbh_dpram->int_ep_ctrl[ISO_SLOT].ctrl = 0;
    usbh_dpram->int_ep_buffer_ctrl[ISO_SLOT].ctrl = 0;
    hw_clear_bits(&usb_hw->buf_status, ISO_BIT);
}

/* ------------------------------------------------------------ start and stop */

void cam_attach(uint8_t daddr) {
    if (!lock) lock = spin_lock_instance(spin_lock_claim_unused(true));
    cam_addr = daddr;
}

void cam_detach(void) {
    if (streaming) iso_close();
    streaming = false;
    started_level = 0;
    cam_progress = 0;
    cam_addr = 0;
}

bool cam_streaming(void) { return streaming; }
bool cam_started(void) { return started_level != 0; }

bool cam_start(int level, uint8_t *fail_step) {
    *fail_step = 0;
    cam_progress = 1;
    if (!cam_addr) { *fail_step = 1; return false; }
    if (streaming || started_level) return true;
    usb_ok = true;
    /* the driver's sd_init (the sensor is known, so no probing) */
    reg_w(0x02, 0x0010);
    reg_r(0x0010);
    reg_w(0x01, 0x0000);
    if (!usb_ok) { *fail_step = 2; return false; }
    cam_progress = 2;
    /* the biggest packets (alternate setting 7: 1023 bytes) */
    if (level >= 2 && !set_alt(7)) { *fail_step = 3; return false; }
    cam_progress = 3;
    /* the driver's sd_start for the MC501CB at 320x232 */
    exchange(mc501cb_InitialScale);
    if (!usb_ok) { *fail_step = 4; return false; }
    cam_progress = 4;
    reg_w(0x08, 0x01c6);                    /* sharpness 2 */
    reg_r(0x01c8);
    reg_r(0x01c9);
    reg_r(0x01ca);
    reg_w(0x0f, 0x01cb);
    set_gamma();
    reg_w(reg08, 0x0008);                   /* JPEG quality */
    reg_w(0x00, 0x0007);                    /* data-rate control off for now (cam_tick) */
    exchange(mc501cb_60HZScale);            /* 60 Hz lights (North America) */
    reg_w(0x42, 0x0180);                    /* automatic exposure */
    if (!usb_ok) { *fail_step = 5; return false; }
    cam_progress = 5;
    started_level = (uint8_t)level;
    if (level < 3) return true;           /* (test levels: no receiving) */
    /* and receive */
    filling = false;
    ready_valid = false;
    streaming = true;
    iso_open(cam_addr);
    cam_progress = 6;
    return true;
}

void cam_stop(void) {
    if (!streaming && !started_level) return;
    if (streaming) iso_close();
    streaming = false;
    started_level = 0;
    cam_progress = 0;
    usb_ok = true;
    set_alt(0);
    reg_w(0x01, 0x0000);                    /* the driver's stop: this also turns the light off */
}

/* The driver's transfer_update: if the camera's buffer overflowed (register
 * 0x11, bit 0), limit its data rate (register 0x07), and relax it again
 * after a while without. */
void cam_tick(void) {
    static uint32_t last;
    static uint8_t reg07, good;
    if (!streaming || now_ms() - last < 100) return;
    last = now_ms();
    usb_ok = true;
    uint8_t reg11 = reg_r(0x0011);
    if (!usb_ok) return;
    bool change = reg11 & 0x01;
    if (change) {
        good = 0;
        if (reg07 == 0) reg07 = 0x32;
        else if (reg07 > 2) reg07 -= 2;
        else change = false;
    } else if (++good >= 10) {
        good = 0;
        if (reg07) {
            change = true;
            if (reg07 < 0x32) reg07 += 2;
            else reg07 = 0;
        }
    }
    if (change) reg_w(reg07, 0x0007);
}

uint32_t cam_packets(void) { return n_packets; }
uint32_t cam_frames(void) { return n_frames; }
uint32_t cam_dropped(void) { return n_dropped; }
uint8_t cam_quality(void) { return qualities[reg08 >> 1]; }
