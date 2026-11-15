/* camview - step 2 of CamLink (video calls on the GameCube): the first live
 * picture.
 *
 * Finds the CamLink adapter in any controller port, starts the webcam (its
 * light should come on), and shows its pictures full screen as fast as the
 * controller-port link allows, with a line of numbers: frames per second, the
 * link's speed, and the adapter's own counts (for diagnosis).
 *
 * Use a wired controller in another port. A: start / stop the camera.
 * START (or hold L+R): stop the camera and restart the GameCube. */
#include <gccore.h>
#include <ogc/si.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link.h"
#include "tjpgd.h"

static void *xfb;
static GXRModeObj *rmode;

/* ------------------------------------------------------------ talking to the adapter */

static volatile int xfer_done;
static volatile u32 xfer_status;
static void xfer_cb(s32 chan, u32 type) { (void)chan; xfer_status = type; xfer_done = 1; }

static bool link_xfer(int chan, const void *out, u32 out_len, void *in, u32 in_len) {
    static u8 obuf[8] ATTRIBUTE_ALIGN(32);
    static u8 ibuf[128] ATTRIBUTE_ALIGN(32);
    memcpy(obuf, out, out_len);
    xfer_done = 0;
    xfer_status = 0;
    if (!SI_Transfer(chan, obuf, out_len, ibuf, in_len, xfer_cb, 0)) return false;
    u64 start = gettime();
    while (!xfer_done) {
        if (diff_usec(start, gettime()) > 20000) return false;
    }
    if (xfer_status & (SI_ERROR_NO_RESPONSE | SI_ERROR_COLLISION | SI_ERROR_UNDER_RUN | SI_ERROR_OVER_RUN)) return false;
    memcpy(in, ibuf, in_len);
    return true;
}

/* The same, tried a few times (an odd reply gets lost). */
static bool link_xfer_retry(int chan, const void *out, u32 out_len, void *in, u32 in_len) {
    for (int i = 0; i < 4; i++) if (link_xfer(chan, out, out_len, in, in_len)) return true;
    return false;
}

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

typedef struct {
    u8 version, state, fail_step;
    u32 packets, frames, dropped;
} Info;

static bool read_info(int chan, Info *info) {
    u8 cmd = LINK_CMD_HELLO, b[LINK_INFO_LEN];
    if (!link_xfer_retry(chan, &cmd, 1, b, LINK_INFO_LEN)) return false;
    if (b[0] != LINK_MAGIC0 || b[1] != LINK_MAGIC1 || b[2] != LINK_MAGIC2 || b[3] != LINK_MAGIC3) return false;
    info->version = b[4];
    info->state = b[5];
    info->fail_step = b[13];
    info->packets = le32(b + 20);
    info->frames = le32(b + 24);
    info->dropped = le32(b + 28);
    return true;
}

static int find_adapter(Info *info) {
    for (int c = 0; c < 4; c++) if (read_info(c, info)) return c;
    return -1;
}

/* ------------------------------------------------------------ JPEG */

/* A baseline JPEG header for the camera's data: the JPEG standard's example
 * tables (Annex K), quantisation scaled to the quality the usual way. */
static const u8 std_luma_q[64] = {
    16, 11, 12, 14, 12, 10, 16, 14, 13, 14, 18, 17, 16, 19, 24, 40,
    26, 24, 22, 22, 24, 49, 35, 37, 29, 40, 58, 51, 61, 60, 57, 51,
    56, 55, 64, 72, 92, 78, 64, 68, 87, 69, 55, 56, 80, 109, 81, 87,
    95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99 };
static const u8 std_chroma_q[64] = {
    17, 18, 18, 24, 21, 24, 47, 26, 26, 47, 99, 66, 56, 66, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99 };
/* (both in the zig-zag order a DQT segment stores them in) */
static const u8 dc_luma_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const u8 dc_chroma_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const u8 dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const u8 ac_luma_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const u8 ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa };
static const u8 ac_chroma_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const u8 ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa };

static int put_dht(u8 *p, u8 cls_id, const u8 *bits, const u8 *vals, int nvals) {
    int n = 0;
    p[n++] = 0xff; p[n++] = 0xc4;
    int len = 2 + 1 + 16 + nvals;
    p[n++] = (u8)(len >> 8); p[n++] = (u8)len;
    p[n++] = cls_id;
    memcpy(p + n, bits, 16); n += 16;
    memcpy(p + n, vals, nvals); n += nvals;
    return n;
}

/* Writes the header for a w x h picture at the given quality; returns its length. */
static int jpeg_header(u8 *p, int w, int h, int quality) {
    int sc = quality <= 0 ? 5000 : quality < 50 ? 5000 / quality : 200 - quality * 2;
    int n = 0;
    p[n++] = 0xff; p[n++] = 0xd8;
    p[n++] = 0xff; p[n++] = 0xdb; p[n++] = 0x00; p[n++] = 0x84;    /* DQT, both tables */
    p[n++] = 0;
    for (int i = 0; i < 64; i++) { int v = (std_luma_q[i] * sc + 50) / 100; p[n++] = (u8)(v < 1 ? 1 : v > 255 ? 255 : v); }
    p[n++] = 1;
    for (int i = 0; i < 64; i++) { int v = (std_chroma_q[i] * sc + 50) / 100; p[n++] = (u8)(v < 1 ? 1 : v > 255 ? 255 : v); }
    n += put_dht(p + n, 0x00, dc_luma_bits, dc_vals, 12);
    n += put_dht(p + n, 0x10, ac_luma_bits, ac_luma_vals, 162);
    n += put_dht(p + n, 0x01, dc_chroma_bits, dc_vals, 12);
    n += put_dht(p + n, 0x11, ac_chroma_bits, ac_chroma_vals, 162);
    static const u8 sof_tail[] = { 0x03, 0x01, 0x21, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01 };
    p[n++] = 0xff; p[n++] = 0xc0; p[n++] = 0x00; p[n++] = 0x11; p[n++] = 0x08;   /* SOF0 */
    p[n++] = (u8)(h >> 8); p[n++] = (u8)h; p[n++] = (u8)(w >> 8); p[n++] = (u8)w;
    memcpy(p + n, sof_tail, sizeof(sof_tail)); n += sizeof(sof_tail);
    static const u8 sos[] = { 0xff, 0xda, 0x00, 0x0c, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3f, 0x00 };
    memcpy(p + n, sos, sizeof(sos)); n += sizeof(sos);
    return n;
}

#define IMG_W 320
#define IMG_H 240
static u32 img[IMG_W * IMG_H];           /* each pixel as an XFB word: Y Cb Y Cr */
static u8 jpeg[40000];
static int jpeg_len, jpeg_pos;
static u8 jd_work[32768] ATTRIBUTE_ALIGN(32);   /* (TJpgDec's working memory: tables and its input buffer) */

static size_t jd_in(JDEC *jd, uint8_t *buf, size_t n) {
    (void)jd;
    if ((int)n > jpeg_len - jpeg_pos) n = (size_t)(jpeg_len - jpeg_pos);
    if (buf) memcpy(buf, jpeg + jpeg_pos, n);
    jpeg_pos += (int)n;
    return n;
}

static int jd_out(JDEC *jd, void *bitmap, JRECT *r) {
    (void)jd;
    const u8 *s = bitmap;
    for (int y = r->top; y <= r->bottom; y++)
        for (int x = r->left; x <= r->right; x++, s += 3) {
            if (x >= IMG_W || y >= IMG_H) continue;
            int R = s[0], G = s[1], B = s[2];
            int Y = (77 * R + 150 * G + 29 * B) >> 8;
            int Cb = ((-43 * R - 85 * G + 128 * B) >> 8) + 128;
            int Cr = ((128 * R - 107 * G - 21 * B) >> 8) + 128;
            if (Cb < 0) Cb = 0; if (Cb > 255) Cb = 255;
            if (Cr < 0) Cr = 0; if (Cr > 255) Cr = 255;
            img[y * IMG_W + x] = ((u32)Y << 24) | ((u32)Cb << 16) | ((u32)Y << 8) | (u32)Cr;
        }
    return 1;
}

static bool decode(int w, int h) {
    JDEC jd;
    jpeg_pos = 0;
    if (jd_prepare(&jd, jd_in, jd_work, sizeof(jd_work), NULL) != JDR_OK) return false;
    (void)w; (void)h;
    return jd_decomp(&jd, jd_out, 0) == JDR_OK;
}

/* The picture, twice the size, in the middle of the screen. */
static void show_picture(int w, int h) {
    u32 *fb = xfb;
    int stride = rmode->fbWidth / 2;              /* words per line (2 pixels each) */
    int x0 = (stride - w) / 2, y0 = (rmode->xfbHeight - h * 2) / 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    for (int y = 0; y < h; y++) {
        const u32 *src = &img[y * IMG_W];
        for (int k = 0; k < 2; k++) {
            int dy = y0 + y * 2 + k;
            if (dy >= rmode->xfbHeight) break;
            memcpy(fb + dy * stride + x0, src, (size_t)(w < stride ? w : stride) * 4);
        }
    }
}

static void clear_screen(void) { printf("\x1b[2J"); }

static const char *state_text(int s) {
    switch (s) {
    case LINK_WAITING: return "no camera plugged in";
    case LINK_CAMERA_OK: return "camera ready";
    case LINK_CAMERA_NO_STREAM: return "camera has no video stream";
    case LINK_OTHER_DEVICE: return "not the QuickCam";
    case LINK_STARTING: return "starting the camera...";
    case LINK_STREAMING: return "streaming";
    case LINK_START_FAILED: return "the camera didn't start";
    default: return "?";
    }
}

int main(void) {
    VIDEO_Init();
    PAD_Init();
    rmode = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    clear_screen();
    printf("\n  CamLink - live picture test (step 2)\n\n  Looking for the adapter...\n");
    Info info;
    memset(&info, 0, sizeof(info));
    int chan = -1;
    while (chan < 0) {
        chan = find_adapter(&info);
        VIDEO_WaitVSync();
        PAD_ScanPads();
        if (PAD_ButtonsDown(0) & PAD_BUTTON_START) SYS_ResetSystem(SYS_HOTRESET, 0, FALSE);
    }
    /* (the console's own controller polling only gets in the way on that port) */
    SI_DisablePolling(SI_CHAN_BIT(chan));

    bool want_on = true;
    u8 cmd[3];
    cmd[0] = LINK_CMD_START;
    u8 r[LINK_FRAME_LEN];
    link_xfer_retry(chan, cmd, 1, r, 1);

    u16 last_id = 0;
    u32 shown = 0, bytes = 0, bad = 0, lost = 0;
    u64 window = gettime();
    char fps_line[80] = "", stat_line[96] = "";
    int quality = 50, w = 320, h = 232;
    u64 last_info = 0;
    for (;;) {
        /* buttons, from the other ports */
        PAD_ScanPads();
        u32 d = 0, held = 0;
        for (int c = 0; c < 4; c++) if (c != chan) { d |= PAD_ButtonsDown(c); held |= PAD_ButtonsHeld(c); }
        if ((d & PAD_BUTTON_START) || ((held & PAD_TRIGGER_L) && (held & PAD_TRIGGER_R))) {
            cmd[0] = LINK_CMD_STOP;
            link_xfer_retry(chan, cmd, 1, r, 1);
            SYS_ResetSystem(SYS_HOTRESET, 0, FALSE);
        }
        if (d & PAD_BUTTON_A) {
            want_on = !want_on;
            cmd[0] = want_on ? LINK_CMD_START : LINK_CMD_STOP;
            link_xfer_retry(chan, cmd, 1, r, 1);
            if (!want_on) clear_screen();
        }

        /* the adapter's counts, twice a second */
        if (diff_usec(last_info, gettime()) > 500000) {
            last_info = gettime();
            if (read_info(chan, &info)) {
                snprintf(stat_line, sizeof(stat_line), "%s%s  packets %u  frames %u  dropped %u",
                         state_text(info.state), info.state == LINK_START_FAILED ? " (step " : "",
                         (unsigned)info.packets, (unsigned)info.frames, (unsigned)info.dropped);
                if (info.state == LINK_START_FAILED) {
                    char tmp[96];
                    snprintf(tmp, sizeof(tmp), "the camera didn't start (step %d)  packets %u", info.fail_step,
                             (unsigned)info.packets);
                    strcpy(stat_line, tmp);
                }
            } else {
                snprintf(stat_line, sizeof(stat_line), "adapter not answering");
            }
        }

        /* a new frame? */
        bool got = false;
        if (want_on) {
            cmd[0] = LINK_CMD_FRAME;
            if (link_xfer_retry(chan, cmd, 1, r, LINK_FRAME_LEN)) {
                u16 id = le16(r);
                u32 len = le32(r + 2);
                if (id && id != last_id && len > 0 && len < sizeof(jpeg) - 700) {
                    last_id = id;
                    w = le16(r + 6);
                    h = le16(r + 8);
                    quality = r[10];
                    if (w <= 0 || w > IMG_W) w = 320;
                    if (h <= 0 || h > IMG_H) h = 232;
                    int hl = jpeg_header(jpeg, w, h, quality);
                    int pieces = (int)((len + LINK_CHUNK_LEN - 1) / LINK_CHUNK_LEN);
                    bool ok = true;
                    for (int i = 0; i < pieces && ok; i++) {
                        u8 c3[3] = { LINK_CMD_CHUNK, (u8)(i >> 8), (u8)i };
                        u8 piece[LINK_CHUNK_LEN];
                        if (!link_xfer_retry(chan, c3, 3, piece, LINK_CHUNK_LEN)) { ok = false; lost++; break; }
                        int n = (int)len - i * LINK_CHUNK_LEN;
                        if (n > LINK_CHUNK_LEN) n = LINK_CHUNK_LEN;
                        memcpy(jpeg + hl + i * LINK_CHUNK_LEN, piece, n);
                    }
                    if (ok) {
                        jpeg_len = hl + (int)len;
                        bytes += len;
                        if (decode(w, h)) { got = true; shown++; }
                        else bad++;
                    }
                }
            }
        }

        if (got) show_picture(w, h);
        /* the numbers, once a second */
        u64 t = gettime();
        if (diff_usec(window, t) >= 1000000) {
            u32 us = (u32)diff_usec(window, t);
            snprintf(fps_line, sizeof(fps_line), "%u.%u fps   %u KB/s   %dx%d q%d   bad %u lost %u",
                     (unsigned)(shown * 10000000ULL / us / 10), (unsigned)(shown * 10000000ULL / us % 10),
                     (unsigned)(bytes * 1000000ULL / us / 1024), w, h, quality, (unsigned)bad, (unsigned)lost);
            shown = bytes = 0;
            window = t;
        }
        if (got || !want_on || (t & 0x3f) == 0) {
            printf("\x1b[1;0H  %-74s\n  %-74s\n", fps_line, stat_line);
            if (!want_on) printf("\n  Camera off. A: start it again   START: restart\n");
        }
        if (!got) VIDEO_WaitVSync();
    }
}
