/* libwup (see wup.h) */
#include <setjmp.h>
#include <stdlib.h>
#include "wup.h"
#include "mmu.h"
#include "wup_hw.h"
#include "armcpu.h"
#include "netfs.h"
typedef uint32_t u32; typedef uint8_t u8; typedef uint16_t u16;
#define REG(a) (*(volatile u32 *)(a))
#define LCD(o) REG(0xf0009400u + (o))
#define HWCLK REG(0xf0000408u)
#define FW_DLY ((void (*)(int))0x00089ae4)            /* RTOS task delay (ARM code) */
#define FW_BUTTONS ((const volatile u8 *)0x000bdab0u)
#define FW_REPORT  ((const volatile u8 *)0x00178d38u)

extern u8 __bss_start[], __bss_end[], __bss2_start[], __bss2_end[];
static u32 stack[6144] __attribute__((aligned(8), section(".bss.stack")));   /* 24 KB, in the app area */
static int fs_state, screen_on, lcd8_saved, ret;
static u32 lcd8[3];
void wup__kernel_exit(void);
void wup__time_start(void), wup__time_end(void);
static void copy_boot(void);
static void uart_init(void);
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
static void fade_in_now(void) { if (fade_pending) { fade_pending = 0; gfx_fade(0, 256); } }

__attribute__((noinline)) static void run_on_stack(void (*f)(void), void *top) {
    __asm__ volatile("mov r4, sp\n\tmov sp, %1\n\tblx %0\n\tmov sp, r4\n\t"
                     :: "r"(f), "r"(top) : "r0", "r1", "r2", "r3", "r4", "r12", "lr", "memory", "cc");
}
static void app_main(void) {
    /* our own memory, cached while we run (ui/mmu.c restores the firmware's mapping) */
    mmu_cache(0x350000, 0x380000);                     /* code, data, stack */
    mmu_cache(0x190000, 0x200000);                     /* big memory: 64 KB pages of a coarse table */
    mmu_cache_section(0x200000, 0x2fa000);             /* big memory in the 2-3 MB section; not the Wi-Fi driver's above it */
    mmu_cache(0x390000, 0x3e0000);                     /* most of the firmware's 8bpp framebuffer */
    /* apps fade in and out: black until the first frame is shown (fade_in_now), fade out on return */
    gfx_brightness(0);
    fade_pending = 1; gfx_present_hook = fade_in_now;
    uart_init();
    copy_boot();
    for (int i = 0; i < 4; i++) {                      /* the sticks at rest: each pad's are a little off centre */
        int v = (int)(FW_REPORT[6 + 2 * i] | FW_REPORT[7 + 2 * i] << 8) - 0x800;
        stick_rest[i] = v > -300 && v < 300 ? v : 0;
    }
    int formatted = 0;
    fs_state = fs_mount(&formatted) == 0;              /* built with FS_NO_FORMAT: never formats */
    wup__time_start();
    extern int wup__big_load(void) __attribute__((weak));   /* (big.c: only in apps with code for WUP_BIGMEM) */
    if (wup__big_load && wup__big_load()) ret = 1;
    else if (!setjmp(exit_jb)) { fade_t0 = HWCLK; ret = main(); }   /* (exit() and abort() come back here; the fade's 0.3 s
                                                       count from here: mounting a fresh filesystem can take longer) */
    wup__time_end();
    wup_audio_close();                                 /* (first: its thread) */
    netfs_close_all();                                /* /net: the connection closed before lwIP's threads stop */
    if (wup__net_exit) wup__net_exit();                /* sockets closed, lwIP's threads stopped, the radio's frames dropped
                                                          again: first, as lwIP's threads never end (waiting for them
                                                          below cost every networked app a second to close) */
    wup__kernel_exit();                                /* the app's threads joined, semaphores given back */
    hw_reset();                                        /* the motor and the microphone off, the menu's brightness */
    gfx_present_hook = 0;
    if (!fade_pending) gfx_fade(256, 0);               /* fade out (unless it never faded in) */
    if (screen_on) gfx_end();                          /* (clears the firmware layer to black) */
    if (lcd8_saved) {                                  /* the firmware layer still shows our last frame: black it */
        u8 *f = (u8 *)LCD(0x74); u32 n = LCD(0x70) * GFX_H;
        for (u32 i = 0; i < n; i += 4) *(u32 *)(f + i) = 0;
        cache_clean(f, n);
        LCD(0x100) = 0; LCD(0x104) = 0xff000000u;
        wup_vblank(); LCD(0xb0) = lcd8[0]; LCD(0x70) = lcd8[1]; LCD(0x74) = lcd8[2];
    }
    gfx_brightness(256);                               /* the normal curve again: the screen is black */
    if (exec_path[0]) ret = stage_exec() ? 0 : WUP_HANDOFF_RUN;   /* the screen's memory is free now: the next app into it */
}
/* The loader jumps here (it must be in the first segment, 0x350000..). A large app's second segment
   (WUP_SPARE) was written by the menu through the D-cache: write the whole D-cache back and drop the
   I-cache before any of it runs (ARM926 test-and-clean). */
__attribute__((section(".text.entry"), target("arm"), noinline)) int _wup_entry(void) {
    __asm__ volatile("1: mrc p15, 0, apsr_nzcv, c7, c10, 3\n\tbne 1b\n\t"
                     "mcr p15, 0, %0, c7, c10, 4\n\tmcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory", "cc");
    for (u8 *p = __bss_start; p < __bss_end; p++) *p = 0;
    for (u32 *p = (u32 *)__bss2_start; p < (u32 *)__bss2_end; p++) *p = 0;   /* large apps: .bss in WUP_BIGMEM */
    run_on_stack(app_main, stack + 6144);
    mmu_uncache_all();                                 /* back on the loader's stack, outside our pages */
    return ret;
}

/* ---- starting another app: its code (as stored in its .wup, deflated) goes to the stage for the loader, once this app
   has ended and the screen's memory is free (include/handoff.h) */
#include "wupfmt.h"
static char exec_arg[192];
static wup_hdr_t exec_hdr;
static int exec_pre;                                  /* the image is already at WUP_HANDOFF_STAGE: stage_exec only names it */
int wup_exec(const char *path, const char *arg) {
    wup_hdr_t *h = &exec_hdr;
    memset(h, 0, sizeof *h);
    int n = fs_read(path, 0, h, sizeof *h);
    if (n < WUP_HDR_V1 || h->magic != WUP_MAGIC || !h->code_size || h->code_size > 0xd9000u) return -1;
    if (h->hdr_size < WUP_HDR_V3) h->code_raw = 0;
    if (h->code_raw > WUP_HANDOFF_WORK_MAX) return -2;
    strncpy(exec_path, path, sizeof exec_path - 1);
    strncpy(exec_arg, arg ? arg : "", sizeof exec_arg - 1);
    exec_pre = 0;
    return 0;
}
/* an image already written at WUP_HANDOFF_STAGE (the menu streaming an app from /net): the loader runs it like a
   staged one; path is what the app sees as its own (its folder is where its saves go), size/raw as stage_exec's */
int wup_exec_staged(const char *path, uint32_t size, uint32_t raw) {
    if (!path[0] || !size) return -1;
    exec_hdr.code_size = size; exec_hdr.code_raw = raw;
    strncpy(exec_path, path, sizeof exec_path - 1);
    exec_arg[0] = 0;
    exec_pre = 1;
    return 0;
}
static int stage_exec(void) {
    volatile wup_handoff_t *pb = WUP_HANDOFF;
    u8 *dst = (u8 *)WUP_HANDOFF_STAGE;
    if (exec_pre) {                                    /* already there: name it for the app, keep the stage as is */
        cache_clean(dst, exec_hdr.code_size);
        pb->stage_size = exec_hdr.code_size; pb->stage_raw = exec_hdr.code_raw;
        for (int i = 0; i < 64; i++) pb->app_path[i] = i < 63 ? exec_path[i] : 0;
        for (int i = 0; i < 192; i++) pb->arg[i] = i < 191 ? exec_arg[i] : 0;
        return 0;                                    /* staged already: 0 = run it (like the fs path's success) */
    }
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
    if (!lcd8_saved) { lcd8[0] = LCD(0xb0); lcd8[1] = LCD(0x70); lcd8[2] = LCD(0x74); lcd8_saved = 1; }
    return (u8 *)LCD(0x74);
}
u32 wup_stride8(void) { return LCD(0x70); }
void wup_palette(int i, u32 rgb) { LCD(0x100) = (u32)i; LCD(0x104) = 0xff000000u | rgb; }
void wup_present8(int y, int h) { u32 s = LCD(0x70); cache_clean((u8 *)LCD(0x74) + (u32)y * s, (u32)h * s); fade_in_now(); }

/* ---- audio: the audio block (0xF0005400) loops over our ring at 48 kHz (measured on the pad: 192000 bytes a second);
   we write just ahead of the play position, and what has been played is cleared at once, or the ring would play it
   again 0.24 s later. That clearing, and noticing when everything queued has been played, is done by a small thread
   of its own every 5 ms, so it happens even while the app is busy (the menu bringing up the radio, a long drawing).
   Its PLAY_CNT (+0x20) bit 0 mutes; bit 2 (STOP) wedges an idle engine and is never used. The ring is in the
   loader's slot (uncached, DMA-reachable, unused by the loader while the app runs). */
#define AU(o) REG(0xf0005400u + (o))
#define RING ((int16_t *)0x00342000u)
#define RFRAMES 11520                                  /* 0.24 s: 0x342000..0x34D000 */
#define RBYTES (RFRAMES * 4)
#define LEAD 1600                                      /* a new sound starts this far ahead of the play position (33 ms) */
#define ROOM (RFRAMES - LEAD - 480)                    /* never more queued than this: the keeper's 5 ms and a margin */
static volatile int au_on, au_run, au_clr, au_wr = -1, au_left;     /* au_wr < 0: nothing queued */
static int au_keeper = -1;
static int au_pos(void) { u32 p = (AU(0x14) & 0xffffff) - (u32)RING; return p < RBYTES ? (int)(p >> 2) : -1; }
static int ring_dist(int a, int b) { int d = b - a; return d < 0 ? d + RFRAMES : d; }   /* a -> b */
static void au_tick(void) {                            /* (interrupts off) clear what was played; see the end of the queue */
    int p = au_pos(); if (p < 0) return;
    u32 *r = (u32 *)RING;
    while (au_clr != p) { r[au_clr] = 0; if (++au_clr == RFRAMES) au_clr = 0; }
    if (au_wr >= 0) {
        int d = ring_dist(p, au_wr);
        if (d > au_left) au_wr = -1;                   /* it went up: the play position has passed the end */
        else au_left = d;
    }
}
static void au_keep(void *arg) {
    (void)arg;
    while (au_run) { unsigned st = wup_irq_off(); au_tick(); wup_irq_restore(st); wup_thread_sleep(5); }
}
int wup_audio_open(void) {
    if (au_on) return 0;
    AU(0x2c) = 0; AU(0x30) = 0x3f;                     /* the firmware's refill off */
    AU(0x20) = (AU(0x20) & ~4u) | 1;                   /* muted while we set up */
    memset(RING, 0, RBYTES);
    AU(0x00) &= ~(7u << 22);                           /* PCM16, 48 kHz */
    AU(0x04) &= ~0x80u;                                /* stereo */
    AU(0x08) = (u32)RING; AU(0x0c) = (u32)RING + RBYTES;
    AU(0x24) = RBYTES >> 4;
    AU(0x10) = (u32)RING + RBYTES;                     /* loop over the ring */
    wup_sleep(4); int p0 = au_pos(); wup_sleep(4); int p1 = au_pos();
    if (p0 < 0 || p1 < 0 || p0 == p1) return -1;       /* not playing our ring: stay muted */
    au_clr = p1; au_wr = -1; au_left = 0; au_on = 1;
    au_run = 1; au_keeper = wup_thread_start(au_keep, 0, 2048, 2);
    AU(0x20) &= ~5u;
    return 0;
}
int wup_audio_queued(void) {
    if (!au_on) return 0;
    unsigned st = wup_irq_off(); au_tick(); int n = au_wr < 0 ? 0 : au_left; wup_irq_restore(st);
    return n;
}
int wup_audio_room(void) { return au_on ? ROOM - wup_audio_queued() : 0; }
int wup_audio_queue(const int16_t *lr, int n) {
    if (!au_on || n <= 0) return 0;
    unsigned st = wup_irq_off();
    au_tick();
    int p = au_pos();
    if (p < 0) { wup_irq_restore(st); return 0; }
    if (au_wr < 0) { au_wr = p + LEAD >= RFRAMES ? p + LEAD - RFRAMES : p + LEAD; au_left = LEAD; }
    if (n > ROOM - au_left) n = ROOM - au_left;
    const u32 *s = (const u32 *)lr; u32 *r = (u32 *)RING;
    for (int i = 0; i < n; i++) { r[au_wr] = s[i]; if (++au_wr == RFRAMES) au_wr = 0; }
    au_left += n;
    wup_irq_restore(st);
    return n;
}
void wup_audio_close(void) {
    if (!au_on) return;
    au_run = 0;
    if (au_keeper >= 0 && wup_thread_join(au_keeper, 100)) wup_thread_kill(au_keeper);
    au_keeper = -1;
    AU(0x20) |= 1;                                     /* mute; the engine keeps looping over silence */
    memset(RING, 0, RBYTES);
    au_on = 0; au_wr = -1;
}

/* Each button's change is taken at once, and then that button is not listened to for 25 ms: a contact that bounces as
   it closes or opens is one press, not two, and nothing is delayed. */
unsigned wup_buttons(void) {
    static unsigned stable; static uint32_t at[16];
    unsigned raw = (unsigned)(FW_BUTTONS[0] << 8 | FW_BUTTONS[1]), diff = (raw ^ stable) & 0xffff;
    uint32_t now = wup_us();
    for (int i = 0; diff; i++, diff >>= 1)
        if ((diff & 1) && (!at[i] || now - at[i] > 25000)) { stable ^= 1u << i; at[i] = now | 1; }
    return stable;
}
int wup_stick(int which, int axis) {
    int i = which * 2 + axis, off = 6 + 2 * i;
    return (int)(FW_REPORT[off] | FW_REPORT[off + 1] << 8) - 0x800 - stick_rest[i & 3];
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
        u16 rx = (u16)(FW_REPORT[36 + 4 * i] | FW_REPORT[37 + 4 * i] << 8), ry = (u16)(FW_REPORT[38 + 4 * i] | FW_REPORT[39 + 4 * i] << 8);
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
        while (ms > 0) { FW_DLY(ms > 2 ? 2 : ms); ms -= 2; wup__net_poll(); }
        return;
    }
    while (ms > 0) { FW_DLY(ms > 100 ? 100 : ms); ms -= 100; }
}


/* ---- status (as the Homebrew Menu reads it, wup-menu: menu/ui/sys.c and wifi_hw.c). The UIC (the pad's microcontroller)
   answers one-byte queries through the firmware's driver: state block at 0x13434 (byte 0 = 1 when ready,
   +4 = driver context, whose +0x10 is the command buffer), semaphore 0x1b (wai_sem 0x8b64c / sig_sem
   0x8ae1e), transfer 0x115f4(ctx, ntx, nrx, rx). UIC 0x10 = battery level 0..6; the input report's byte 4
   bit 6 = charging. The signal: the radio driver's ioctl 0x1b164(cmd, buf, len, set), WLC_GET_RSSI = 127. */
#define WAI_SEM  ((int (*)(int, int))0x0008b64d)
#define SIG_SEM  ((int (*)(int))0x0008ae1f)
#define UIC_XFER ((int (*)(u32, int, int, u8 *))0x000115f5)
#define WL_IOCTL ((int (*)(u32, void *, u32, u32))0x0001b165)
static int uic_query(int cmd) {
    u32 st = *(volatile u32 *)0x00013434u;
    if (!st || *(volatile u8 *)st != 1) return -1;
    u32 ctx = *(volatile u32 *)(st + 4);
    if (!ctx || WAI_SEM(0x1b, -1)) return -1;
    u8 *buf = *(u8 **)(ctx + 0x10);
    buf[0] = (u8)cmd;
    UIC_XFER(ctx, 1, 1, buf + 1);
    int v = buf[1];
    SIG_SEM(0x1b);
    return v;
}
int wup_battery(int *charging) {
    int chg = (*(const volatile u8 *)(0x00178d38u + 4) >> 6) & 1;      /* (the input report, byte 4) */
    if (charging) *charging = chg;
    int lv = uic_query(0x10);
    if (lv < 0 || lv > 6 || (chg && lv == 0)) return -1;               /* (level 0 while charging: unknown) */
    return (lv * 100 + 3) / 6;
}
int wup_wifi_rssi(void) {
    static int buf[8] __attribute__((aligned(32)));
    buf[0] = 0; cache_flush(buf, sizeof buf);
    int r = WL_IOCTL(127, buf, 4, 0);
    cache_flush(buf, sizeof buf);
    return r || buf[0] >= 0 ? 0 : buf[0];
}

/* UART1 (the battery-cover header), 115200 8N1, as the firmware's boot ROM loader leaves it wired; the stock
   firmware does not use it. Set up once per app: every wait has a timeout, so nothing hangs without a cable. */
static void uart_init(void) {
    REG(0xf00050d4) = 0x8001; REG(0xf00050d8) = 0x0801;           /* pins: UART1 TX / RX */
    REG(0xf0000058) |= 1u << 8; REG(0xf0000044) = 8 - 1; REG(0xf0000058) &= ~(1u << 8);   /* reset; 32 MHz / 8 */
    REG(0xf0004c70) = 1;
    REG(0xf0004c64) = 1; REG(0xf0004c68) = 288; REG(0xf0004c6c) = 625;   /* 115200 baud */
    REG(0xf0004c50) = 0; REG(0xf0004c54) = 3; REG(0xf0004c58) = 0; REG(0xf0004c48) = 0;   /* 8N1, no interrupts */
}
static void uputc(char c) {
    for (int i = 0; i < 100000 && !(REG(0xf0004c5c) & 0x20); i++) ;
    REG(0xf0004c44) = (u8)c;
}
void (*wup__net_log)(const char *s);                  /* networking: the same text over Wi-Fi (net.c) */
void wup_log(const char *s) {
    if (wup__net_log) wup__net_log(s);
    while (*s) { if (*s == '\n') uputc('\r'); uputc(*s++); }
}
void wup_log_hex(u32 v) { char b[9]; for (int i = 0; i < 8; i++) { int d = v >> (28 - 4 * i) & 15; b[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); } b[8] = 0; wup_log(b); }
