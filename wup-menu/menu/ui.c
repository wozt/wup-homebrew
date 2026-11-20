/* The Homebrew Menu's screens, with LVGL's stock widgets and theme. Every screen is a function that builds it from
 * the current state; the screens in use are a stack (B goes back). Work that takes time (scanning, joining,
 * installing) never runs inside an LVGL event: the event defers it to ui_step(), in the menu's main loop. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu.h"
#include "fs.h"
#include "wup_hw.h"
#include "wupfmt.h"
#include "ftpd.h"
#include "netshare.h"

typedef void (*screen_fn)(lv_obj_t *scr);
static void kb_screen(lv_obj_t *scr);
static lv_obj_t *kb_obj;                           /* the keyboard on screen: its key selection clicks as it moves */
static screen_fn stack[8];
static int depth;                                  /* stack[depth] is showing; 0 = home */
static void (*deferred)(void);
static int home_sel;
static lv_obj_t *status, *battery, *clk;                 /* the title bar's Wi-Fi (and charging) symbols; the battery's charge */

/* Which row has the focus is kept per screen: a screen built again (a setting changed) or returned to (B) shows
   the same row selected, not the first. */
static int focus_at[8], shown = -1;
static int focused_index(void) {
    lv_group_t *g = lv_group_get_default();
    lv_obj_t *f = lv_group_get_focused(g);
    for (uint32_t i = 0; f && i < lv_group_get_obj_count(g); i++) if (lv_group_get_obj_by_index(g, i) == f) return (int)i;
    return 0;
}
static void status_text(void);
static int quiet;                                  /* (focus set by the menu itself: no click) */
static void show(void) {                           /* build stack[depth] again and switch to it */
    quiet = 1;
    lv_group_t *g = lv_group_get_default();
    lv_obj_t *old = lv_screen_active(), *scr = lv_obj_create(NULL);
    if (shown >= 0 && shown <= depth) focus_at[shown] = focused_index();     /* (a screen left by B forgets its row) */
    lv_group_remove_all_objs(g);
    status = battery = clk = 0;
    stack[depth](scr);
    status_text();
    lv_obj_t *f = stack[depth] != kb_screen && focus_at[depth] < (int)lv_group_get_obj_count(g) ? lv_group_get_obj_by_index(g, (uint32_t)focus_at[depth]) : 0;
    if (!f && lv_group_get_obj_count(g)) f = lv_group_get_obj_by_index(g, 0);
    if (f && stack[depth] != kb_screen) { lv_group_focus_obj(f); lv_obj_add_state(f, LV_STATE_FOCUS_KEY); }   /* shown selected, as after a button press */
    shown = depth;
    quiet = 0;
    lv_screen_load(scr);
    if (old) lv_obj_delete_async(old);             /* (async: an event of one of its widgets may be running) */
}
static void push(screen_fn f) { if (depth < 7) { sound_click(); stack[++depth] = f; focus_at[depth] = 0; show(); } }
static void pop(void) { if (depth > 0) { sound_click(); depth--; show(); } }
static void on_group_focus(lv_group_t *g) {
    if (!quiet) sound_click();
    lv_obj_t *f = lv_group_get_focused(g);          /* the last row that can be selected: the list shows its end too */
    uint32_t n = lv_group_get_obj_count(g);         /* (rows below it that only show something, like Storage) */
    if (f && n && lv_group_get_obj_by_index(g, n - 1) == f) lv_obj_scroll_to_y(lv_obj_get_parent(f), LV_COORD_MAX, quiet ? LV_ANIM_OFF : LV_ANIM_ON);
}
static void defer(void (*f)(void)) { deferred = f; }

/* ---- the parts of a screen */
#define HUD 20                                     /* the size of everything in the bars: button badges, Wi-Fi, battery */
/* the hints at the bottom right: "B Back    A Select" -> for each, the button as a round badge, then what it does */
static void hint_bar(lv_obj_t *scr, const char *hints) {
    lv_color_t ink = M.cfg.light ? lv_color_hex(0x202124) : lv_color_hex(0xf2f2f2), paper = M.cfg.light ? lv_color_white() : lv_color_hex(0x15171a);
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 44); lv_obj_set_style_pad_hor(bar, 32, 0); lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (!depth) {                                   /* home: the menu's version at the left */
        lv_obj_t *v = lv_label_create(bar);
        lv_label_set_text(v, "v" MENU_VERSION); lv_obj_set_style_text_font(v, &inter_16, 0); lv_obj_set_style_text_opa(v, LV_OPA_50, 0);
        lv_obj_set_flex_grow(v, 1);
    }
    for (const char *p = hints; *p; ) {
        char key[2] = { *p, 0 }, what[32]; int n = 0;
        p++; while (*p == ' ') p++;
        while (*p && !(p[0] == ' ' && p[1] == ' ') && n < 31) what[n++] = *p++;
        what[n] = 0; while (*p == ' ') p++;
        lv_obj_t *badge = lv_obj_create(bar);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, HUD, HUD); lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(badge, ink, 0); lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_t *k = lv_label_create(badge);
        lv_label_set_text(k, key); lv_obj_set_style_text_font(k, &inter_hud, 0); lv_obj_set_style_text_color(k, paper, 0); lv_obj_align(k, LV_ALIGN_CENTER, 0, 0);
        lv_obj_t *t = lv_label_create(bar);
        lv_label_set_text(t, what); lv_obj_set_style_text_font(t, &inter_16, 0); lv_obj_set_style_text_opa(t, LV_OPA_70, 0);
        lv_obj_set_style_pad_right(t, 16, 0);
    }
}
static lv_obj_t *page(lv_obj_t *scr, const char *title, const char *hints) {
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 0, 0); lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), 64); lv_obj_set_style_pad_hor(bar, 32, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title); lv_obj_set_style_text_font(t, &inter_32, 0);
    {                                               /* Wi-Fi, charging and the battery, on every screen, at the size of the button badges */
        lv_color_t ink = M.cfg.light ? lv_color_hex(0x202124) : lv_color_hex(0xf2f2f2);
        lv_obj_t *st = lv_obj_create(bar);
        lv_obj_remove_style_all(st); lv_obj_set_size(st, LV_SIZE_CONTENT, 28); lv_obj_set_style_pad_column(st, 10, 0);
        lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(st, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        status = lv_label_create(st); lv_label_set_text(status, ""); lv_obj_set_style_text_color(status, ink, 0);
        lv_obj_t *bat = lv_obj_create(st);            /* the battery: a body with its charge inside, and the tip */
        lv_obj_remove_style_all(bat); lv_obj_set_size(bat, 36, HUD - 4);
        lv_obj_t *body = lv_obj_create(bat);
        lv_obj_remove_style_all(body); lv_obj_set_size(body, 32, HUD - 4); lv_obj_set_style_radius(body, 4, 0);
        lv_obj_set_style_border_width(body, 2, 0); lv_obj_set_style_border_color(body, ink, 0); lv_obj_set_style_pad_all(body, 2, 0);
        battery = lv_obj_create(body);
        lv_obj_remove_style_all(battery); lv_obj_set_height(battery, LV_PCT(100)); lv_obj_set_style_radius(battery, 1, 0);
        lv_obj_set_style_bg_color(battery, ink, 0); lv_obj_set_style_bg_opa(battery, LV_OPA_COVER, 0);
        lv_obj_t *tip = lv_obj_create(bat);
        lv_obj_remove_style_all(tip); lv_obj_set_size(tip, 3, 7); lv_obj_align(tip, LV_ALIGN_RIGHT_MID, 0, 0); lv_obj_set_style_radius(tip, 1, 0);
        lv_obj_set_style_bg_color(tip, ink, 0); lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
        clk = lv_label_create(st); lv_label_set_text(clk, ""); lv_obj_set_style_text_color(clk, ink, 0);   /* the time, once known */
    }
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_width(list, LV_PCT(100)); lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_radius(list, 0, 0); lv_obj_set_style_border_width(list, 0, 0); lv_obj_set_style_pad_hor(list, 24, 0);
    hint_bar(scr, hints);
    return list;
}
static lv_obj_t *row(lv_obj_t *list, const void *icon, const char *text, const char *value, lv_event_cb_t cb, void *arg) {
    lv_obj_t *b = lv_list_add_button(list, icon, text);
    lv_obj_set_style_pad_ver(b, 16, 0);
    if (icon && lv_image_src_get_type(icon) == LV_IMAGE_SRC_SYMBOL) {       /* symbols differ in width: one column, so the texts line up */
        lv_obj_t *im = lv_obj_get_child(b, 0);
        lv_obj_set_width(im, 28); lv_image_set_inner_align(im, LV_IMAGE_ALIGN_CENTER);
    }
    if (value) { lv_obj_t *v = lv_label_create(b); lv_label_set_text(v, value); lv_obj_set_style_text_opa(v, LV_OPA_60, 0); }
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg); else { lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE); lv_group_remove_obj(b); }
    return b;
}
static void sw_row_icon(lv_obj_t *list, const void *icon, const char *text, int on, lv_event_cb_t cb) {
    lv_obj_t *b = row(list, icon, text, NULL, cb, NULL), *s = lv_switch_create(b);
    lv_group_remove_obj(s); lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s, 46, 24);                     /* (no taller than the text: every row the same height) */
    lv_obj_set_style_bg_color(s, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);   /* (blue is the selected row's)*/
    if (on) lv_obj_add_state(s, LV_STATE_CHECKED);
}
static void sw_row(lv_obj_t *list, const char *text, int on, lv_event_cb_t cb) { sw_row_icon(list, NULL, text, on, cb); }
static void note(lv_obj_t *list, const char *text) {
    lv_obj_t *l = lv_label_create(list);
    lv_label_set_text(l, text); lv_obj_set_width(l, LV_PCT(100)); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, &inter_16, 0); lv_obj_set_style_text_opa(l, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(l, 16, 0); lv_obj_set_style_pad_top(l, 8, 0); lv_obj_set_style_pad_bottom(l, 20, 0);
}
static void apply_theme(void) {
    lv_display_t *d = lv_display_get_default();
    lv_display_set_theme(d, lv_theme_default_init(d, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), !M.cfg.light, &inter_20));
}

/* ---- a keyboard: a Wi-Fi password, a hidden network's name, the file sharing password */
static char kb_title[64], kb_text[64];
static const char *kb_hint = "Password"; static int kb_min = 8, kb_max = 63, kb_or_empty;
static void (*kb_done)(void);
static void kb_event(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        const char *t = lv_textarea_get_text(ta);
        if ((int)strlen(t) < kb_min && !(kb_or_empty && !t[0])) return;   /* (a WPA2 password: 8 or more) */
        strncpy(kb_text, t, sizeof kb_text - 1); kb_text[sizeof kb_text - 1] = 0;
        defer(kb_done);
    } else if (!lv_event_get_param(e)) defer(pop); /* the keyboard's own "put away" key. (B, which LVGL also reports as a
                                                      cancel, is kb_buttons': it deletes, or with nothing typed goes back) */
}
/* The GamePad's buttons type too, as on other consoles' keyboards: B deletes (with nothing typed: back), Y a space, X
   capitals, - symbols, + done, L and R move through the text. Each of those keys carries its button, small, in its
   corner (drawn as the key is: the keyboard has no place for children on its keys). */
static lv_obj_t *kb_ta;
static const char *kb_button(const char *key) {
    static const struct { const char *key, *button; } of[] = { { LV_SYMBOL_BACKSPACE, "B" }, { " ", "Y" }, { "ABC", "X" }, { "abc", "X" }, { "1#", "-" },
                                                               { LV_SYMBOL_OK, "+" }, { LV_SYMBOL_LEFT, "L" }, { LV_SYMBOL_RIGHT, "R" } };
    for (unsigned i = 0; key && i < sizeof of / sizeof of[0]; i++) if (!strcmp(key, of[i].key)) return of[i].button;
    return NULL;
}
static void kb_draw(lv_event_t *e) {
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
    if (!base || base->part != LV_PART_ITEMS || lv_draw_task_get_type(t) != LV_DRAW_TASK_TYPE_FILL) return;
    const char *b = kb_button(lv_buttonmatrix_get_button_text(lv_event_get_target(e), base->id1));
    if (!b) return;
    lv_area_t a, at; lv_draw_task_get_area(t, &a);
    at.x1 = a.x1 + 5; at.y1 = a.y1 + 5; at.x2 = at.x1 + 17; at.y2 = at.y1 + 17;
    lv_draw_rect_dsc_t r; lv_draw_rect_dsc_init(&r);
    r.bg_color = M.cfg.light ? lv_color_hex(0x202124) : lv_color_hex(0xf2f2f2); r.bg_opa = LV_OPA_70; r.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(base->layer, &r, &at);
    lv_draw_label_dsc_t l; lv_draw_label_dsc_init(&l);
    l.font = &inter_hud; l.color = M.cfg.light ? lv_color_white() : lv_color_hex(0x15171a); l.text = b; l.align = LV_TEXT_ALIGN_CENTER;
    at.y1 += (18 - lv_font_get_line_height(&inter_hud)) / 2;
    lv_draw_label(base->layer, &l, &at);
}
static void kb_buttons(unsigned pressed, unsigned *left) {   /* the buttons that type; *left: what is for the rest of the menu */
    lv_obj_t *kb = kb_obj, *ta = kb_ta;
    *left = pressed & ~(WUP_B | WUP_Y | WUP_X | WUP_MINUS | WUP_PLUS | WUP_L | WUP_R);
    if (pressed & (WUP_B | WUP_Y | WUP_X | WUP_MINUS | WUP_PLUS | WUP_L | WUP_R)) sound_click();
    if (pressed & WUP_B) { if (lv_textarea_get_text(ta)[0]) lv_textarea_delete_char(ta); else *left |= WUP_B; }   /* (nothing typed: back) */
    if (pressed & WUP_Y) lv_textarea_add_char(ta, ' ');
    if (pressed & WUP_L) lv_textarea_cursor_left(ta);
    if (pressed & WUP_R) lv_textarea_cursor_right(ta);
    if (pressed & (WUP_X | WUP_MINUS)) {
        lv_keyboard_mode_t m = lv_keyboard_get_mode(kb), to;
        if (pressed & WUP_X) to = m == LV_KEYBOARD_MODE_TEXT_LOWER ? LV_KEYBOARD_MODE_TEXT_UPPER : LV_KEYBOARD_MODE_TEXT_LOWER;
        else to = m == LV_KEYBOARD_MODE_SPECIAL ? LV_KEYBOARD_MODE_TEXT_LOWER : LV_KEYBOARD_MODE_SPECIAL;
        uint32_t sel = lv_buttonmatrix_get_selected_button(kb);
        lv_keyboard_set_mode(kb, to);
        lv_buttonmatrix_set_selected_button(kb, sel == LV_BUTTONMATRIX_BUTTON_NONE ? 0 : sel);     /* (the selection stays where it was) */
    }
    if (pressed & WUP_PLUS) lv_obj_send_event(kb, LV_EVENT_READY, NULL);
}
static void kb_screen(lv_obj_t *scr) {
    lv_obj_t *t = lv_label_create(scr);
    lv_label_set_text(t, kb_title); lv_obj_align(t, LV_ALIGN_TOP_LEFT, 32, 20);
    lv_obj_t *ta = kb_ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(ta, true); lv_textarea_set_max_length(ta, (uint32_t)kb_max); lv_textarea_set_placeholder_text(ta, kb_hint);
    lv_obj_set_width(ta, 790); lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 60);
    lv_group_remove_obj(ta);
    lv_obj_t *kb = kb_obj = lv_keyboard_create(scr);
    lv_obj_set_size(kb, LV_PCT(100), 330);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_READY, ta); lv_obj_add_event_cb(kb, kb_event, LV_EVENT_CANCEL, ta);
    lv_obj_set_style_outline_width(kb, 0, LV_STATE_FOCUS_KEY); lv_obj_set_style_border_width(kb, 0, LV_STATE_FOCUS_KEY);   /* (no focus line: it is all there is) */
    lv_obj_set_style_outline_width(kb, 0, LV_STATE_FOCUSED); lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS); lv_obj_add_event_cb(kb, kb_draw, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_group_focus_obj(kb);
    /* the text's cursor: a thin line, blinking, where the next letter goes. (The text box is not what the buttons move
       in, the keyboard is: it is given the state its cursor shows in, and the cursor its look, by hand) */
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    for (int st = 0; st < 2; st++) {
        lv_style_selector_t c = (lv_style_selector_t)LV_PART_CURSOR | (st ? LV_STATE_FOCUSED : 0);
        lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, c);
        lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, c); lv_obj_set_style_border_width(ta, 2, c);
        lv_obj_set_style_border_color(ta, lv_palette_main(LV_PALETTE_BLUE), c); lv_obj_set_style_border_opa(ta, LV_OPA_COVER, c);
        lv_obj_set_style_anim_duration(ta, 530, c);        /* (on, off: about a second) */
    }
    lv_buttonmatrix_set_selected_button(kb, 1); lv_obj_add_state(kb, LV_STATE_FOCUS_KEY);   /* a key is selected from the start (the
                                                       keyboard only does that itself when a button press brought it up) */
}
static void ask_text(const char *title, const char *hint, int min, int max, int or_empty, void (*done)(void)) {
    snprintf(kb_title, sizeof kb_title, "%s", title); kb_done = done; memset(kb_text, 0, sizeof kb_text);
    kb_hint = hint; kb_min = min; kb_max = max; kb_or_empty = or_empty;
    if (stack[depth] == kb_screen) show(); else push(kb_screen);     /* (a second question: the keyboard again, empty) */
}
static void ask_password(const char *title, void (*done)(void)) { ask_text(title, "Password", 8, 63, 0, done); }

/* the network's row: the network joined, or Not Connected */
static const char *net_label(void) {
#ifdef MENU_PREVIEW                                /* (pictures from the emulator, which has no Wi-Fi) */
    return "Home";
#endif
    return M.ip[0] ? M.cfg.ssid : "Not Connected";
}

/* ---- Wi-Fi */
#define MAXN 16
static wifi_net nets[MAXN];
static int nnets = -1, net_pick;                   /* -1: not searched yet, -2: no Wi-Fi. net_pick -1: other */
static wifi_net other;                             /* a network joined by its name: one that hides it */
static const char *fail_why;                       /* how the last try to join went wrong: said in an alert */
static void spinner(lv_obj_t *parent);
/* work in progress, the same everywhere (searching, joining, erasing): the spinner, a title ending in an ellipsis, and
   a quieter line under it. -> the title; *sub, *spin if wanted */
static lv_obj_t *busy(lv_obj_t *l, const char *title, const char *line, lv_obj_t **sub, lv_obj_t **spin) {
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(l, 10, 0);
    spinner(l);
    lv_obj_t *sp = lv_obj_get_child(l, -1); lv_obj_set_style_margin_bottom(sp, 10, 0);
    lv_obj_t *t = lv_label_create(l); lv_obj_set_style_text_font(t, &inter_24, 0); lv_label_set_text(t, title);
    lv_obj_t *s = lv_label_create(l); lv_obj_set_style_text_opa(s, LV_OPA_60, 0); lv_label_set_text(s, line);
    if (sub) *sub = s;
    if (spin) *spin = sp;
    return t;
}
static void do_scan(void) {
#ifdef MENU_PREVIEW_SEARCH                         /* (the emulator's radio answers at once: time for a picture) */
    menu_wait(600000);
#endif
#ifdef MENU_PREVIEW                                /* (sample networks: the emulator has none) */
    static const char *const sample[] = { "Home", "Home 5G Guest", "Neighbor" };
    for (nnets = 0; nnets < 3; nnets++) { memset(&nets[nnets], 0, sizeof nets[0]); strcpy(nets[nnets].ssid, sample[nnets]); nets[nnets].secured = nnets < 2; }   /* (Neighbor: open) */
    strcpy(M.cfg.ssid, "Home");
#ifdef MENU_PREVIEW_NOTICE                         /* (a picture of a notice) */
    wup_notice("Connected to \"Home\"", 1);
#endif
#ifdef MENU_PREVIEW_FAIL                           /* (a picture of a join that did not work) */
    wup_alert("Incorrect Password for \"Home 5G Guest\"", NULL, "OK", NULL, NULL, 0);
#endif
    show(); return;
#endif
    if (menu_radio()) { nnets = -2; show(); return; }
    int n = wifi_scan(nets, MAXN);
    nnets = n > 0 ? n : 0;
    show();
}
static void joining_screen(lv_obj_t *scr) {         /* while it joins: no buttons */
    char t[48]; snprintf(t, sizeof t, "Joining %s\xE2\x80\xA6", net_pick < 0 ? other.ssid : nets[net_pick].ssid);
    busy(page(scr, "Wi-Fi", ""), t, "", NULL, NULL);
}
static void wifi_screen(lv_obj_t *scr);
static void do_connect(void) {
    wifi_net n = net_pick < 0 ? other : nets[net_pick];
    fail_why = menu_connect(&n, kb_text);          /* (joined: Network shows its name; not: an alert says why) */
    memset(kb_text, 0, sizeof kb_text);
    if (fail_why) {
        static char t[64]; int pw = !strcmp(fail_why, "Incorrect Password");
        snprintf(t, sizeof t, pw ? "Incorrect Password for \"%s\"" : "Unable to Join \"%s\"", n.ssid);
        wup_alert(t, pw ? NULL : !strcmp(fail_why, "No Address") ? "The network gave the GamePad no address" : "The GamePad joins 5 GHz networks with WPA2 security", "OK", NULL, NULL, 0);
    }
    depth--;                                       /* the joining screen goes. Not joined: the list as it was, to try again */
    if (!fail_why && depth > 0 && stack[depth] == wifi_screen) depth--;   /* joined: back to Network, which names it */
    show();
    if (!fail_why) { static char t[56]; snprintf(t, sizeof t, "Connected to \"%s\"", n.ssid); wup_notice(t, 1); }
}
static void start_join(void) {
    if (stack[depth] == kb_screen) depth--;        /* (the keyboard goes) */
    stack[++depth] = joining_screen; focus_at[depth] = 0; show();
    defer(do_connect);
}
/* the network the pad is on: what it was given there, and a way off it (as a console shows its connection) */
static void forget_go(void) {
    static char t[56]; snprintf(t, sizeof t, "Forgot \"%s\"", M.cfg.ssid);
    menu_forget();
    if (depth > 0) depth--;                        /* (its page goes: the list, searched again) */
    nnets = -1; show();
    wup_notice(t, 1);
}
static void on_forget(lv_event_t *e) {
    (void)e; static char t[56]; snprintf(t, sizeof t, "Forget \"%s\"", M.cfg.ssid);
    wup_alert(t, "The GamePad leaves this network and no longer joins it by itself", "Forget", "Cancel", forget_go, 1);
}
static void on_nothing(lv_event_t *e) { (void)e; }   /* (a row that can be moved to, so the list scrolls, and does nothing) */
static void joined_screen(lv_obj_t *scr) {
    lv_obj_t *l = page(scr, M.cfg.ssid, "B Back    A Select");
    uint32_t ip = 0, mask = 0, gw = 0, dns = 0; static char a[4][20], mac[20], sig[24];
    wup_net_info(&ip, &mask, &gw, &dns);
#ifdef MENU_PREVIEW                                /* (a picture from the emulator, which has no Wi-Fi) */
    ip = 0x1704a8c0; mask = 0x00ffffff; gw = dns = 0x0104a8c0;
#endif
    const uint32_t v[4] = { ip, mask, gw, dns }; static const char *const what[4] = { "IP Address", "Subnet Mask", "Router", "DNS" };
    for (int i = 0; i < 4; i++) {
        snprintf(a[i], sizeof a[i], "%u.%u.%u.%u", (unsigned)(v[i] & 255), (unsigned)(v[i] >> 8 & 255), (unsigned)(v[i] >> 16 & 255), (unsigned)(v[i] >> 24));
        row(l, NULL, what[i], v[i] ? a[i] : "\xE2\x80\x94", on_nothing, NULL);
    }
    uint8_t m[8] __attribute__((aligned(4))) = { 0 }; int rssi = wifi_rssi();
#ifdef MENU_PREVIEW
    rssi = -52;
#endif
    if (!wifi_mac(m)) { snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); row(l, NULL, "Wi-Fi Address", mac, on_nothing, NULL); }
    if (rssi < 0) { snprintf(sig, sizeof sig, "%s (%d dBm)", rssi > -60 ? "Strong" : rssi > -72 ? "Good" : "Weak", rssi); row(l, NULL, "Signal", sig, on_nothing, NULL); }
    row(l, NULL, "Security", M.cfg.secured ? "WPA2" : "None", on_nothing, NULL);
    row(l, LV_SYMBOL_TRASH, "Forget This Network", NULL, on_forget, NULL);
}
static void on_joined(lv_event_t *e) { (void)e; push(joined_screen); }
static void on_net(lv_event_t *e) {
    net_pick = (int)(intptr_t)lv_event_get_user_data(e);
    const wifi_net *n = &nets[net_pick];
    int on_it = M.ip[0] && !strcmp(n->ssid, M.cfg.ssid);
#ifdef MENU_PREVIEW
    on_it = !strcmp(n->ssid, M.cfg.ssid);
#endif
    if (on_it) { push(joined_screen); return; }    /* the one it is on: its page */
    if (n->secured) { char t[64]; snprintf(t, sizeof t, "Enter the password for \"%s\"", n->ssid); ask_password(t, start_join); }
    else { kb_text[0] = 0; start_join(); }
}
/* Other...: a network that hides its name. Its name, then its password (none: an open network) */
static void other_pass(void) { other.secured = kb_text[0] != 0; start_join(); }
static void other_name(void) {
    memset(&other, 0, sizeof other); snprintf(other.ssid, sizeof other.ssid, "%s", kb_text); other.ssid_len = (u8)strlen(other.ssid);
    char t[64]; snprintf(t, sizeof t, "Enter the password for \"%s\"", other.ssid);
    ask_text(t, "Password", 8, 63, 1, other_pass);
}
static void on_other(lv_event_t *e) { (void)e; net_pick = -1; ask_text("Enter the network's name", "Network Name", 1, 32, 0, other_name); }
/* a plain spinner: a quarter of a ring turning at one even speed (LVGL's own also grows and shrinks its arc) */
static void spin(void *o, int32_t v) { lv_arc_set_rotation(o, v); }
static void spinner(lv_obj_t *parent) {
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, 44, 44); lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE); lv_group_remove_obj(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(a, 5, 0); lv_obj_set_style_arc_width(a, 5, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(a, 0, 360); lv_arc_set_angles(a, 0, 90);
    lv_anim_t an; lv_anim_init(&an);
    lv_anim_set_var(&an, a); lv_anim_set_exec_cb(&an, spin); lv_anim_set_values(&an, 0, 360);
    lv_anim_set_duration(&an, 900); lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE); lv_anim_set_path_cb(&an, lv_anim_path_linear);
    lv_anim_start(&an);
}
static void wifi_screen(lv_obj_t *scr) {
    lv_obj_t *l = page(scr, "Wi-Fi", nnets == -1 ? "" : nnets == -2 ? "B Back" : "B Back    A Join");   /* (none while it searches) */
    if (nnets == -2) {                                  /* the radio did not come up */
        lv_obj_set_flex_align(l, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *t = lv_label_create(l); lv_label_set_text(t, "Wi-Fi Not Available"); lv_obj_set_style_text_font(t, &inter_24, 0);
        return;
    }
    if (nnets < 0) {
        busy(l, "Searching\xE2\x80\xA6", "", NULL, NULL);
        defer(do_scan);
        return;
    }
    int listed = 0;
    for (int i = 0; i < nnets; i++) {
        int here = M.ip[0] && !strcmp(nets[i].ssid, M.cfg.ssid);
#ifdef MENU_PREVIEW
        here = !strcmp(nets[i].ssid, M.cfg.ssid);
#endif
        listed |= here;
        row(l, here ? LV_SYMBOL_OK : LV_SYMBOL_WIFI, nets[i].ssid, NULL, on_net, (void *)(intptr_t)i);   /* (joined: a checkmark) */
    }
    if (M.ip[0] && !listed) row(l, LV_SYMBOL_OK, M.cfg.ssid, NULL, on_joined, NULL);               /* (joined, and hiding its name) */
    lv_obj_t *o = row(l, LV_SYMBOL_WIFI, "Other\xE2\x80\xA6", NULL, on_other, NULL);
    lv_obj_set_style_opa(lv_obj_get_child(o, 0), LV_OPA_TRANSP, 0);                              /* (no symbol; its name in line with the others) */
    note(l, nnets ? "The GamePad joins 5 GHz networks." : "No networks found. The GamePad uses 5 GHz Wi-Fi only.");
}

/* ---- Settings */
static void file_sharing_screen(lv_obj_t *scr);
static void network_screen(lv_obj_t *scr);
static void system_screen(lv_obj_t *scr);
static void netfs_screen(lv_obj_t *scr);
static void browse_screen(lv_obj_t *scr);
static void test_screen(lv_obj_t *scr);
static void on_open(lv_event_t *e) { push((screen_fn)lv_event_get_user_data(e)); }
static void on_wifi(lv_event_t *e) { (void)e; nnets = -1; push(wifi_screen); }
static void on_theme(lv_event_t *e) { (void)e; M.cfg.light = !M.cfg.light; menu_save(); apply_theme(); show(); }
static void on_sounds(lv_event_t *e) { (void)e; M.cfg.sound_off = !M.cfg.sound_off; menu_save(); show(); }
static void on_bright(lv_event_t *e) { (void)e; M.cfg.brightness = (uint8_t)(M.cfg.brightness % 10 + 1); hw_backlight(M.cfg.brightness * 10); menu_save(); show(); }
static void on_share(lv_event_t *e) { (void)e; M.cfg.share_off = !M.cfg.share_off; menu_save(); show(); }
static void share_pw_done(void) { menu_share_password(kb_text); memset(kb_text, 0, sizeof kb_text); pop(); wup_notice("Password Set", 1); }
static void on_share_pw(lv_event_t *e) { (void)e; ask_password("Choose a password for file sharing", share_pw_done); }
static void on_login(lv_event_t *e) {
    if (!M.share_auth && !M.share_pass_set) { on_share_pw(e); return; }        /* "Password" needs one first */
    M.share_auth = !M.share_auth; menu_share_save(); show();
}
static void on_boot(lv_event_t *e) { (void)e; M.cfg.autoboot = !M.cfg.autoboot; menu_save(); show(); }
static void dev_flip(void) { M.cfg.debug_off = !M.cfg.debug_off; menu_save(); show(); }
static void on_dev(lv_event_t *e) {
    (void)e;
    if (M.cfg.debug_off) wup_alert("Turn On Developer Mode", "Tools on your network can then read and change the GamePad's memory and flash", "Turn On", "Cancel", dev_flip, 0);
    else dev_flip();
}

static void settings_screen(lv_obj_t *scr) {
    char b[8];
    lv_obj_t *l = page(scr, "Settings", "B Back    A Select");
    row(l, LV_SYMBOL_WIFI, "Network", net_label(), on_open, network_screen);
    row(l, LV_SYMBOL_EYE_OPEN, "Appearance", M.cfg.light ? "Light" : "Dark", on_theme, NULL);
    sw_row_icon(l, LV_SYMBOL_VOLUME_MAX, "Sound Effects", !M.cfg.sound_off, on_sounds);
    snprintf(b, sizeof b, "%d%%", M.cfg.brightness * 10);          /* 10% .. 100% in tens (never dark) */
    row(l, WUP_SYMBOL_SUN, "Brightness", b, on_bright, NULL);
    row(l, LV_SYMBOL_SETTINGS, "System", "", on_open, system_screen);
}
static void network_screen(lv_obj_t *scr) {
    lv_obj_t *l = page(scr, "Network", "B Back    A Select");
    row(l, LV_SYMBOL_WIFI, "Wi-Fi", net_label(), on_wifi, NULL);
    note(l, "The network the GamePad joins for file sharing, the Netloader and apps.");
    row(l, LV_SYMBOL_DIRECTORY, "File Sharing", M.cfg.share_off ? "Off" : "On", on_open, file_sharing_screen);
    note(l, "Copy apps and files between a computer and the GamePad.");
    { netfs_cfg_t *c = menu_netfs(); char v[32]; snprintf(v, sizeof v, "%s%s", c->on ? "On" : "Off", c->ip[0] ? "" : " — no server");
      row(l, LV_SYMBOL_SD_CARD, "Network Storage", v, on_open, netfs_screen); }
    note(l, "A folder on a computer, read by apps at /net.");
}
static void file_sharing_screen(lv_obj_t *scr) {
    char f[160];
    lv_obj_t *l = page(scr, "File Sharing", "B Back    A Select");
    sw_row(l, "File Sharing", !M.cfg.share_off, on_share);
    if (M.cfg.share_off) snprintf(f, sizeof f, "Turn on to copy apps and files between a computer and the GamePad over Wi-Fi.");
    else if (!M.ip[0]) snprintf(f, sizeof f, "Join a Wi-Fi network in Network > Wi-Fi to use file sharing.");
    else snprintf(f, sizeof f, "Connect from a computer using FTP at ftp://%s.", M.ip);
    note(l, f);
    row(l, NULL, "Login", M.share_auth ? "Password" : "Anyone", on_login, NULL);
    if (M.share_auth) row(l, NULL, "Password", M.share_pass_set ? "Set" : "Not Set", on_share_pw, NULL);
    note(l, M.share_auth ? "Sign in with the user name \"pad\" and this password." : "Anyone on your network can connect without a password.");
}

/* ---- Network Storage (/net): a folder on a computer, read by apps through libwup's VFS */
static int ip_ok(const char *s) {                        /* dotted quad, four 0-255 numbers */
    int parts = 0, v = -1;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') { if (v < 0) v = 0; v = v * 10 + *p - '0'; if (v > 255) return 0; }
        else if (*p == '.') { if (v < 0 || parts == 3) return 0; parts++; v = -1; }
        else if (!*p) return v >= 0 && parts == 3;
        else return 0;
    }
}
static void nfs_save_show(void) { menu_netfs_save(); show(); }
static void on_nfs_on(lv_event_t *e) { (void)e; netfs_cfg_t *c = menu_netfs(); c->on = !c->on; nfs_save_show(); }
static void nfs_ip_done(void) {
    netfs_cfg_t *c = menu_netfs();
    if (!ip_ok(kb_text)) { wup_alert("Not an IP Address", "Like 192.168.1.10", "OK", NULL, NULL, 0); return; }
    snprintf(c->ip, sizeof c->ip, "%s", kb_text); nfs_save_show();
}
static void on_nfs_ip(lv_event_t *e) { (void)e; ask_text("Server Address", "192.168.1.10", 7, 15, 0, nfs_ip_done); }
static void nfs_port_done(void) {
    netfs_cfg_t *c = menu_netfs();
    char *end = 0; long p = kb_text[0] ? strtol(kb_text, &end, 10) : 0;
    if (!end || *end || p < 1 || p > 65535) { wup_alert("Not a Port", "A number from 1 to 65535", "OK", NULL, NULL, 0); return; }
    c->port = (uint16_t)p; nfs_save_show();
}
static void on_nfs_port(lv_event_t *e) { (void)e; ask_text("Server Port", "8734", 1, 5, 0, nfs_port_done); }
static void nfs_token_done(void) {
    netfs_cfg_t *c = menu_netfs();
    int n = (int)strlen(kb_text);
    if (n > NETFS_TOKEN_LEN) n = NETFS_TOKEN_LEN;
    memcpy(c->token, kb_text, (size_t)n); c->token_len = (uint8_t)n; c->token[n] = 0; nfs_save_show();
}
static void on_nfs_token(lv_event_t *e) { (void)e; ask_text("Access Token", "Empty for none", 0, NETFS_TOKEN_LEN, 1, nfs_token_done); }
static void on_nfs_cache(lv_event_t *e) {
    (void)e; netfs_cfg_t *c = menu_netfs();
    static const uint8_t v[] = { 255, 0, 64, 16, 8 };   /* off, 32 (default), 64, 16, 8 KB */
    int i = 0; while (i < 5 && c->cache_kb != v[i]) i++;
    c->cache_kb = v[(i + 1) % 5]; nfs_save_show();
}
static const char *cache_label(uint8_t k) { return k == 255 ? "Off" : !k ? "32 KB" : k == 64 ? "64 KB" : k == 16 ? "16 KB" : "8 KB"; }
static const char *ns_err(int e) {
    switch (e) {
    case ENETUNREACH: return "No Wi-Fi connection";
    case ECONNREFUSED: return "Nothing answers on that address";
    case ETIMEDOUT: return "The server did not answer";
    case EACCES: case EPERM: return "Wrong access token";
    case ENOENT: return "Nothing at that path";
    case ENOTDIR: return "Not a folder";
    case EINVAL: return "Bad server address";
    case ECANCELED: return "Cancelled";
    default: return "The connection failed";
    }
}
static void on_nfs_test(lv_event_t *e) {
    (void)e; netfs_cfg_t *c = menu_netfs();
    if (!c->on || !c->ip[0]) { wup_alert("Network Storage Is Off", "Turn it on and set a server address first", "OK", NULL, NULL, 0); return; }
    ns_begin_test(c); push(test_screen);
}
static void browse_go(const char *path);
static void on_nfs_browse(lv_event_t *e) {
    (void)e; netfs_cfg_t *c = menu_netfs();
    if (!c->on || !c->ip[0]) { wup_alert("Network Storage Is Off", "Turn it on and set a server address first", "OK", NULL, NULL, 0); return; }
    browse_go("/"); push(browse_screen);
}
static void netfs_screen(lv_obj_t *scr) {
    char v[80];
    lv_obj_t *l = page(scr, "Network Storage", "B Back    A Select");
    netfs_cfg_t *c = menu_netfs();
    sw_row(l, "Network Storage", c->on, on_nfs_on);
    snprintf(v, sizeof v, "%s", c->ip[0] ? c->ip : "—");
    row(l, NULL, "Server", v, on_nfs_ip, NULL);
    snprintf(v, sizeof v, "%u", c->port ? c->port : NETFS_PORT_DEF);
    row(l, NULL, "Port", v, on_nfs_port, NULL);
    row(l, NULL, "Access Token", c->token_len ? "Set" : "None", on_nfs_token, NULL);
    row(l, NULL, "Read Cache", cache_label(c->cache_kb), on_nfs_cache, NULL);
    if (!M.ip[0]) note(l, "Join a Wi-Fi network in Network > Wi-Fi first.");
    else {
        row(l, LV_SYMBOL_DOWNLOAD, "Test Connection", NULL, on_nfs_test, NULL);
        row(l, LV_SYMBOL_DIRECTORY, "Browse Files", NULL, on_nfs_browse, NULL);
        note(l, "Apps see the server's folder at /net. A computer runs tools/netfs-server to share one.");
    }
}
/* the test: a busy screen while the server answers; ui_step reports the outcome */
static void test_screen(lv_obj_t *scr) { busy(page(scr, "Network Storage", "B Back"), "Testing\xE2\x80\xA6", "", NULL, NULL); }
static void test_done(void) {
    if (stack[depth] != test_screen) return;
    int r = ns_result();
    pop();
    if (!r) wup_notice("Connected", 1);
    else { static char t[128]; snprintf(t, sizeof t, "%s", ns_err(r)); wup_alert("Can't Reach the Server", t, "OK", NULL, NULL, 0); }
}
/* browsing: br_path is the server-side folder; entries come from netshare.c */
static int ends_wup(const char *n) { int l = (int)strlen(n); return l > 4 && !strcmp(n + l - 4, ".wup"); }
static char br_path[320] = "/";
static void browse_go(const char *path) {
    snprintf(br_path, sizeof br_path, "%s", path);
    ns_begin_browse(menu_netfs(), br_path);
}
static void br_open(const char *name) {           /* a directory row: go deeper */
    size_t n = strlen(br_path);
    snprintf(br_path, sizeof br_path, "%s%s%s", br_path, n > 1 ? "/" : "", name);
    ns_begin_browse(menu_netfs(), br_path);
    push(browse_screen);
}
static void br_up(void) {                          /* "..": the parent folder */
    char *s = br_path + strlen(br_path);
    while (s > br_path + 1 && *--s != '/') ;
    if (*s == '/') *s = 0; else br_path[1] = 0;
    ns_begin_browse(menu_netfs(), br_path);
}
static void br_file(const char *name, uint64_t size) {
    static char t[320];
    snprintf(t, sizeof t, "%s — %llu bytes", name, (unsigned long long)size);
    wup_alert("File", t, "OK", NULL, NULL, 0);
}
static void on_br_dir(lv_event_t *e) { int i = (int)(intptr_t)lv_event_get_user_data(e); int n; const ns_ent_t *es = ns_entries(&n); if (i < n) br_open(es[i].name); }
static void on_br_file(lv_event_t *e) { int i = (int)(intptr_t)lv_event_get_user_data(e); int n; const ns_ent_t *es = ns_entries(&n); if (i < n) br_file(es[i].name, es[i].size); }
static void on_br_up(lv_event_t *e) { (void)e; br_up(); show(); }
static void on_nfs_browse_retry(lv_event_t *e) { (void)e; browse_go(br_path); show(); }
static void browse_screen(lv_obj_t *scr) {
    lv_obj_t *l = page(scr, "Network Storage", "B Back    A Open");
    if (ns_state() == NS_WORK) { busy(l, "Reading\xE2\x80\xA6", "", NULL, NULL); return; }
    if (ns_state() == NS_FAIL) {
        lv_obj_t *t = lv_label_create(l); lv_label_set_text(t, ns_err(ns_result())); lv_obj_set_style_text_font(t, &inter_20, 0);
        row(l, LV_SYMBOL_REFRESH, "Try Again", NULL, on_nfs_browse_retry, NULL);
        return;
    }
    if (strcmp(br_path, "/")) row(l, LV_SYMBOL_LEFT, "..", NULL, on_br_up, NULL);
    int n = 0; const ns_ent_t *es = ns_entries(&n);
    if (!n) { note(l, "Empty folder"); return; }
    /* homebrew on the share (.wup) apart from the other files */
    int wups = 0;
    for (int i = 0; i < n; i++) if (!es[i].is_dir && ends_wup(es[i].name)) wups++;
    for (int pass = wups ? 0 : 1; pass < 2; pass++) {
        if (wups && (pass == 0 ? !wups : n == wups)) continue;
        if (wups) note(l, pass ? "Files and Folders" : "Apps");
        for (int i = 0; i < n; i++) {
            int wup = !es[i].is_dir && ends_wup(es[i].name);
            if ((pass == 0) != wup) continue;
            char val[24];
            if (es[i].is_dir) { val[0] = 0; row(l, LV_SYMBOL_DIRECTORY, es[i].name, NULL, on_br_dir, (void *)(uintptr_t)i); }
            else {
                if (es[i].size >= (1u << 30)) snprintf(val, sizeof val, "%.1f GB", es[i].size / 1073741824.0);
                else if (es[i].size >= (1u << 20)) snprintf(val, sizeof val, "%.1f MB", es[i].size / 1048576.0);
                else if (es[i].size >= 1024) snprintf(val, sizeof val, "%.1f KB", es[i].size / 1024.0);
                else snprintf(val, sizeof val, "%llu B", (unsigned long long)es[i].size);
                row(l, wup ? LV_SYMBOL_UPLOAD : LV_SYMBOL_FILE, es[i].name, val, on_br_file, (void *)(uintptr_t)i);
            }
        }
    }
    if (ns_truncated()) note(l, "First 64 entries shown");
    note(l, br_path);
}
static void zone_screen(lv_obj_t *scr);
static void date_time_screen(lv_obj_t *scr);
static void reset_screen(lv_obj_t *scr);
static void on_24h(lv_event_t *e) { (void)e; clock_set_24h(!clock_24h()); show(); }
static void on_zone(lv_event_t *e) { clock_set_zone((int)(intptr_t)lv_event_get_user_data(e)); pop(); }
static void date_time_screen(lv_obj_t *scr) {
    lv_obj_t *l = page(scr, "Date & Time", "B Back    A Select");
    row(l, NULL, "Time Zone", clock_zones[clock_zone()].name, on_open, zone_screen);
    sw_row(l, "24-Hour Time", clock_24h(), on_24h);
    note(l, "Set automatically over Wi-Fi.");
}
static void zone_screen(lv_obj_t *scr) {
    lv_obj_t *l = page(scr, "Time Zone", "B Back    A Select");
    for (int i = 0; i < clock_nzones; i++) row(l, i == clock_zone() ? LV_SYMBOL_OK : LV_SYMBOL_DUMMY, clock_zones[i].name, NULL, on_zone, (void *)(intptr_t)i);   /* (DUMMY: an empty column, so the names line up) */
    focus_at[depth] = clock_zone();
}
static void system_screen(lv_obj_t *scr) {
    lv_obj_t *l = page(scr, "System", "B Back    A Select");
    menu_refresh_info();
    row(l, LV_SYMBOL_POWER, "Boot", M.cfg.autoboot ? "Autoboot" : "Hold A", on_boot, NULL);
    note(l, M.cfg.autoboot ? "This menu opens every time the GamePad turns on." : "Hold A while turning the GamePad on to open this menu.");
    { char t[40]; if (!clock_text(t, sizeof t, 1)) strcpy(t, "Not Set"); row(l, WUP_SYMBOL_CLOCK, "Date & Time", t, on_open, date_time_screen); }
    sw_row_icon(l, WUP_SYMBOL_TERMINAL, "Developer Mode", !M.cfg.debug_off, on_dev);
    note(l, "Lets development tools on your network read and change the GamePad's memory and flash. Keep it off unless you need it.");
    row(l, WUP_SYMBOL_CHIP, "Firmware", M.firmware, NULL, NULL);
    row(l, WUP_SYMBOL_DRIVE, "Storage", M.storage, NULL, NULL);
    row(l, LV_SYMBOL_REFRESH, "Reset", NULL, on_open, reset_screen);
}

/* ---- Reset: erase all content and settings; then the GamePad is turned off by hand */
static lv_obj_t *erase_title, *erase_sub, *erase_spin;
static void erase_tick(void) { lv_timer_handler(); }   /* (between sectors: the spinner keeps turning) */
static void do_erase(void) {
    int r = menu_reset(erase_tick);
    lv_obj_add_flag(erase_spin, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(erase_title, r ? "Not Everything Was Erased" : "Erased");
    lv_label_set_text(erase_sub, "You can now turn off the Wii U GamePad");
}
static void erasing_screen(lv_obj_t *scr) {
    erase_title = busy(page(scr, "Reset", ""), "Erasing\xE2\x80\xA6", "Don't turn off the GamePad", &erase_sub, &erase_spin);
}
static void erase_go(void) { push(erasing_screen); defer(do_erase); }
static void on_erase(lv_event_t *e) {
    (void)e;
    wup_alert("Erase All Content and Settings", "Apps, their data, Wi-Fi and all settings are erased. This can't be undone.", "Erase", "Cancel", erase_go, 1);
}
static void reset_screen(lv_obj_t *scr) {
    lv_obj_t *l = page(scr, "Reset", "B Back    A Select");
    row(l, LV_SYMBOL_TRASH, "Erase All Content and Settings", NULL, on_erase, NULL);
    note(l, "Apps, their data, Wi-Fi and all settings are erased. The Homebrew Menu, App Store and Software Update stay.");
}

/* ---- the Netloader: an app sent from a computer (tools/wuplink.py). One thing in the middle of the screen: a
   symbol, a line saying what is happening, a smaller one below it, and the bar while an app arrives. */
static lv_obj_t *nl_icon, *nl_title, *nl_sub, *nl_bar;
static int nl_state;
static void nl_show(const char *symbol, const char *title, const char *sub, int bar) {
    lv_label_set_text(nl_icon, symbol); lv_label_set_text(nl_title, title); lv_label_set_text(nl_sub, sub);
    if (bar) lv_obj_remove_flag(nl_bar, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(nl_bar, LV_OBJ_FLAG_HIDDEN);
    if (sub[0]) lv_obj_remove_flag(nl_sub, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(nl_sub, LV_OBJ_FLAG_HIDDEN);
}
static void nl_waiting(void) {
#ifdef MENU_PREVIEW
    strcpy(M.ip, "192.168.4.104");
#endif
    nl_show(LV_SYMBOL_DOWNLOAD, "Ready to Receive", M.ip, 0);
#ifdef MENU_PREVIEW_RX                             /* (a picture of an app arriving) */
    nl_show(LV_SYMBOL_DOWNLOAD, "Hello World", "", 1); lv_bar_set_value(nl_bar, 62, LV_ANIM_OFF);
#endif
}
static void netloader_screen(lv_obj_t *scr) {
    lv_obj_t *l = page(scr, "Netloader", "B Back");
    lv_obj_set_flex_align(l, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(l, 10, 0);
    nl_icon = lv_label_create(l); lv_obj_set_style_text_font(nl_icon, &inter_32, 0); lv_obj_set_style_text_opa(nl_icon, LV_OPA_50, 0);
    lv_obj_set_style_pad_bottom(nl_icon, 14, 0);
    nl_title = lv_label_create(l); lv_obj_set_style_text_font(nl_title, &inter_24, 0);
    nl_sub = lv_label_create(l); lv_obj_set_style_text_opa(nl_sub, LV_OPA_60, 0);
    nl_bar = lv_bar_create(l); lv_obj_set_size(nl_bar, 360, 8); lv_obj_set_style_margin_top(nl_bar, 14, 0);
    nl_state = -1;
#ifndef MENU_PREVIEW
    if (!M.ip[0]) { nl_show(LV_SYMBOL_WIFI, "Wi-Fi Not Connected", "Join a network in Settings", 0); return; }
    if (wuplink_open()) { nl_show(LV_SYMBOL_WARNING, "Netloader Not Available", "", 0); return; }
#endif
    nl_waiting();
}
/* An app has arrived: it is kept in /apps and started at once, and after it the menu opens at home. Anything that
   goes wrong (no room, a damaged transfer, not an app) just closes the Netloader: the sender's side says why. */
static void nl_leave(void) { if (stack[depth] == netloader_screen) { wuplink_close(); nl_title = 0; pop(); } }
static void nl_log(const char *why) { wup_log("[menu] netloader: "); wup_log(why); wup_log("\n"); }   /* (the UART log has it) */
static void nl_failed(const char *why) { nl_log(why); nl_leave(); wup_alert("Couldn't Receive the App", why, "OK", NULL, NULL, 0); }
static void nl_finish(void) { const char *err = menu_install_received(1); if (err) nl_failed(err); }
static void netloader_step(void) {
    const wup_status_t *s = wuplink_status();
    if (!nl_title || s->state == WUP_OFF) return;
    if (s->state == WUP_RECEIVING) {
        if (nl_state != WUP_RECEIVING) nl_show(LV_SYMBOL_DOWNLOAD, s->name[0] ? s->name : "Receiving\xE2\x80\xA6", "", 1);
        lv_bar_set_value(nl_bar, s->total ? (int32_t)((uint64_t)s->done * 100 / s->total) : 0, LV_ANIM_OFF);
    } else if (s->state == WUP_READY && nl_state != WUP_READY) defer(nl_finish);
    else if (s->state == WUP_ERROR) { nl_failed(s->error ? s->error : "The transfer stopped"); return; }
    else if (s->state == WUP_WAITING && nl_state != WUP_WAITING) nl_waiting();
    nl_state = s->state;
}

/* ---- home: the installed apps */
#define ICONS 12                                   /* icons kept in memory (8 KB each); apps after that show without */
static lv_image_dsc_t icon[ICONS];
static void on_app(lv_event_t *e) {
    home_sel = (int)(intptr_t)lv_event_get_user_data(e);
    const char *err = menu_start(apps_list[home_sel].path);
    if (!err) { sound_launch(); for (int t = 0; t < 260; t += 10) wup_lvgl_run(10); }      /* (heard before the menu goes) */
    if (err) { static char t[64]; snprintf(t, sizeof t, "\"%s\" Can't Be Opened", apps_list[home_sel].name); wup_alert(t, err, "OK", NULL, NULL, 0); }
}
static void on_focus(lv_event_t *e) { home_sel = (int)(intptr_t)lv_event_get_user_data(e); }
/* - on an app: what it is and what it takes, with Delete. The system apps can't go. */
static int app_info_sel;
static void size_str(char *out, int n, uint64_t b) {
    if (b >= (1ull << 30)) snprintf(out, (size_t)n, "%.1f GB", (double)b / 1073741824.0);
    else if (b >= (1ull << 20)) snprintf(out, (size_t)n, "%.1f MB", (double)b / 1048576.0);
    else if (b >= 1024) snprintf(out, (size_t)n, "%.1f KB", (double)b / 1024.0);
    else snprintf(out, (size_t)n, "%llu B", (unsigned long long)b);
}
static void app_info_done(void) {
    int i = app_info_sel;
    if (apps_remove_all(i)) { wup_alert("Couldn't Delete the App", NULL, "OK", NULL, NULL, 0); return; }
    apps_scan();
    if (home_sel >= apps_count) home_sel = apps_count - 1;
    if (home_sel < 0) home_sel = 0;
    focus_at[0] = home_sel;
    show();
    wup_notice("App Deleted", 1);
}
static void app_info(void) {
    if (!apps_count) return;
    const app_entry_t *a = &apps_list[home_sel];
    char sz[24], pc[40] = "", t[256];
    uint64_t bytes = apps_footprint(home_sel), total = (uint64_t)fs_size_kb() * 1024;
    size_str(sz, sizeof sz, bytes);
    if (total) snprintf(pc, sizeof pc, " — %.1f%% of the GamePad's storage", (double)bytes * 100.0 / (double)total);
    snprintf(t, sizeof t, "%s\n%s%s", a->version[0] ? a->version : " ", sz, pc);
    app_info_sel = home_sel;
    wup_alert(a->name, t, "Delete", "Cancel", app_info_done, 1);
}
static void home_screen(lv_obj_t *scr) {
    lv_obj_t *l = page(scr, "Homebrew Menu", apps_count ? "Y Netloader    X Settings    A Open    - Info" : "Y Netloader    X Settings    - Info");
    for (int i = 0; i < ICONS; i++) { free((void *)icon[i].data); icon[i].data = 0; }
    for (int i = 0; i < apps_count; i++) {
        char t[140];
        const app_entry_t *a = &apps_list[i];
        const void *img = NULL;
        if (i < ICONS && a->icon_off) {
            uint8_t *px = malloc(WUP_ICON * WUP_ICON * 2);
            if (px && apps_load_icon(i, px)) {
                icon[i].header.magic = LV_IMAGE_HEADER_MAGIC; icon[i].header.cf = LV_COLOR_FORMAT_RGB565;
                icon[i].header.w = icon[i].header.h = WUP_ICON; icon[i].header.stride = WUP_ICON * 2;
                icon[i].data = px; icon[i].data_size = WUP_ICON * WUP_ICON * 2;
                img = &icon[i];
            } else free(px);
        }
        const char *sub = a->description;
        if (sub[0]) snprintf(t, sizeof t, "%s\n%s", a->name, sub); else snprintf(t, sizeof t, "%s", a->name);
        lv_obj_t *b = row(l, img ? img : LV_SYMBOL_FILE, t, a->version, on_app, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, on_focus, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
    }
    if (!apps_count) {                              /* an empty place for an app, and three words */
        lv_obj_set_flex_align(l, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(l, 24, 0);
        lv_obj_t *slot = lv_obj_create(l);
        lv_obj_remove_style_all(slot); lv_obj_set_size(slot, 96, 96); lv_obj_set_style_radius(slot, 22, 0);
        lv_obj_set_style_border_width(slot, 3, 0); lv_obj_set_style_border_color(slot, lv_palette_main(LV_PALETTE_GREY), 0); lv_obj_set_style_border_opa(slot, LV_OPA_50, 0);
        lv_obj_t *plus = lv_label_create(slot);
        lv_label_set_text(plus, LV_SYMBOL_PLUS); lv_obj_set_style_text_font(plus, &inter_32, 0); lv_obj_set_style_text_opa(plus, LV_OPA_50, 0); lv_obj_center(plus);
        lv_obj_t *t = lv_label_create(l);
        lv_label_set_text(t, M.fs_ok ? "No Apps Installed" : "Storage Not Available"); lv_obj_set_style_text_font(t, &inter_24, 0);
    }
}

/* ---- the menu's loop */
static void status_text(void) {
    char t[24];
    if (!status) return;
    int p = M.power.percent, on = M.ip[0] != 0, charging = M.power.charging;
#ifdef MENU_PREVIEW                                /* (pictures from the emulator, which has no Wi-Fi) */
    on = 1; p = 66; charging = 0;
#ifdef MENU_PREVIEW_CHARGING
    charging = 1;
#endif
#endif
    snprintf(t, sizeof t, "%s%s%s", on ? LV_SYMBOL_WIFI : "", on && charging ? "  " : "", charging ? LV_SYMBOL_CHARGE : "");
    if (strcmp(lv_label_get_text(status), t)) lv_label_set_text(status, t);
    lv_obj_set_width(battery, p < 0 ? 0 : 2 + p * 22 / 100);      /* (inside the body: 24 pixels at most) */
    {                                              /* charging: the bolt alone; not: the battery alone */
        lv_obj_t *whole = lv_obj_get_parent(lv_obj_get_parent(battery));
        if (charging) lv_obj_add_flag(whole, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(whole, LV_OBJ_FLAG_HIDDEN);
    }
    char c[16]; clock_text(c, sizeof c, 0);
    if (clk && strcmp(lv_label_get_text(clk), c)) { lv_label_set_text(clk, c); if (c[0]) lv_obj_remove_flag(clk, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(clk, LV_OBJ_FLAG_HIDDEN); }
}
/* ---- the first start after a Wii U install: its apps go onto the pad (bundle.c), which takes flash its time. The
   menu fades in as always, with this in place of the list; then the list. Nothing to press meanwhile. */
static void setup_tick(void) { lv_timer_handler(); }   /* (between pieces: the spinner keeps turning) */
static void setup_screen(lv_obj_t *scr) { busy(page(scr, "Homebrew Menu", ""), "Setting Up\xE2\x80\xA6", "Don't turn off the GamePad", NULL, NULL); }
static void do_setup(void) {
    bundle_apps(setup_tick);
    M.setup = 0;
    apps_scan();
    home_sel = 0; focus_at[0] = 0; stack[0] = home_screen; depth = 0;
    show();
}
void ui_start(int selected) {
    home_sel = selected < apps_count ? selected : 0;
    focus_at[0] = home_sel;
    apply_theme();
    stack[0] = M.setup ? setup_screen : home_screen; depth = 0;
    sound_init(); lv_group_set_focus_cb(lv_group_get_default(), on_group_focus);
    wup_alert_sound = sound_note;
    show();
    if (M.setup) defer(do_setup);
    sys_power(&M.power); status_text();
}
int ui_selected(void) { return home_sel; }
void ui_step(void) {
    static unsigned held = ~0u; static int ticks; static char had_ip;
    unsigned b = wup_buttons(), pressed = b & ~held; held = b;
    if (deferred) { void (*f)(void) = deferred; deferred = 0; f(); held = wup_buttons(); return; }
    if (wup_alert_shown()) {                       /* an alert: the buttons are its own */
        if (pressed & (WUP_A | WUP_B | WUP_LEFT | WUP_RIGHT)) sound_click();
        wup_alert_step(pressed);
        return;
    }
    if (pressed & WUP_A) sound_click();
    {                                              /* the keyboard: a click for each key the selection moves to (held: each repeat) */
        static uint32_t kb_sel; static int on_kb;
        int here = stack[depth] == kb_screen && kb_obj;
        uint32_t sel = here ? lv_buttonmatrix_get_selected_button(kb_obj) : LV_BUTTONMATRIX_BUTTON_NONE;
        if (here && on_kb && sel != kb_sel && sel != LV_BUTTONMATRIX_BUTTON_NONE) sound_click();   /* (not on arriving) */
        kb_sel = sel; on_kb = here;
    }
    if (stack[depth] == erasing_screen) pressed &= ~WUP_B;   /* (no way back while it erases) */
    if (stack[0] == setup_screen) pressed = 0;                /* (setting up: nothing to press) */
    if (stack[depth] == kb_screen && kb_obj) kb_buttons(pressed, &pressed);   /* the keyboard: B, Y, X, -, +, L and R type */
    if (!depth && stack[depth] == home_screen && apps_count && (pressed & WUP_MINUS)) { sound_click(); app_info(); }
    if (pressed & WUP_B) {
        if (stack[depth] == netloader_screen) { wuplink_close(); nl_title = 0; }
        if (stack[depth] == test_screen || stack[depth] == browse_screen) ns_abort();
        pop();
    } else if (!depth && (pressed & WUP_X)) push(settings_screen);
    else if (!depth && (pressed & WUP_Y)) push(netloader_screen);
    if (stack[depth] == netloader_screen) netloader_step();
    if (stack[depth] == test_screen && ns_state() != NS_WORK) { defer(test_done); return; }
    if (stack[depth] == browse_screen && ns_state() != NS_WORK && ns_state() != NS_IDLE) show();
    if (++ticks % 100 == 0) {
        sys_power(&M.power); status_text();
        static int warned;
        if (!warned && !M.power.charging && M.power.percent >= 0 && M.power.percent <= 10) { warned = 1; wup_alert("Low Battery", "Connect the GamePad to its AC adapter", "OK", NULL, NULL, 0); }
    }
    /* apps copied or removed over file sharing appear by themselves: once the transfers are done and a second has
       passed (a program often sends several files) */
    static int changed;
    if (ftpd_apps_changed()) changed = ticks;
    if (changed && !ftpd_busy() && ticks - changed > 100) {
        changed = 0;
        int had = apps_count;
        apps_scan();
        if (!depth) { if (home_sel >= apps_count) home_sel = 0; show(); }
        if (apps_count != had) wup_notice(apps_count > had ? "App Added" : "App Removed", 1);
    }
    if (!!M.ip[0] != had_ip) {                      /* connected or lost: what shows the state is built again */
        had_ip = !!M.ip[0];
        status_text();
        if (stack[depth] == settings_screen || stack[depth] == network_screen || stack[depth] == file_sharing_screen) show();
    }
}
