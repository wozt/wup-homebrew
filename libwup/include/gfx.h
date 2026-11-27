/* gfx: RGB565 drawing for the GamePad UI (854x480 UI layer in RGB565, see gfx.c). */
#pragma once
#include <stdint.h>
#include "font_aa.h"
#ifdef __cplusplus                                   /* (C++ apps: these are C) */
extern "C" {
#endif

typedef uint16_t px_t;
#define RGB(r, g, b) ((px_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | ((b) >> 3)))
#define GFX_W 854
#define GFX_H 480

void gfx_begin(void);                 /* the RGB565 screen on (wup_screen does this) */
void gfx_end(void);                   /* given up for the 8-bit screen (wup_screen8 does this) */
/* Fades through the LCD's gamma curve (33 points, 0..256): it dims whatever is on screen, without redrawing.
   256 = normal. Apps need not fade themselves: libwup fades every app in and out. */
#define GFX_FADE_MS 200
void gfx_brightness(int level);       /* 0 (black) .. 256 (normal), immediately */
void gfx_fade(int from, int to);      /* over GFX_FADE_MS, one step per frame */
extern void (*gfx_present_hook)(void); /* called after each present (libwup: fade in on the first frame) */
px_t *gfx_fb(void);
void gfx_target(px_t *buf, int w, int h);       /* draw into an off-screen buffer (w = stride) */
void gfx_target_screen(void);
void gfx_clip_rows(int y, int h);              /* draw only into rows y..y+h; coordinates from row y */
void gfx_unclip(void);
int gfx_clip_y(void);
void gfx_present(int x, int y, int w, int h);  /* flush a region of the framebuffer to the display */
void gfx_present_all(void);
void gfx_dim_half(void);                       /* whole screen at half brightness (for fades) */
void gfx_dim_target(void);                     /* the current target at 9/16 brightness */

px_t gfx_mix(px_t a, px_t b, int alpha);       /* alpha 0..256: 0 = a, 256 = b */
void gfx_fill(int x, int y, int w, int h, px_t c);
void gfx_fill_a(int x, int y, int w, int h, px_t c, int alpha);
void gfx_vgrad(int x, int y, int w, int h, px_t top, px_t bottom);
void gfx_rrect(int x, int y, int w, int h, int r, px_t c, int alpha);           /* anti-aliased */
void gfx_rrect_outline(int x, int y, int w, int h, int r, int t, px_t c);
void gfx_shadow(int x, int y, int w, int h, int r, int blur, int strength);      /* strength 0..256 */
void gfx_circle(int cx, int cy, int r, px_t c, int alpha);
void gfx_poly(int x, int y, const int *pts, int n, px_t c);    /* filled, anti-aliased; pts in 1/16 px */

int gfx_text(const font_t *f, int x, int baseline, const char *s, px_t c);      /* returns end x */
int gfx_text_w(const font_t *f, const char *s);
void gfx_text_c(const font_t *f, int cx, int baseline, const char *s, px_t c);  /* centred */
void gfx_blit(int x, int y, int w, int h, const px_t *src);
void gfx_icon(int x, int y, int size, int r, const px_t *src, int sw);   /* scaled, rounded */
/* 24-bit colour gradients, ordered-dithered to RGB565 */
typedef uint32_t rgb24_t;
#define C24(r, g, b) ((rgb24_t)((r) << 16 | (g) << 8 | (b)))
#define TO565(c) RGB((c) >> 16 & 255, (c) >> 8 & 255, (c) & 255)
void gfx_vgrad24(int x, int y, int w, int h, rgb24_t top, rgb24_t bottom);
/* fast paths for the animated band */
void gfx_bg_rowpat(px_t *row4, rgb24_t top, rgb24_t bot, int y, int h);  /* 4-pixel dither pattern of row y */
void gfx_fill_pat(px_t *dst, int n, const px_t *pat4, int x0);          /* n pixels of a 4-px pattern */
void gfx_copy(px_t *dst, const px_t *src, int n);                       /* n pixels, word-wise when aligned */
void gfx_rrect_vgrad24(int x, int y, int w, int h, int r, rgb24_t top, rgb24_t bottom);

#ifdef __cplusplus
}
#endif
