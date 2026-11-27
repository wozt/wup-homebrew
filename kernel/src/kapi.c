/* The kernel's services for libwup and the menu (libwup/include/wupk.h): the table, and the locks that keep tasks off
 * each other on shared hardware. */
#include "os.h"
#include "../../libwup/include/wupk.h"

/* the SPI bus (flash and UIC) is one device: one task on it at a time (spi.c takes it around every transfer) */
static int bus = -1;
void bus_lock(void) { if (bus > 0 && sched_current() >= 0) sem_wait(bus, -1); }
void bus_unlock(void) { if (bus > 0 && sched_current() >= 0) sem_signal(bus); }

/* the radio: one task at a time in the driver (the radio task polls it; apps ask it things) */
static int radio = -1;
static void rlock(void) { if (radio > 0) sem_wait(radio, -1); }
static void runlock(void) { if (radio > 0) sem_signal(radio); }
static int radio_up;
static void radio_task(void *unused) {                  /* frames in and out, and the link's state */
    (void)unused;
    for (;;) {
        if (radio_up) { rlock(); wifi_poll(); wifi_tick(); runlock(); }
        sched_sleep(1);
    }
}
static int k_wifi_up(void) {
    rlock(); int r = wifi_up(); runlock();
    if (!r) radio_up = 1;
    return r;
}
static int k_wifi_ioctl(u32 cmd, void *b, u32 n, int set) { rlock(); int r = wifi_ioctl(cmd, b, n, set); runlock(); return r; }
static int k_wifi_var(const char *name, void *v, u32 n, int set) { rlock(); int r = wifi_var(name, v, n, set); runlock(); return r; }
static int k_wifi_join(const char *ssid, const u8 *pmk) { rlock(); int r = wifi_join(ssid, pmk); runlock(); return r; }
static void k_wifi_leave(void) { rlock(); wifi_leave(); runlock(); }
static int k_wifi_state(void) { return wifi_state; }
static int k_wifi_tx(const void *e, u32 n) { rlock(); int r = wifi_tx(e, n); runlock(); return r; }
static int k_wifi_room(void) { return wifi_tx_room(); }
static void k_wifi_hooks(void (*rx)(const u8 *, u32), void (*link)(int)) { wifi_rx_hook = rx; wifi_link_hook = link; }

static int k_uic(const u8 *o, int no, u8 *i, int ni) { return uic_cmd(o, no, i, ni); }
static void k_backlight(int pwm) { lcd_backlight(pwm < 0 ? 0 : pwm > 255 ? 255 : pwm); }
void power_off(void) {                             /* (also the Power button: input.c) */
    klog("power off\n");
    lcd_off();
    static const u8 c[2] = { 0x01, 0x04 };
    for (int i = 0;; i++) {                           /* (until the UIC has cut the power) */
        int e = uic_cmd(c, 2, NULL, 0);
        if (i < 5) klog("power off: asked (%d)\n", e);
        sched_sleep(200);
    }
}
static void k_power_off(void) { power_off(); }
static void k_reboot(void) { klog("reboot asked: ignored (the way out is holding Power)\n"); }
static int k_flash_write(u32 a, const void *p, u32 n) { return flash_program(a, p, n); }
static void k_log(const char *s) { klog("%s", s); }
static int k_self(void) { return sched_current(); }
static void k_screen_show(const void *fb) { lcd_show((u32)fb); }
static const void *k_screen_shown(void) { return (const void *)REG(0xf0009474u); }
/* the rumble motor: a pin, 0xF0005114 (high: on), as the stock firmware's vibrate module drives it */
void rumble(int on) { REG(0xf0005114u) = on ? 0xc300u : 0xc200u; }
/* video: the decoder is polled every millisecond while it is open (its queue, faults, stalls), by a task of its own */
static int video_lock = -1;
static void video_task(void *unused) {
    (void)unused;
    for (;;) {
        if (vdec_active()) { sem_wait(video_lock, -1); vdec_poll(); sem_signal(video_lock); }
        sched_sleep(1);
    }
}
static int k_video_open(void) { sem_wait(video_lock, -1); int r = vdec_open(); sem_signal(video_lock); return r; }
static int k_video_frame(const void *d, u32 n, int key) { sem_wait(video_lock, -1); int r = vdec_frame(d, n, key); sem_signal(video_lock); return r; }
static int k_video_need_key(void) { return vdec_need_key(); }
static int k_video_waiting(void) { return (int)vdec_waiting(); }
static void k_video_close(void) { sem_wait(video_lock, -1); cam_enc_stop(); if (vdec_active()) vdec_close(); sem_signal(video_lock); }
static u32 k_us_since_vblank(void) { return time_us() - vblank_us; }
static int k_cam_open(u8 *buf) { return vdec_active() ? -1 : cam_start(buf); }
static int k_cam_enc_open(void) { return vdec_active() ? cam_enc_start() : -1; }
static int k_cam_enc_read(u8 *out, u32 max) { sem_wait(video_lock, -1); int r = cam_enc_frame(out, max); sem_signal(video_lock); return r; }
static u32 k_cam_enc_starts(void) { return cam_enc_starts; }
/* whatever an app left on, off (app.c, when it ends) */
void video_off(void) { cam_stop(); k_video_close(); }

static const wupk_t table = {
    WUPK_MAGIC, WUPK_VERSION,
    task_start, task_exit, task_join, task_kill, k_self, task_prio, sched_sleep,
    sem_new, sem_free, sem_wait, sem_signal,
    &ticks_ms, input_report, &input_seq,
    k_uic, k_backlight, k_reboot, k_power_off,
    flash_read, k_flash_write, flash_erase4k,
    k_wifi_up, k_wifi_ioctl, k_wifi_var, k_wifi_join, k_wifi_leave, k_wifi_state, k_wifi_tx, k_wifi_room, k_wifi_hooks,
    wifi_mac,
    k_log,
    k_screen_show, lcd_wait_vblank, &vblanks, k_screen_shown,
    audio_queue, audio_room, audio_queued, audio_stop,
    mic_open, mic_read, mic_close, rumble,
    k_video_open, k_video_frame, k_video_need_key, k_video_waiting, k_video_close, k_us_since_vblank,
    CAM_FRAME, k_cam_open, cam_ready, cam_next, cam_convert, cam_stop,
    k_cam_enc_open, k_cam_enc_read, cam_enc_restart, k_cam_enc_starts, cam_enc_stop,
    flash_erase64k,
};

void kapi_init(void) {
    bus = sem_new(1, 1);
    radio = sem_new(1, 1);
    *(const wupk_t **)WUPK_PTR = &table;
    task_start(radio_task, NULL, NULL, 8192, 6, "radio");
    video_lock = sem_new(1, 1);
    task_start(video_task, NULL, NULL, 4096, 5, "video");
}
