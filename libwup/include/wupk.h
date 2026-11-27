/* The wup-homebrew kernel's services, for libwup and the menu.
 *
 * The kernel (wup-homebrew/kernel) runs the pad: it is started by the early hook before any of Nintendo's firmware
 * runs (wup-menu/boot/early.c), and the menu and every app get the whole pad. libwup reaches the kernel through this
 * table, whose address is at WUPK_PTR; the table grows only at its end, and `version` says how far it goes. */
#pragma once
#include <stdint.h>

#define WUPK_PTR     0x00000080u
#define WUPK_MAGIC   0x4b505557u                 /* "WUPK" */
#define WUPK_VERSION 6

typedef struct wupk {
    uint32_t magic, version;
    /* tasks (a lower priority number is more urgent) and counting semaphores */
    int (*task_start)(void (*fn)(void *), void *arg, void *stack, uint32_t size, int prio, const char *name);
    void (*task_exit)(void);
    int (*task_join)(int id, int ms);            /* 0, or -1: timed out */
    void (*task_kill)(int id);                   /* stopped where it is; join it after */
    int (*task_self)(void);
    int (*task_prio)(int id);
    void (*sleep)(int ms);                       /* 0: give others a turn */
    int (*sem_new)(int count, int max);
    void (*sem_free)(int id);
    int (*sem_wait)(int id, int ms);             /* 0, or -1: timed out (ms < 0: for ever, 0: only if free now) */
    void (*sem_signal)(int id);
    /* time and input */
    const volatile uint32_t *ms;                 /* milliseconds since the kernel started */
    const volatile uint8_t *report;              /* the UIC's latest 128-byte input report (as Nintendo's at 0x178D38) */
    const volatile uint32_t *report_seq;         /* counts new reports */
    /* the UIC (the pad's power microcontroller): a command and its answer (battery 0x10, EEPROM 0x03, LCD power 0x12) */
    int (*uic)(const uint8_t *out, int nout, uint8_t *in, int nin);
    void (*backlight)(int pwm);                  /* 0..255 (the panel's own PWM; 122: the console's middle) */
    void (*reboot)(void);                        /* Nintendo's start again: stock, or whatever the buttons pick */
    void (*power_off)(void);
    /* the SPI flash: 0, or < 0 (writes: 4 KB-aligned erases; never below 0xF000) */
    int (*flash_read)(uint32_t addr, void *p, uint32_t n);
    int (*flash_write)(uint32_t addr, const void *p, uint32_t n);   /* program (the sectors erased first) */
    int (*flash_erase4k)(uint32_t addr);
    /* Wi-Fi: the radio up (its firmware from Nintendo's bank), Broadcom's ioctls and iovars, Ethernet frames */
    int (*wifi_up)(void);
    int (*wifi_ioctl)(uint32_t cmd, void *buf, uint32_t len, int set);
    int (*wifi_var)(const char *name, void *v, uint32_t n, int set);
    int (*wifi_join)(const char *ssid, const uint8_t *pmk);         /* pmk: 32 bytes (WPA2), or NULL (open) */
    void (*wifi_leave)(void);
    int (*wifi_state)(void);                     /* 0 off, 1 joining, 2 connected, 3 failed, 4 wrong password */
    int (*wifi_tx)(const void *eth, uint32_t len);
    int (*wifi_tx_room)(void);
    void (*wifi_hooks)(void (*rx)(const uint8_t *eth, uint32_t len), void (*link)(int up));
    const uint8_t *mac;                          /* the radio's MAC, once it is up */
    /* the log (the UART) */
    void (*log)(const char *s);
    /* version 2: the screen. The kernel shows one RGB565 buffer (WUP_FB0 or WUP_FB1, wup_ram.h); the switch happens at
       the screen's vertical blank, so a buffer is never shown half drawn */
    void (*screen_show)(const void *fb);         /* fb shown from the next refresh on; returns once it is (the other
                                                    buffer is then free to draw into) */
    void (*vblank_wait)(void);                   /* until the next refresh (59.94 a second) */
    const volatile uint32_t *vblanks;            /* counts refreshes */
    const void *(*screen_shown)(void);           /* the buffer on the screen now */
    /* version 3: sound, the microphone and the rumble motor (all stopped whenever an app ends). Sound: 48 kHz 16-bit
       stereo frames into the kernel's ring (about 0.2 s ahead); the volume slider applies */
    int (*audio_queue)(const int16_t *lr, int frames);   /* frames taken (up to audio_room) */
    int (*audio_room)(void);
    int (*audio_queued)(void);                   /* frames not yet played */
    void (*audio_stop)(void);                    /* everything queued dropped */
    int (*mic_open)(void);                       /* 16-bit mono, 48 kHz; 0, or < 0 */
    int (*mic_read)(int16_t *out, int max);      /* the samples since the last read (up to max); read at least every
                                                    20 ms or the ring overruns */
    void (*mic_close)(void);
    void (*rumble)(int on);
    /* version 4: video and the camera (all off again whenever an app ends).
       Video: the hardware H.264 decoder shows its pictures itself (854 x 480; each frame's raw CABAC slice data, as
       libwupstream makes it). Its memory is the screens': while video is open nothing may be drawn or shown. */
    int (*video_open)(void);
    int (*video_frame)(const void *data, uint32_t n, int keyframe);   /* 0, or < 0: dropped (send a keyframe) */
    int (*video_need_key)(void);                 /* nonzero: only a keyframe can go on (lost frames, a stall) */
    int (*video_waiting)(void);                  /* frames not yet decoded (keep it at 1..3) */
    void (*video_close)(void);                   /* the screen back (both buffers to be drawn again) */
    uint32_t (*us_since_vblank)(void);           /* microseconds since the screen's last refresh */
    /* the camera's raw frames (front, 640 x 480): into buf (cam_frame_size bytes, 32-aligned, the app's, until closed) */
    uint32_t cam_frame_size;
    int (*cam_open)(uint8_t *buf);               /* 0, or < 0 */
    int (*cam_ready)(void);                      /* nonzero: a new frame is in buf, held until cam_next */
    void (*cam_next)(void);
    void (*cam_convert)(uint16_t *dst, int stride, int mirror);   /* the frame -> 640 x 480 RGB565 at dst */
    void (*cam_close)(void);
    /* the camera through the H.264 encoder, while video is open (its memory is the screens' too): about 59 frames a
       second of bare slice data; the first after every start a keyframe. Not together with cam_open */
    int (*cam_enc_open)(void);                   /* 0, or < 0 (frames ~0.2 s later) */
    int (*cam_enc_read)(uint8_t *out, uint32_t max);   /* often: a frame's length, 0 none yet, < 0 off */
    void (*cam_enc_restart)(void);               /* a new start now (a receiver that lost a frame asks for it) */
    uint32_t (*cam_enc_starts)(void);            /* starts since opened */
    void (*cam_enc_close)(void);
    /* (version 5) */
    int (*flash_erase64k)(uint32_t addr);         /* a 64 KB block (aligned): ~0.7 s, where 16 erase4k take ~3.8 s */
    /* (version 6) */
    int (*wifi_country)(const char *cc);          /* the regulatory country for the next radio bring-up (2 letters,
                                                    e.g. "DE"); it matters at WLC_UP, so a change applies from the
                                                    next bring-up. Default "DE" (the v1 menu's proven domain) */
} wupk_t;

#define WUPK (*(const wupk_t *const *)WUPK_PTR)
static inline int wupk_present(void) { return WUPK && WUPK->magic == WUPK_MAGIC; }
