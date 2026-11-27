/* LVGL's display and input drivers for the GamePad (wup_lvgl.h) */
#include <stdlib.h>
#include <string.h>
#include "wup_lvgl.h"

static uint32_t ms_, last_us, frac;
static uint32_t tick(void) { uint32_t c = wup_us(); frac += c - last_us; last_us = c; ms_ += frac / 997; frac %= 997; return ms_; }

/* LVGL draws each changed area complete in a buffer of its own, then it is copied to the screen: drawn straight
   into the screen ("direct" mode), the screen showed every step (the background, then the text on it) and flickered
   while a list scrolled. Flushing is the copy, then writing it out of the D-cache. */
static px_t *fb;
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    const px_t *s = (const px_t *)px;
    for (int y = 0; y < h; y++) memcpy(fb + (a->y1 + y) * GFX_W + a->x1, s + y * w, (size_t)w * sizeof(px_t));
    if (lv_display_flush_is_last(d)) gfx_present(a->x1, a->y1, w, h);
    else {                                             /* (libwup fades the app in at its first present: only once the */
        void (*hook)(void) = gfx_present_hook;         /*  whole first frame is on the screen, not its first part) */
        gfx_present_hook = 0; gfx_present(a->x1, a->y1, w, h); gfx_present_hook = hook;
    }
    lv_display_flush_ready(d);
}
static void flush_direct(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    (void)px;
    gfx_present(a->x1, a->y1, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
    lv_display_flush_ready(d);
}
#define BUF_ROWS 80                                    /* a sixth of the screen (133 KB): a whole list row and more at once */
/* On the wup-homebrew kernel: LVGL draws straight into the screen not shown (both screens are its buffers: direct
   mode, double buffered), only what changed, and keeps the two alike itself (LVGL 9's sync areas); at the end of a
   frame the kernel shows it from the next refresh on. No half-drawn frame, no tearing, no copying. */
#include "wup_ram.h"
void gfx__show(px_t *buf, int y0, int y1);
static int hold;                                     /* wup_lvgl_hold: screen 1 is the stage (an app's image streaming in) */
static void flush_double(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    static int y0 = GFX_H, y1 = -1;                   /* the rows changed this frame */
    if (hold) { y0 = GFX_H; y1 = -1; lv_display_flush_ready(d); return; }   /* screen 1 is the stage: only screen 0 is drawn */
    if (a->y1 < y0) y0 = a->y1; if (a->y2 > y1) y1 = a->y2;
    if (lv_display_flush_is_last(d)) { gfx__show((px_t *)px, y0, y1); y0 = GFX_H; y1 = -1; }
    lv_display_flush_ready(d);
}
static void touch_read(lv_indev_t *i, lv_indev_data_t *d) {
    static int x, y;
    (void)i;
    d->state = wup_touch(&x, &y) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    d->point.x = x; d->point.y = y;
}
/* does the focused object want the four directions itself? */
static int wants_arrows(void) {
    lv_obj_t *o = lv_group_get_focused(lv_group_get_default());
    if (!o) return 0;
    return lv_obj_check_type(o, &lv_buttonmatrix_class) || lv_obj_check_type(o, &lv_keyboard_class) || lv_obj_check_type(o, &lv_roller_class)
        || (lv_obj_check_type(o, &lv_dropdown_class) && lv_dropdown_is_open(o)) || lv_group_get_editing(lv_group_get_default());
}
static void keys_read(lv_indev_t *i, lv_indev_data_t *d) {
    static const struct { unsigned b; uint32_t key, nav; } map[] = {
        { WUP_A, LV_KEY_ENTER, 0 }, { WUP_B, LV_KEY_ESC, 0 }, { WUP_UP, LV_KEY_UP, LV_KEY_PREV }, { WUP_DOWN, LV_KEY_DOWN, LV_KEY_NEXT },
        { WUP_LEFT, LV_KEY_LEFT, 0 }, { WUP_RIGHT, LV_KEY_RIGHT, 0 } };
    static uint32_t last = LV_KEY_ENTER;
    static int armed;                                  /* (not a button still held from before the app started) */
    unsigned b = wup_buttons();
    if (!armed) { if (b & (WUP_A | WUP_B)) b = 0; else armed = 1; }
    int sx = wup_stick(0, 0), sy = wup_stick(0, 1);    /* the left stick is the D-pad too, all four ways */
    if (sy > 600) b |= WUP_UP; else if (sy < -600) b |= WUP_DOWN;
    else if (sx > 600) b |= WUP_RIGHT; else if (sx < -600) b |= WUP_LEFT;
    (void)i;
    /* A is reported as a tap (pressed for one reading, then released, however long it is held): held, LVGL sends
       "pressing" events, and its button grids (the keyboard) take those as a finger sliding off the key and drop
       their selection, so the next direction starts from the first key again. */
    static unsigned a_was;
    unsigned a_now = b & WUP_A;
    if (a_now && a_was) b &= ~WUP_A;
    a_was = a_now;
    d->state = LV_INDEV_STATE_RELEASED;
    for (unsigned k = 0; k < sizeof map / sizeof *map; k++) if (b & map[k].b) {
        last = map[k].nav && !wants_arrows() ? map[k].nav : map[k].key;
        d->state = LV_INDEV_STATE_PRESSED;
        break;
    }
    d->key = last;
}

static lv_indev_t *touch_, *keys_;
static lv_display_t *disp;                           /* for wup_lvgl_hold: the buffers to give back */
static px_t *buf_a, *buf_b;
int wup_lvgl_hold(int on) {
    int was = hold; hold = on;
    if (disp) lv_display_set_buffers(disp, on ? (px_t *)WUP_FB0 : buf_a, on ? (px_t *)WUP_FB0 : buf_b,
                                     GFX_W * GFX_H * sizeof(px_t), LV_DISPLAY_RENDER_MODE_DIRECT);
    return was;
}
lv_display_t *wup_lvgl_init(void) {
    last_us = wup_us();
    lv_init();
    lv_tick_set_cb(tick);
    fb = wup_screen();
    lv_display_t *d = lv_display_create(GFX_W, GFX_H);
    (void)flush; (void)flush_direct; (void)fb;
    px_t *back = gfx_fb(), *front = back == (px_t *)WUP_FB0 ? (px_t *)WUP_FB1 : (px_t *)WUP_FB0;
    buf_a = back; buf_b = front;
    lv_display_set_flush_cb(d, flush_double);
    lv_display_set_buffers(d, back, front, GFX_W * GFX_H * sizeof(px_t), LV_DISPLAY_RENDER_MODE_DIRECT);
    disp = d;
    touch_ = lv_indev_create(); lv_indev_set_type(touch_, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(touch_, touch_read);
    lv_group_set_default(lv_group_create());
    keys_ = lv_indev_create(); lv_indev_set_type(keys_, LV_INDEV_TYPE_KEYPAD); lv_indev_set_read_cb(keys_, keys_read);
    lv_indev_set_group(keys_, lv_group_get_default());
    return d;
}
lv_indev_t *wup_lvgl_touch(void) { return touch_; }
lv_indev_t *wup_lvgl_keys(void) { return keys_; }
void wup_lvgl_run(int ms) {
    uint32_t t0 = tick();
    do { lv_timer_handler(); wup_sleep(2); } while ((int)(tick() - t0) < ms);
}
