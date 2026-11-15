/* camtest - step 1 of CamLink (video calls on the GameCube), the GameCube side.
 *
 * Finds the USB-to-GameCube adapter running the camlink firmware in any
 * controller port and shows what it found: whether the webcam answered,
 * its USB ids, its video stream and microphone. A runs a speed test of the
 * controller-port link (how fast pictures could travel), X shows the
 * camera's USB description as hex (to send back for the next step).
 * Use a normal controller in another port. START: exit. */
#include <gccore.h>
#include <ogc/si.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "link.h"

static void *xfb;
static GXRModeObj *rmode;

/* ------------------------------------------------------------ talking to the adapter */

static volatile int xfer_done;
static volatile u32 xfer_status;
static void xfer_cb(s32 chan, u32 type) { (void)chan; xfer_status = type; xfer_done = 1; }

/* One command and its reply; false if no (or a broken) answer. */
static bool link_xfer(int chan, const void *out, u32 out_len, void *in, u32 in_len) {
    static u8 obuf[8] ATTRIBUTE_ALIGN(32);
    static u8 ibuf[128] ATTRIBUTE_ALIGN(32);
    memcpy(obuf, out, out_len);
    xfer_done = 0;
    xfer_status = 0;
    if (!SI_Transfer(chan, obuf, out_len, ibuf, in_len, xfer_cb, 0)) return false;
    u64 start = gettime();
    while (!xfer_done) {
        if (diff_usec(start, gettime()) > 20000) return false;   /* 20 ms */
    }
    if (xfer_status & (SI_ERROR_NO_RESPONSE | SI_ERROR_COLLISION | SI_ERROR_UNDER_RUN | SI_ERROR_OVER_RUN)) return false;
    memcpy(in, ibuf, in_len);
    return true;
}

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

typedef struct {
    bool found;
    u8 version, state, mic;
    u16 vid, pid, max_iso, desc_len;
    u32 commands, speed;
} Info;

static bool read_info(int chan, Info *info) {
    u8 cmd = LINK_CMD_HELLO, b[LINK_INFO_LEN];
    memset(info, 0, sizeof(*info));
    if (!link_xfer(chan, &cmd, 1, b, LINK_INFO_LEN)) return false;
    if (b[0] != LINK_MAGIC0 || b[1] != LINK_MAGIC1 || b[2] != LINK_MAGIC2 || b[3] != LINK_MAGIC3) return false;
    info->found = true;
    info->version = b[4];
    info->state = b[5];
    info->vid = le16(b + 6);
    info->pid = le16(b + 8);
    info->max_iso = le16(b + 10);
    info->mic = b[12];
    info->desc_len = le16(b + 14);
    info->commands = le32(b + 16);
    info->speed = le32(b + 20);
    return true;
}

/* ------------------------------------------------------------ screens */

static void clear(void) { printf("\x1b[2J\x1b[2;0H"); }

static const char *state_text(int s) {
    switch (s) {
    case LINK_WAITING: return "No USB device (is the camera plugged in?)";
    case LINK_CAMERA_OK: return "QuickCam found, video stream found";
    case LINK_CAMERA_NO_STREAM: return "QuickCam found, but no video stream";
    case LINK_OTHER_DEVICE: return "A different USB device (not the QuickCam)";
    default: return "?";
    }
}

static void show_main(int chan, const Info *in, const char *speed_line) {
    clear();
    printf("  CamLink - adapter test (step 1)\n");
    printf("  ===============================\n\n");
    for (int c = 0; c < 4; c++) {
        if (c == chan) printf("  Port %d: adapter (CamLink v%d)\n", c + 1, in->version);
        else printf("  Port %d: -\n", c + 1);
    }
    printf("\n");
    if (chan < 0) {
        printf("  No adapter found. Is it in a controller port, with\n");
        printf("  the CamLink firmware on it?\n");
    } else {
        printf("  Camera:      %s\n", state_text(in->state));
        if (in->vid || in->pid) printf("  USB ids:     %04x:%04x\n", in->vid, in->pid);
        else printf("  USB ids:     -\n");
        printf("  Video:       %s", in->max_iso ? "" : "-\n");
        if (in->max_iso) printf("packets up to %u bytes\n", in->max_iso);
        printf("  Microphone:  %s\n", in->mic ? "yes" : (in->state == LINK_CAMERA_OK ? "not found" : "-"));
        printf("  Answered:    %u commands\n", (unsigned)in->commands);
        printf("\n  Link speed:  %s\n", speed_line);
    }
    printf("\n\n  A: speed test   X: camera details   START: exit\n");
}

/* About 3 seconds of 64-byte replies: how much gets through, and intact. */
static void speed_test(int chan, char *line, size_t cap) {
    clear();
    printf("\n  Testing the link speed (3 seconds)...\n");
    u32 ok = 0, bad = 0, lost = 0;
    u8 seq = 0, b[LINK_SPEED_LEN];
    u64 start = gettime();
    while (diff_usec(start, gettime()) < 3000000) {
        u8 cmd[2] = { LINK_CMD_SPEED, seq };
        if (!link_xfer(chan, cmd, 2, b, LINK_SPEED_LEN)) { lost++; seq++; continue; }
        u8 sum = 0;
        bool good = b[0] == seq;
        for (int i = 0; i < LINK_SPEED_LEN - 1; i++) {
            sum += b[i];
            if (b[i] != (u8)(seq + i)) good = false;
        }
        if (sum != b[LINK_SPEED_LEN - 1]) good = false;
        if (good) ok++; else bad++;
        seq++;
    }
    u32 secs_x10 = (u32)(diff_usec(start, gettime()) / 100000);
    u32 bytes = ok * LINK_SPEED_LEN;
    u32 kbps_x10 = secs_x10 ? bytes * 10 * 10 / 1024 / secs_x10 : 0;
    snprintf(line, cap, "%u.%u KB/s  (%u good, %u damaged, %u lost)",
             (unsigned)(kbps_x10 / 10), (unsigned)(kbps_x10 % 10), (unsigned)ok, (unsigned)bad, (unsigned)lost);
}

/* The camera's USB configuration as hex, a screen at a time. */
static void show_details(int chan, const Info *in) {
    static u8 desc[512];
    int len = in->desc_len > sizeof(desc) ? (int)sizeof(desc) : in->desc_len;
    bool ok = true;
    for (int page = 0; page * LINK_PAGE_LEN < len; page++) {
        u8 cmd[2] = { LINK_CMD_DESC, (u8)page };
        if (!link_xfer(chan, cmd, 2, desc + page * LINK_PAGE_LEN, LINK_PAGE_LEN)) { ok = false; break; }
    }
    int top = 0;
    const int rows = 16;
    for (;;) {
        clear();
        printf("  Camera details  %04x:%04x, %d bytes%s\n\n", in->vid, in->pid, len, ok ? "" : " (read failed)");
        for (int r = 0; r < rows; r++) {
            int off = (top + r) * 16;
            if (off >= len) break;
            printf("  %03x ", off);
            for (int i = 0; i < 16 && off + i < len; i++) printf(" %02x", desc[off + i]);
            printf("\n");
        }
        printf("\n  Up/down: scroll   B: back\n");
        printf("  (A photo of these screens is all I need)\n");
        for (;;) {
            VIDEO_WaitVSync();
            PAD_ScanPads();
            u32 d = 0;
            for (int c = 0; c < 4; c++) d |= PAD_ButtonsDown(c);
            if (d & PAD_BUTTON_B) return;
            if ((d & PAD_BUTTON_DOWN) && (top + rows) * 16 < len) { top += rows; break; }
            if ((d & PAD_BUTTON_UP) && top > 0) { top -= rows; break; }
        }
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

    char speed_line[96] = "not tested yet (press A)";
    int chan = -1, frame = 0;
    Info info;
    memset(&info, 0, sizeof(info));
    for (;;) {
        /* look for the adapter (and refresh what it reports) twice a second */
        if (frame % 30 == 0) {
            Info now;
            int found = -1;
            if (chan >= 0 && read_info(chan, &now)) found = chan;
            for (int c = 0; c < 4 && found < 0; c++) if (read_info(c, &now)) found = c;
            chan = found;
            if (found >= 0) info = now; else memset(&info, 0, sizeof(info));
            show_main(chan, &info, speed_line);
        }
        frame++;
        VIDEO_WaitVSync();
        PAD_ScanPads();
        u32 d = 0;
        for (int c = 0; c < 4; c++) if (c != chan) d |= PAD_ButtonsDown(c);
        if (d & PAD_BUTTON_START) exit(0);
        if ((d & PAD_BUTTON_A) && chan >= 0) { speed_test(chan, speed_line, sizeof(speed_line)); frame = 0; }
        if ((d & PAD_BUTTON_X) && chan >= 0 && info.desc_len) { show_details(chan, &info); frame = 0; }
    }
}
