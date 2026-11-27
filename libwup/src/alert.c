/* wup_alert, wup_notice: see include/wup_lvgl.h. An alert over whatever is on screen: the screen dims a little (at once: fading the whole
 * screen would draw all of it again every frame), a card fades in
 * (and rises a few pixels) with a title, a quieter line and one or two choices; it leaves the same way. The buttons
 * are read here, not by LVGL (the keypad is taken off what is behind while it shows): left/right choose, A takes the
 * choice, B cancels. What was chosen is acted on from wup_alert_step(), never inside an LVGL event.
 * A notice is good news (or a small "could not"): a dark pill above the button hints with a symbol and a few words,
 * there for a moment and gone by itself; nothing to press, nothing stops. */
#include "wup_lvgl.h"

static lv_obj_t *dim, *card, *btn[2];
static lv_group_t *nowhere, *behind;
static void (*on_ok)(void);
static int two, pick, danger, closing;
void (*wup_alert_sound)(int notice);                  /* (the menu, the kit: their sound for one appearing) */

static void set_card(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); lv_obj_set_style_translate_y(o, (255 - v) * 14 / 255, 0); }
static void anim(lv_obj_t *o, lv_anim_exec_xcb_t f, int from, int to, int ms, lv_anim_completed_cb_t done) {
    lv_anim_t a; lv_anim_init(&a);
    lv_anim_set_var(&a, o); lv_anim_set_exec_cb(&a, f); lv_anim_set_values(&a, from, to); lv_anim_set_duration(&a, (uint32_t)ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}
static void paint(void) {                              /* the choice: filled (red for a choice that destroys); the other: quiet */
    for (int i = 0; i < 1 + two; i++) {
        int on = i == pick, ok = i == two;             /* (the last one is the action; the first, of two, Cancel) */
        lv_obj_set_style_bg_color(btn[i], on ? lv_palette_main(ok && danger ? LV_PALETTE_RED : LV_PALETTE_BLUE) : lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_bg_opa(btn[i], on ? LV_OPA_COVER : LV_OPA_20, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn[i], 0), on ? lv_color_white() : lv_obj_get_style_text_color(card, 0), 0);
    }
}
static lv_obj_t *choice(lv_obj_t *row, const char *text) {
    lv_obj_t *b = lv_obj_create(row);
    lv_obj_remove_style_all(b); lv_obj_set_height(b, 52); lv_obj_set_flex_grow(b, 1); lv_obj_set_style_radius(b, 12, 0);
    lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, text); lv_obj_center(l);
    return b;
}
void wup_alert(const char *title, const char *message, const char *ok, const char *cancel, void (*chosen)(void), int destroys) {
    if (dim) return;
    on_ok = chosen; two = cancel != NULL; danger = destroys; pick = two && !destroys ? 1 : 0; closing = 0;   /* (a choice that destroys: Cancel first) */
    if (!nowhere) nowhere = lv_group_create();
    behind = lv_indev_get_group(wup_lvgl_keys()); lv_indev_set_group(wup_lvgl_keys(), nowhere);
    dim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(dim); lv_obj_set_size(dim, LV_PCT(100), LV_PCT(100)); lv_obj_set_style_bg_color(dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim, 96, 0);               /* (38 %) */
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);       /* (touches stop here) */
    card = lv_obj_create(dim);                         /* the theme's card: its colours in light and dark */
    lv_obj_set_size(card, 520, LV_SIZE_CONTENT); lv_obj_center(card); lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 20, 0); lv_obj_set_style_border_width(card, 0, 0); lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);         /* (the dimmed screen is what sets it apart) */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN); lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title); lv_obj_set_style_text_font(t, &inter_24, 0); lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP); lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    if (message && message[0]) {
        lv_obj_t *m = lv_label_create(card);
        lv_label_set_text(m, message); lv_obj_set_width(m, LV_PCT(100)); lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_style_text_opa(m, LV_OPA_60, 0);
    }
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row); lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT); lv_obj_set_style_margin_top(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_column(row, 14, 0);
    if (two) btn[0] = choice(row, cancel);
    btn[two] = choice(row, ok ? ok : "OK");
    paint();
    if (wup_alert_sound) wup_alert_sound(0);
    anim(card, set_card, 0, 255, 180, NULL);
}
static void gone(lv_anim_t *a) { (void)a; if (dim) { lv_obj_delete_async(dim); dim = card = 0; lv_indev_set_group(wup_lvgl_keys(), behind); behind = 0; } }
static void leave(void) {
    closing = 1;
    /* the keys stay on nowhere until the card is gone: a button still held as the alert closes
       must not click what is behind (its choice already ran) */
    lv_anim_delete(card, set_card);
    anim(card, set_card, lv_obj_get_style_opa(card, 0), 0, 120, gone);   /* (then the dim goes with it, at once) */
}
int wup_alert_shown(void) { return dim != 0; }
int wup_alert_step(unsigned pressed) {
    if (!dim) return 0;
    if (closing) return 1;                             /* (leaving: nothing reaches what is behind yet) */
    if (two && (pressed & (WUP_LEFT | WUP_RIGHT))) { pick = pressed & WUP_LEFT ? 0 : 1; paint(); }
    if (pressed & WUP_B) leave();
    else if (pressed & WUP_A) {
        void (*f)(void) = pick == two ? on_ok : 0;
        leave();
        if (f) f();
    }
    return 1;
}

/* ---- the notice */
static lv_obj_t *pill; static lv_timer_t *pill_timer;
static void pill_fade(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); lv_obj_set_style_translate_y(o, (255 - v) * 10 / 255, 0); }
static void pill_gone(lv_anim_t *a) { (void)a; if (pill) { lv_obj_delete_async(pill); pill = 0; } }
static void pill_out(lv_timer_t *t) { (void)t; pill_timer = 0; if (pill) anim(pill, pill_fade, 255, 0, 260, pill_gone); }
void wup_notice(const char *text, int good) {
    if (pill) { lv_anim_delete(pill, pill_fade); lv_obj_delete(pill); pill = 0; }
    if (pill_timer) { lv_timer_delete(pill_timer); pill_timer = 0; }
    int dark = lv_color_brightness(lv_obj_get_style_bg_color(lv_screen_active(), 0)) < 128;
    pill = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pill); lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT); lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(pill, lv_color_hex(dark ? 0x48484a : 0x1c1c1e), 0); lv_obj_set_style_bg_opa(pill, 240, 0);
    lv_obj_set_style_radius(pill, 26, 0); lv_obj_set_style_pad_hor(pill, 24, 0); lv_obj_set_style_pad_ver(pill, 14, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, 12, 0); lv_obj_set_style_text_color(pill, lv_color_white(), 0);
    lv_obj_t *i = lv_label_create(pill); lv_label_set_text(i, good ? LV_SYMBOL_OK : LV_SYMBOL_WARNING);
    lv_obj_t *l = lv_label_create(pill); lv_label_set_text(l, text);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -64);    /* (above the button hints) */
    if (wup_alert_sound && good) wup_alert_sound(1);
    anim(pill, pill_fade, 0, 255, 200, NULL);
    pill_timer = lv_timer_create(pill_out, 1800, NULL); lv_timer_set_repeat_count(pill_timer, 1);
}
