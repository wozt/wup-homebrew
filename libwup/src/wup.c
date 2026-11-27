/* libwup (see wup.h) */
#include <setjmp.h>
#include <stdlib.h>
#include "wup.h"
#include "wup_hw.h"
#include "netfs.h"
#include "armcpu.h"
typedef uint32_t u32; typedef uint8_t u8; typedef uint16_t u16;
#define REG(a) (*(volatile u32 *)(a))
#define LCD(o) REG(0xf0009400u + (o))
#define HWCLK REG(0xf0000408u)
#include "wupk.h"
#define DLY(ms) WUPK->sleep(ms)
#define REPORT  (WUPK->report)
#define BUTTONS (WUPK->report + 2)                 /* (the report's buttons, big-endian) */

extern u8 __bss_start[], __bss_end[];
#define STACK_WORDS 16384                              /* 64 KB: the app block has room */
static u32 stack[STACK_WORDS] __attribute__((aligned(8), section(".bss.stack")));
static int fs_state, screen_on, lcd8_saved, ret;
static u32 lcd8[3];
void wup__kernel_exit(void);
void wup__time_start(void), wup__time_end(void);
static void copy_boot(void);
static int stage_exec(void);
static char exec_path[96];
static jmp_buf exit_jb;
void _exit(int code) { ret = code; longjmp(exit_jb, 1); }
static void hw_reset(void);
static int stick_rest[4];
static void cache_clean(const void *p, u32 n);
/* fade in once the app's first frame is on screen (or after 0.3 s, whatever it shows) */
static int fade_pending; static u32 fade_t0, fade_wait = 300000;
void wup_screen_hold(void) { fade_wait = 3000000; }
static int fade_instant;
void wup_screen_instant(void) { fade_instant = 1; }  /* (the menu: it appears at once, no fade-in) */
static void fade_in_now(void) { if (fade_pending) { fade_pending = 0; if (fade_instant) gfx_brightness(256); else gfx_fade(0, 256); } }

__attribute__((noinline)) static void run_on_stack(void (*f)(void), void *top) {
    __asm__ volatile("mov r4, sp\n\tmov sp, %1\n\tblx %0\n\tmov sp, r4\n\t"
                     :: "r"(f), "r"(top) : "r0", "r1", "r2", "r3", "r4", "r12", "lr", "memory", "cc");
}
static void app_main(void) {
    /* apps fade in and out: the screen is dark between apps (the kernel keeps it so); the first frame is drawn unseen,
       then faded in (fade_in_now) */
    gfx_brightness(0);
    fade_pending = 1; gfx_present_hook = fade_in_now;
    copy_boot();
    for (int i = 0; i < 4; i++) {                      /* the sticks at rest: each pad's are a little off centre */
        int v = (int)(REPORT[6 + 2 * i] | REPORT[7 + 2 * i] << 8) - 0x800;
        stick_rest[i] = v > -300 && v < 300 ? v : 0;
    }
    int formatted = 0;
    fs_state = fs_mount(&formatted) == 0;              /* built with FS_NO_FORMAT: never formats */
    wup__time_start();
    if (!setjmp(exit_jb)) { fade_t0 = HWCLK; ret = main(); }   /* (exit() and abort() come back here; the fade's 0.3 s
                                                       count from here: mounting a fresh filesystem can take longer) */
    wup__time_end();
    wup_audio_close();
    netfs_close_all();                                  /* /net: the connection closed before lwIP's threads stop */
    if (wup__net_exit) wup__net_exit();                /* sockets closed, lwIP's threads stopped, the radio's frames dropped
                                                          again: first, as lwIP's threads never end (waiting for them
                                                          below cost every networked app a second to close) */
    wup__kernel_exit();                                /* the app's threads joined, semaphores given back */
    hw_reset();                                        /* the motor and the microphone off, the menu's brightness */
    gfx_present_hook = 0;
    if (!fade_pending) gfx_fade(256, 0);               /* fade out (unless it never faded in) */
    if (screen_on) gfx_end();
    if (lcd8_saved) {                                  /* the 8-bit screen: cleared, the RGB565 layer back for the next app */
        u8 *f = (u8 *)LCD(0x74); u32 n = LCD(0x70) * GFX_H;
        for (u32 i = 0; i < n; i += 4) *(u32 *)(f + i) = 0;
        cache_clean(f, n);
        LCD(0x100) = 0; LCD(0x104) = 0xff000000u;
        wup_vblank(); LCD(0xb0) = lcd8[0]; LCD(0x70) = lcd8[1]; LCD(0x74) = lcd8[2];
    }
    gfx_brightness(0);                                 /* dark until the next app fades in */
    if (exec_path[0]) ret = stage_exec() ? 0 : WUP_HANDOFF_RUN;   /* the screen's memory is free now: the next app into it */
}
/* The kernel's app runner calls this (kernel/src/app.c): the image is in place and the caches are in step with it.
   .bss cleared, then the app on libwup's own stack. */
__attribute__((section(".text.entry"), target("arm"), noinline)) int _wup_entry(void) {
    for (u8 *p = __bss_start; p < __bss_end; p++) *p = 0;
    run_on_stack(app_main, stack + STACK_WORDS);
    return ret;
}

/* ---- starting another app: its code (as stored in its .wup, deflated) goes to the stage for the kernel, once this app
   has ended and the screen's memory is free (include/handoff.h) */
#include "wupfmt.h"
static char exec_arg[192];
static wup_hdr_t exec_hdr;
int wup_exec(const char *path, const char *arg) {
    wup_hdr_t *h = &exec_hdr;
    memset(h, 0, sizeof *h);
    int n = fs_read(path, 0, h, sizeof *h);
    if (n < WUP_HDR_V1 || h->magic != WUP_MAGIC || !h->code_size || h->code_size > WUP_STAGE_MAX) return -1;
    if (h->hdr_size < WUP_HDR_V3) h->code_raw = h->flags = 0;
    if (h->code_raw > WUP_HANDOFF_WORK_MAX) return -2;
    if (!(h->flags & WUP_FLAG_KERNEL)) return -3;       /* a v1 app (built for Nintendo's firmware): not runnable */
    strncpy(exec_path, path, sizeof exec_path - 1);
    strncpy(exec_arg, arg ? arg : "", sizeof exec_arg - 1);
    return 0;
}
static int stage_exec(void) {
    volatile wup_handoff_t *pb = WUP_HANDOFF;
    u8 *dst = (u8 *)WUP_HANDOFF_STAGE;
    if (fs_read(exec_path, exec_hdr.code_off, dst, exec_hdr.code_size) != (int)exec_hdr.code_size) return -1;
    if (crc32(dst, exec_hdr.code_size) != exec_hdr.code_crc) return -2;
    cache_clean(dst, exec_hdr.code_size);
    pb->stage_size = exec_hdr.code_size; pb->stage_raw = exec_hdr.code_raw;
    for (int i = 0; i < 64; i++) pb->app_path[i] = i < 63 ? exec_path[i] : 0;
    for (int i = 0; i < 192; i++) pb->arg[i] = i < 191 ? exec_arg[i] : 0;
    return 0;
}

const wup_handoff_t *wup_handoff(void) { return WUP_HANDOFF->magic == WUP_HANDOFF_MAGIC ? (const wup_handoff_t *)WUP_HANDOFF : 0; }
int wup_fs_ok(void) { return fs_state; }
static char app_path[64], app_dir[64], app_arg[192];
const char *wup_app_path(void) { return app_path; }
const char *wup_app_dir(void) { return app_dir; }
const char *wup_arg(void) { return app_arg; }
static void copy_boot(void) {
    volatile wup_handoff_t *pb = WUP_HANDOFF;
    if (pb->magic != WUP_HANDOFF_MAGIC) return;
    for (int i = 0; i < 63 && (app_path[i] = pb->app_path[i]); i++) ;
    for (int i = 0; i < 191 && (app_arg[i] = pb->arg[i]); i++) ;
    int slash = 0; for (int i = 0; app_path[i]; i++) if (app_path[i] == '/') slash = i;
    for (int i = 0; i < slash; i++) app_dir[i] = app_path[i];
    app_dir[slash] = 0;
}

void wup_vblank(void) {        /* VCOUNT (+0xC8): visible lines 8..487 of 510 */
    u32 t0 = HWCLK;
    while ((LCD(0xc8) & 0x7ff) >= 488 && HWCLK - t0 < 25000) ;   /* a new blank, not the tail of one */
    while ((LCD(0xc8) & 0x7ff) < 488 && HWCLK - t0 < 25000) ;
}
px_t *wup_screen(void) {
    if (!screen_on) {                                  /* a cleared screen; this present is set-up, not */
        void (*hook)(void) = gfx_present_hook;         /* the app's first frame: it must not fade in    */
        gfx_present_hook = 0;
        gfx_fill(0, 0, GFX_W, GFX_H, 0); gfx_present_all(); gfx_begin(); screen_on = 1;
        gfx_present_hook = hook;
    }
    return gfx_fb();
}
u8 *wup_screen8(void) {
    if (screen_on) { gfx_end(); screen_on = 0; }
    if (!lcd8_saved) { lcd8[0] = LCD(0xb0); lcd8[1] = LCD(0x70); lcd8[2] = LCD(0x74); lcd8_saved = 1;
        /* our own 8-bit screen (wup_ram.h), the UI layer switched to it the way the stock firmware ran its own (format
           0x00FF0008: 8-bit paletted, the stride in bytes) */
        memset((void *)WUP_FB8, 0, 864u * GFX_H); cache_clean((void *)WUP_FB8, 864u * GFX_H);
        wup_vblank(); LCD(0x70) = 864; LCD(0x74) = WUP_FB8; LCD(0xb0) = 0x00ff0008u;
    }
    return (u8 *)LCD(0x74);
}
u32 wup_stride8(void) { return LCD(0x70); }
void wup_palette(int i, u32 rgb) { LCD(0x100) = (u32)i; LCD(0x104) = 0xff000000u | rgb; }
void wup_present8(int y, int h) { u32 s = LCD(0x70); cache_clean((u8 *)LCD(0x74) + (u32)y * s, (u32)h * s); fade_in_now(); }

/* ---- audio: the kernel's (wupk.h): it keeps the ring at 48 kHz, clears what has been played and drops what is left
   when the app ends. The volume slider applies (the kernel) */
int wup_audio_open(void) { return WUPK->audio_room() > 0 ? 0 : -1; }
int wup_audio_queue(const int16_t *lr, int frames) { return WUPK->audio_queue(lr, frames); }
int wup_audio_queued(void) { return WUPK->audio_queued(); }
int wup_audio_room(void) { return WUPK->audio_room(); }
void wup_audio_close(void) { WUPK->audio_stop(); }

/* Each button's change is taken at once, and then that button is not listened to for 25 ms: a contact that bounces as
   it closes or opens is one press, not two, and nothing is delayed. */
unsigned wup_buttons(void) {                         /* (a change counts once it has held 25 ms: no bounce) */
    static unsigned stable; static uint32_t at[19];
    u8 x = REPORT[80];                             /* the stick clicks and TV (report byte 80) */
    unsigned raw = (unsigned)(BUTTONS[0] << 8 | BUTTONS[1]) | (x & 0x80 ? WUP_L3 : 0) | (x & 0x40 ? WUP_R3 : 0) | (x & 0x20 ? WUP_TV : 0);
    unsigned diff = (raw ^ stable) & 0x7ffff;
    uint32_t now = wup_us();
    for (int i = 0; diff; i++, diff >>= 1)
        if ((diff & 1) && (!at[i] || now - at[i] > 25000)) { stable ^= 1u << i; at[i] = now | 1; }
    return stable;
}
unsigned wup_pressed(void) {
    static unsigned last;
    unsigned b = wup_buttons(), p = b & ~last;
    last = b;
    return p;
}
unsigned wup_repeat(void) {
    static unsigned last; static uint32_t since[4], at[4];
    static const unsigned dirs[4] = { WUP_UP, WUP_DOWN, WUP_LEFT, WUP_RIGHT };
    unsigned b = wup_buttons(), r = b & ~last;
    uint32_t now = wup_us() / 1000;
    for (int i = 0; i < 4; i++) {
        if (r & dirs[i]) { since[i] = at[i] = now; continue; }
        if ((b & dirs[i]) && now - since[i] >= 400 && now - at[i] >= 80) { r |= dirs[i]; at[i] = now; }
    }
    last = b;
    return r;
}
void wup_motion(wup_motion_t *m) {
    const volatile u8 *r = REPORT;
    for (int i = 0; i < 3; i++) {
        m->accel[i] = (int16_t)(r[15 + 2 * i] | r[16 + 2 * i] << 8);
        int g = r[21 + 3 * i] | r[22 + 3 * i] << 8 | r[23 + 3 * i] << 16;
        m->gyro[i] = g << 8 >> 8;                     /* (24 bits, signed) */
        m->mag[i] = (int16_t)(r[30 + 2 * i] | r[31 + 2 * i] << 8);
    }
}
int wup_volume(void) { return REPORT[14]; }
int wup_power_ac(void) { return REPORT[4] & 1; }
int wup_headphones(void) { return (REPORT[4] >> 5) & 1; }
void wup_power_off(void) { WUPK->power_off(); }
int wup_stick(int which, int axis) {
    int i = which * 2 + axis, off = 6 + 2 * i;
    return (int)(REPORT[off] | REPORT[off + 1] << 8) - 0x800 - stick_rest[i & 3];
}

/* ---- the motor, the brightness, the microphone (src/wup_hw.c) */
static int rumbling, light_set, mic_on;
void wup_rumble(int on) { rumbling = !!on; hw_rumble(rumbling); }
int wup_backlight_level(void) {
    int l = WUP_HANDOFF->magic == WUP_HANDOFF_MAGIC ? WUP_HANDOFF->light : 0;
    return l >= 1 && l <= 101 ? l - 1 : 70;
}
void wup_backlight(int percent) {
    if (percent < 0 || percent > 100) return;
    hw_backlight(percent); light_set = 1;
}
int wup_mic_open(void) { hw_mic(1); mic_on = 1; return 0; }
int wup_mic_read(int16_t *out, int max) { return mic_on ? hw_mic_read(out, max) : 0; }
void wup_mic_close(void) { if (mic_on) hw_mic(0); mic_on = 0; }
static void hw_reset(void) {
    if (rumbling) wup_rumble(0);
    wup_mic_close();
    if (light_set) { hw_backlight(wup_backlight_level()); light_set = 0; }
}
/* touch: up to 10 samples in the report (+36, 4 bytes each: x, y with bit 15 = valid, 12-bit values);
   raw x 107..3980 left to right, y 3895..155 top to bottom (measured on the pad, wup-menu: menu/main.c) */
int wup_touch(int *x, int *y) {
    int n = 0, sx = 0, sy = 0;
    for (int i = 0; i < 10; i++) {
        u16 rx = (u16)(REPORT[36 + 4 * i] | REPORT[37 + 4 * i] << 8), ry = (u16)(REPORT[38 + 4 * i] | REPORT[39 + 4 * i] << 8);
        if ((rx & 0x8000) && (ry & 0x8000)) { sx += rx & 0xfff; sy += ry & 0xfff; n++; }
    }
    if (!n) return 0;
    int tx = (sx / n - 107) * GFX_W / (3980 - 107), ty = (3895 - sy / n) * GFX_H / (3895 - 155);
    *x = tx < 0 ? 0 : tx >= GFX_W ? GFX_W - 1 : tx; *y = ty < 0 ? 0 : ty >= GFX_H ? GFX_H - 1 : ty;
    return 1;
}
u32 wup_us(void) { return HWCLK; }
/* played audio is cleared behind the play position here too: the hardware loops over the ring, so a
   finished sound would otherwise repeat every 0.24 s until the app queues something new */
void (*wup__net_poll)(void), (*wup__net_exit)(void);   /* set by wup_net_init (net.c) */
void wup_sleep(int ms) {
    if (fade_pending && HWCLK - fade_t0 > fade_wait) fade_in_now();   /* never presented: show it anyway */
    if (wup__net_poll) {                               /* networking: keep the stack running while asleep */
        wup__net_poll();
        while (ms > 0) { DLY(ms > 2 ? 2 : ms); ms -= 2; wup__net_poll(); }
        return;
    }
    while (ms > 0) { DLY(ms > 100 ? 100 : ms); ms -= 100; }
}


/* ---- status: the UIC (the pad's microcontroller) through the kernel: 0x10 = battery level 0..6; the input report's
   byte 4 bit 6 = charging. The signal: the radio's WLC_GET_RSSI (127) */
static int uic_query(int cmd) { u8 c = (u8)cmd, v = 0; return WUPK->uic(&c, 1, &v, 1) ? -1 : v; }
int wup_battery(int *charging) {
    int chg = (REPORT[4] >> 6) & 1;                                  /* (the input report, byte 4) */
    if (charging) *charging = chg;
    int lv = uic_query(0x10);
    if (lv < 0 || lv > 6 || (chg && lv == 0)) return -1;               /* (level 0 while charging: unknown) */
    return (lv * 100 + 3) / 6;
}
int wup_wifi_rssi(void) {
    static int buf[8] __attribute__((aligned(32)));
    buf[0] = 0; cache_flush(buf, sizeof buf);
    int r = WUPK->wifi_ioctl(127, buf, 4, 0);
    cache_flush(buf, sizeof buf);
    return r || buf[0] >= 0 ? 0 : buf[0];
}

/* the log: the kernel's (the UART on the battery-cover header), and the same text over Wi-Fi while networking runs */
void (*wup__net_log)(const char *s);                  /* (net.c) */
void wup_log(const char *s) {
    if (wup__net_log) wup__net_log(s);
    WUPK->log(s);
}
void wup_log_hex(u32 v) { char b[9]; for (int i = 0; i < 8; i++) { int d = v >> (28 - 4 * i) & 15; b[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); } b[8] = 0; wup_log(b); }
