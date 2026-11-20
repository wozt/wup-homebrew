/* LVGL on the GamePad (app.cfg: lvgl = yes). The standard GUI library, with the pad's screen, touch screen and
 * buttons wired in: after wup_lvgl_init(), everything in <lvgl.h> works as in its documentation (docs.lvgl.io).
 *
 *     wup_lvgl_init();
 *     lv_obj_t *b = lv_button_create(lv_screen_active());  ...
 *     for (;;) wup_lvgl_run(5);
 *
 * Screen: 854x480, RGB565, drawn straight into the framebuffer. Touch: a pointer. Buttons: a keypad in the default
 * group (lv_group_get_default()): up/down move the focus (inside a keyboard, list of choices or text box all four
 * directions go to it), A = LV_KEY_ENTER, B = LV_KEY_ESC. The other buttons: wup_buttons().
 * Fonts: inter_16, inter_20 (default), inter_24, inter_32, with LV_SYMBOL_* icons (and inter_hud: button letters). Files: "A:/apps/x/icon.png". */
#pragma once
#include <lvgl.h>
#include "wup.h"

/* Font Awesome symbols in the fonts beside LVGL's own LV_SYMBOL_* */
#define WUP_SYMBOL_SUN       "\xEF\x86\x85"     /* f185 a sun */
#define WUP_SYMBOL_CLOCK     "\xEF\x80\x97"     /* f017 a clock */
#define WUP_SYMBOL_TERMINAL  "\xEF\x84\xA0"     /* f120 a terminal prompt */
#define WUP_SYMBOL_CHIP      "\xEF\x8B\x9B"     /* f2db a microchip */
#define WUP_SYMBOL_DRIVE     "\xEF\x82\xA0"     /* f0a0 a drive */

lv_display_t *wup_lvgl_init(void);
void wup_lvgl_run(int ms);                 /* draw and handle input for about ms milliseconds (the system runs too) */
int wup_lvgl_hold(int on);                 /* 1: stop writing the framebuffer (it is being used for something else, e.g. a staged app); 0: resume. Partial-buffer mode only */
lv_indev_t *wup_lvgl_touch(void);
lv_indev_t *wup_lvgl_keys(void);

/* An alert, where one is needed: something failed that the user must know about, or a step that destroys wants a yes.
   The screen dims a little and a card fades in over it: a title, a quieter line (or NULL), and "OK", or two choices
   (cancel != NULL). chosen() is called when the action is taken (A on it); B, or Cancel, just closes. destroys: the
   action shows red and Cancel is the first choice. Call wup_alert_step() in the loop with the buttons newly pressed:
   while it returns 1 an alert is up and has them (handle nothing else). Not for progress or states: those are screens. */
void wup_alert(const char *title, const char *message, const char *ok, const char *cancel, void (*chosen)(void), int destroys);
int wup_alert_step(unsigned pressed);
int wup_alert_shown(void);
/* A notice: good news (good = 1: a checkmark, e.g. Connected to "Home") or a small "could not" (0: a warning sign):
   a dark pill above the button hints, there for a moment and gone by itself. Nothing to press; nothing stops. */
void wup_notice(const char *text, int good);
extern void (*wup_alert_sound)(int notice);        /* called when one appears (0 an alert, 1 a notice): the caller's sound */
