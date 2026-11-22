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
static int hold;                                     /* wup_lvgl_hold: no presents (the framebuffer is being written by hand) */
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
    if (hold) { lv_display_flush_ready(d); return; }   /* (fb is the stage: nothing may be written there) */
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
    if (hold) { lv_display_flush_ready(d); return; }
    gfx_present(a->x1, a->y1, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
    lv_display_flush_ready(d);
}
#define BUF_ROWS 80                                    /* a sixth of the screen (133 KB): a whole list row and more at once */
#ifdef WUP_LVGL_BUF_SPARE
static px_t buf_spare[GFX_W * BUF_ROWS] __attribute__((section(".wup_lvglbuf"), aligned(64)));   /* WUP_SPARE's free tail: no heap needed */
#endif
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
lv_display_t *wup_lvgl_init(void) {
    last_us = wup_us();
    lv_init();
    lv_tick_set_cb(tick);
    fb = wup_screen();
    lv_display_t *d = lv_display_create(GFX_W, GFX_H);
#ifdef WUP_LVGL_BUF_SPARE
    void *buf = buf_spare;
#else
    void *buf = malloc((size_t)GFX_W * BUF_ROWS * sizeof(px_t));
#endif
    if (buf) { lv_display_set_flush_cb(d, flush); lv_display_set_buffers(d, buf, 0, GFX_W * BUF_ROWS * sizeof(px_t), LV_DISPLAY_RENDER_MODE_PARTIAL); }
    else { lv_display_set_flush_cb(d, flush_direct); lv_display_set_buffers(d, fb, 0, GFX_W * GFX_H * sizeof(px_t), LV_DISPLAY_RENDER_MODE_DIRECT); }   /* (no memory: as before) */
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
int wup_lvgl_hold(int on) { int was = hold; hold = on; return was; }   /* (partial-buffer mode only: direct mode draws into fb itself) */
