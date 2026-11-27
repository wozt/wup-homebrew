/* The wup-homebrew kernel: the pad's own drivers and tasks under the Homebrew Menu and its apps, started by the early
 * hook before any of Nintendo's firmware runs (wup-menu/boot/early.c). Its services for apps: kapi.c (libwup's wupk.h). */
#pragma once
#include "types.h"
#include "ram.h"

#define IMAGE_BASE  0x10000u
#define HOOK_ARG    0x4b4f4f48u                 /* "HOOK": in r0 from the early hook */

/* lib.c */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
size_t strlen(const char *s);
char *strchr(const char *s, int c);
char *strcpy(char *d, const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
size_t strspn(const char *s, const char *a);
size_t strcspn(const char *s, const char *r);
int inflate_raw(unsigned char *dst, unsigned *n, const unsigned char *src, unsigned len);   /* zinflate.c: 0 = inflated */
void *kmalloc(size_t n);                     /* heap.c */
void kfree(void *p);
u32 kheap_free(u32 *largest);
typedef struct { void *free; } heap_t;
void heap_reset(heap_t *h, u32 start, u32 end);
void *heap_alloc(heap_t *h, u32 n);
void heap_free(heap_t *h, void *p);
u32 heap_avail(heap_t *h, u32 *largest);
int memcmp(const void *a, const void *b, size_t n);
u32 crc32(const void *p, u32 n);                /* (zlib's) */
u32 crc32_more(u32 crc, const void *p, u32 n);
int snprintf(char *b, size_t n, const char *f, ...);
typedef __builtin_va_list va_list;
int vsnprintf(char *b, size_t n, const char *f, va_list ap);

/* board.c: the stock firmware's board init (fw_init.c), the chip revision, time */
void board_init(void);
u32 soc_rev(void);
u32 time_us(void);                    /* the free-running ~1 MHz counter (0xF0000408) */
void delay_us(u32 us);
void delay_ms(u32 ms);
extern volatile u32 ticks_ms;         /* 1 kHz timer tick (irq.c) */

/* irq.c */
typedef void (*irq_fn)(int n);
void irq_init(void);
void irq_attach(int n, int prio, irq_fn f);
void irq_mask(int n);
void irq_quiesce(void);
void irq_on(void);
void irq_off(void);
void timer_init(void);                /* timer 1: the 1 kHz tick */

/* mmu.c */
void mmu_init(void);
void dcache_clean(const void *p, u32 n);
void dsb(void);

/* uart.c: UART1, the battery-cover header (115200 8N1) */
void uart_init(void);
void klog(const char *f, ...);
void uart_write(const void *p, u32 n);          /* raw bytes */
int uart_getc(u32 timeout_us);                  /* a received byte, or -1 */
void uart_rx_poll(void);                        /* (the timer tick) */
extern volatile u32 uart_overruns, uart_lost;

/* input.c: the UIC's input report, read every few ms by a task of its own (libwup reads it as it read Nintendo's) */
#include "input.h"
extern u8 input_report[128];
extern volatile u32 input_seq;
void input_start(void);

/* sched.c: tasks and semaphores */
int task_start(void (*fn)(void *), void *arg, void *stack, u32 size, int prio, const char *name);
void task_exit(void);
int task_join(int id, int ms);
void task_reap(int id);
void task_kill(int id);
int task_prio(int id);
int sched_current(void);
void sched_sleep(int ms);
void sched_yield(void);
void sched_start(void);
void sched_tick(void);
void sched_dump(void);
int sem_new(int count, int max);
void sem_free(int id);
int sem_wait(int id, int ms);                   /* 0, or -1: timed out (ms < 0: for ever, 0: only if free now) */
void sem_signal(int id);
void bus_lock(void);                            /* the SPI bus (flash, UIC) and I2C: one task at a time (kapi.c) */
void bus_unlock(void);

/* app.c: the Homebrew Menu and its apps, one after another */
void apps_run(void *unused);
void devload_start(void);                       /* devload.c */

/* sdio.c, wifi.c: the Wi-Fi radio (Broadcom BCM4319 on SDIO) */
int sdio_init(void);
void sdio_abort(int fn);
const char *sdio_state(void);
int sdio_cmd(int idx, u32 arg, int flags, u32 *resp);
int sdio_read8(int fn, u32 addr, u8 *v);
int sdio_write8(int fn, u32 addr, u8 v);
int sdio_rw(int write, int fn, u32 addr, int inc, void *p, u32 n, u32 blksz);
typedef struct { char ssid[33]; u8 bssid[6]; u8 channel, secure; s16 rssi; } wifi_net_t;
int wifi_up(void);
int wifi_set_country(const char *cc);            /* the regulatory country for the bring-up (2 letters) */
int wifi_scan(wifi_net_t *out, int max);
int wifi_ioctl(u32 cmd, void *buf, u32 len, int set);
int wifi_var(const char *name, void *v, u32 n, int set);
extern u8 wifi_mac[6];
enum { WIFI_OFF, WIFI_JOINING, WIFI_CONNECTED, WIFI_FAILED, WIFI_BADPASS };
extern int wifi_state;
extern char wifi_ssid[33];
extern u32 wifi_chan_frames[4], wifi_events[128];
extern u32 wifi_rx_frames, wifi_tx_frames, wifi_tx_stalls, net_rx_drops, wifi_tx_drops, wifi_fc_seen;
u32 wifi_seq(void);
int wifi_join(const char *ssid, const u8 *pmk);  /* pmk NULL: an open network */
extern void (*wifi_rx_hook)(const u8 *eth, u32 len);   /* frames for the app's network stack (kapi.c) */
extern void (*wifi_link_hook)(int up);
void wifi_leave(void);
void wifi_poll(void);
void wifi_tick(void);
int wifi_tx(const void *eth, u32 len);
int wifi_tx_room(void);

/* audio.c: 48 kHz stereo to the speaker */
int audio_init(void);
void audio_tick(void);                          /* (the 1 kHz tick) */
int audio_queue(const short *lr, int frames);   /* frames taken */
int audio_room(void);
int audio_queued(void);
void audio_stop(void);
void audio_volume(int slider);
int mic_open(void);
int mic_read(short *out, int max);              /* 48 kHz mono samples since the last read */
void mic_close(void);
void rumble(int on);
void video_off(void);                                   /* video and the camera off (an app ended) */

/* the scratch region: 1 MB nobody else uses (big transfers: the shell's update) */
/* spi.c: the SPI bus (UIC, flash) */
int spi_xfer(u32 cs, const u8 *out, u32 nout, const u8 *out2, u32 nout2, u8 *in, u32 nin);

/* flash.c: the SPI flash (writes only in our free gap) */
int flash_init(u8 id[3]);
void flash_done(void);
int flash_read(u32 addr, void *p, u32 n);
int flash_may_write(u32 addr, u32 n);
int flash_erase4k(u32 addr);
int flash_erase64k(u32 addr);
int flash_program(u32 addr, const void *p, u32 n);

/* uic.c: the UIC (buttons, sticks, touch, battery, power), over SPI */
int uic_cmd(const u8 *out, int nout, u8 *in, int nin);
extern u8 uic_report[128];
int uic_poll(void);
int uic_cmd_mode(u8 cmd, u8 *in, int nin, int mode);   /* diagnostics: 0 FIFO in chunks of 8, 1 DMA */                   /* refresh uic_report (command 0x07); 0 if it is good */
void uic_lcd_power(int on);

/* i2c.c: the Renesas-style controller, channel 3 (LCD panel 0x39, camera 0x21, codec 0x18) */
void i2c_init(void);
int i2c_write(int addr, const u8 *p, int n);
int i2c_probe(int addr, u32 *err, u32 *bus);
int i2c_read(int addr, u8 reg, u8 *p, int n);

/* lcd.c */
void lcd_init(int pwm, int panel_setup);   /* panel_setup 0: the panel is up already (after a normal boot) */
void lcd_wait_vblank(void);
void lcd_off(void);
void power_off(void);
void lcd_backlight(int pwm);                    /* the panel's PWM, 0..255 */
void lcd_show(u32 addr);
extern volatile u32 vblanks, vblank_us;

/* radio.c: the radio's firmware from Nintendo's bank, patched as read (a WFW1 package) */
int radio_size(void);                             /* the package's length, or < 0 */
int radio_read(u32 off, void *p, u32 n);

/* exc.c */
void fault(int kind, u32 *regs, u32 spsr);

/* vdec.c: the H.264 decoder and the video layer (raw CABAC slice data, as libwupstream sends it). Its memory, and the
   camera encoder's (camera.c), is the stock's layout, 0x285000-0x3FE310: over the screens (wup_ram.h), so nothing is
   drawn while video is open */
int vdec_open(void);
int vdec_frame(const void *data, u32 len, int idr);    /* 0, or < 0 dropped (then wait for a keyframe) */
void vdec_poll(void);                                   /* often (the video task, kapi.c): the queue and the stall check */
int vdec_need_key(void);
int vdec_active(void);
void vdec_fault(const char *what, u32 bits);             /* both units reset: the camera starts again after a keyframe */
u32 vdec_waiting(void);                                 /* frames not yet decoded */
void vdec_close(void);                                  /* the UI layer back: redraw both screens */
extern u32 vdec_frames, vdec_keys, vdec_stalls, vdec_resets;
extern u32 eng_keep;                                    /* ENG +0x00 bits that read back 0 but must stay set (vdec.c) */
/* camera.c: the front camera (OV7740), raw frames or through the H.264 encoder */
#define CAM_FRAME 0x70800u                              /* bytes in a frame buffer (codec 4:2:0 layout) */
int cam_start(u8 *buf);                                 /* 0, or < 0; buf: CAM_FRAME bytes, 32-aligned */
int cam_ready(void);                                    /* nonzero: a new frame is in buf (VIN idle until cam_next) */
void cam_next(void);
void cam_convert(u16 *dst, int stride, int mirror);    /* the frame -> 640x480 RGB565 */
void cam_stop(void);
int cam_regs(u8 *out);
extern u32 cam_frames;
int cam_enc_start(void);                                /* the camera through the H.264 encoder (also beside the decoder) */
int cam_enc_frame(u8 *out, u32 max);                    /* often: a frame's length, 0, or < 0 (off) */
void cam_enc_down(void);                                 /* (vdec.c's fault reset) the encoder and capture off, waiting */
void cam_enc_restart(void);                              /* a new start: its first frame stands alone */
void cam_enc_stop(void);
extern u32 cam_enc_err, cam_enc_starts, cam_enc_kbps;
