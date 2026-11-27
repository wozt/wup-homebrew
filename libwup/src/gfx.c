/* gfx: RGB565 drawing for the GamePad's screen (854 x 480).
 * The kernel shows one of two screens (wup_ram.h); drawing goes to the other (FB, the back buffer). gfx_present hands
 * it to the kernel, which shows it from the next refresh on (never half drawn, never torn), and the region presented
 * is copied into the other screen, which becomes the back buffer: what an app drew stays drawn, as on a single
 * screen. LVGL apps keep the two alike through LVGL itself (src/lvgl.c, gfx__show). */
#include <string.h>
#include "gfx.h"
typedef uint32_t u32;
#define LCD(o) (*(volatile u32 *)(0xf0009400u + (o)))
#include "wup_ram.h"
#include "wupk.h"
#ifdef WUP_LARGE                                          /* a large app (app.cfg large = yes): screen 1 alone; screen 0 is
                                                             its heap (malloc_large.c). Drawn where it is shown */
static px_t *scr_back = (px_t *)WUP_FB1, *scr_front = (px_t *)WUP_FB1;
#else
static px_t *scr_back = (px_t *)WUP_FB0, *scr_front = (px_t *)WUP_FB1;
#endif
#define FB scr_back
#include "armcpu.h"
#define dclean cache_clean
#define HWCLK (*(volatile u32 *)0xf0000408u)              /* ~997 kHz */

px_t *gfx_fb(void) { return FB; }
#ifdef WUP_LARGE
static px_t *tgt = (px_t *)WUP_FB1;
static void screens_pick(void) { }
#else
static px_t *tgt = (px_t *)WUP_FB0;
static void screens_pick(void) {                          /* the back buffer is the one not on the screen now */
    px_t *shown = (px_t *)WUPK->screen_shown();
    scr_front = shown == (px_t *)WUP_FB1 ? (px_t *)WUP_FB1 : (px_t *)WUP_FB0;
    scr_back = scr_front == (px_t *)WUP_FB0 ? (px_t *)WUP_FB1 : (px_t *)WUP_FB0;
}
#endif
/* the RGB565 screen on: the layer set up for it (an app may have used the 8-bit screen before), the screen the kernel
   shows kept, drawing into the other */
static int active;
void gfx_begin(void) {
    if (active) return;
    active = 1;
    int was_ours = tgt == scr_back;
    screens_pick();
    if (was_ours) tgt = scr_back;
    WUPK->vblank_wait();
    LCD(0x70) = GFX_W; LCD(0xb0) = (LCD(0xb0) & ~3u) | 3;
    WUPK->screen_show(scr_front);
    LCD(0x80) |= 0x10;
}
void gfx_end(void) { active = 0; }                        /* (the 8-bit screen takes over: wup_screen8) */
/* brightness through the LCD's gamma curve (33 points, 0..256): what fades use; the backlight stays as set */
void gfx_brightness(int level) {
    if (level < 0) level = 0; if (level > 256) level = 256;
    for (int i = 0; i <= 32; i++) LCD(0x200 + 4 * i) = (u32)(i * 8 * level) >> 8;
}
void gfx_fade(int from, int to) {                         /* one step per refresh, GFX_FADE_MS in all */
    u32 t0 = HWCLK, dur = GFX_FADE_MS * 997u;
    for (u32 t; (t = HWCLK - t0) < dur; ) {
        WUPK->vblank_wait();
        gfx_brightness(from + (to - from) * (int)(t >> 4) / (int)(dur >> 4));
    }
    gfx_brightness(to);
}

void (*gfx_present_hook)(void);
static void rect_copy(px_t *to, const px_t *from, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) memcpy(to + (y + j) * GFX_W + x, from + (y + j) * GFX_W + x, (size_t)w * sizeof(px_t));
}
/* the back buffer shown; the region x, y, w, h (what changed since the last present) carried over into the new back
   buffer. NULL region: the whole screen */
extern int wup__video_on;                                 /* (video.c: the screens are the decoder's) */
static void flip(int x, int y, int w, int h) {
    if (wup__video_on) return;
#ifdef WUP_LARGE
    /* one screen, shown all along: nothing to switch (screen_show waits for a refresh: each frame of a game waited for
       two). Screen 1 is write-through: what was drawn is in RAM once the write buffer drains. */
    (void)w; (void)h;
    dclean(scr_back, 0);                                  /* (n = 0: the write buffer drained, alone) */
    if (WUPK->screen_shown() != scr_back) WUPK->screen_show(scr_back);
#else
    if (w > 0 && h > 0) dclean(scr_back + y * GFX_W, (u32)h * GFX_W * 2);   /* (the rows presented) */
    WUPK->screen_show(scr_back);
#endif
#ifndef WUP_LARGE                                         /* (one screen: nothing to swap or carry over) */
    px_t *t = scr_front; scr_front = scr_back; scr_back = t;
    if (tgt == scr_front) tgt = scr_back;
    if (w > 0 && h > 0) rect_copy(scr_back, scr_front, x, y, w, h);
#else
    (void)rect_copy;
#endif
    if (gfx_present_hook) gfx_present_hook();
}
void gfx_present(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; } if (x + w > GFX_W) w = GFX_W - x;
    if (y < 0) { h += y; y = 0; } if (y + h > GFX_H) h = GFX_H - y;
    flip(x, y, w, h);
}
void gfx_present_all(void) { flip(0, 0, GFX_W, GFX_H); }
/* LVGL (src/lvgl.c) keeps both screens itself: it only needs the switch */
void gfx__show(px_t *buf, int y0, int y1) {        /* (rows y0..y1 changed) */
    if (wup__video_on) return;
    if (y1 >= y0) dclean(buf + y0 * GFX_W, (u32)(y1 - y0 + 1) * GFX_W * 2);
    WUPK->screen_show(buf);
    scr_front = buf; scr_back = buf == (px_t *)WUP_FB0 ? (px_t *)WUP_FB1 : (px_t *)WUP_FB0;
    if (gfx_present_hook) gfx_present_hook();
}

/* render target: the screen, or an off-screen buffer (sprite caches, strips) */
static int tw = GFX_W, th = GFX_H;
void gfx_target(px_t *buf, int w, int h) { tgt = buf; tw = w; th = h; }
void gfx_target_screen(void) { tgt = FB; tw = GFX_W; th = GFX_H; }
/* narrow the target to rows y..y+h (drawing coordinates then start at row y); gfx_unclip undoes it */
static px_t *clip_t; static int clip_h, clip_y;
void gfx_clip_rows(int y, int h) {
    if (y < 0) { h += y; y = 0; } if (y + h > th) h = th - y; if (h < 0) h = 0;
    clip_t = tgt; clip_h = th; clip_y = y; tgt += y * tw; th = h;
}
void gfx_unclip(void) { tgt = clip_t; th = clip_h; }
int gfx_clip_y(void) { return clip_y; }
static int clip(int *x, int *y, int *w, int *h) {
    if (*x < 0) { *w += *x; *x = 0; } if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > tw) *w = tw - *x; if (*y + *h > th) *h = th - *y;
    return *w > 0 && *h > 0;
}
px_t gfx_mix(px_t a, px_t b, int al) {
    if (al <= 0) return a; if (al >= 256) return b;
    int ar = a >> 11, ag = a >> 5 & 63, ab = a & 31, br = b >> 11, bg = b >> 5 & 63, bb = b & 31;
    ar += ((br - ar) * al + 128) >> 8; ag += ((bg - ag) * al + 128) >> 8; ab += ((bb - ab) * al + 128) >> 8;
    return (px_t)(ar << 11 | ag << 5 | ab);
}
void gfx_fill(int x, int y, int w, int h, px_t c) {
    if (!clip(&x, &y, &w, &h)) return;
    u32 c2 = c | (u32)c << 16;
    for (int j = 0; j < h; j++) {
        px_t *p = tgt + (y + j) * tw + x; int n = w;
        if ((u32)p & 2) { *p++ = c; n--; }
        u32 *q = (u32 *)p;
        for (; n >= 8; n -= 8) { q[0] = c2; q[1] = c2; q[2] = c2; q[3] = c2; q += 4; }
        for (; n >= 2; n -= 2) *q++ = c2;
        if (n) *(px_t *)q = c;
    }
}
void gfx_fill_a(int x, int y, int w, int h, px_t c, int al) {
    if (al >= 256) { gfx_fill(x, y, w, h, c); return; }
    if (al <= 0 || !clip(&x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) { px_t *p = tgt + (y + j) * tw + x; for (int i = 0; i < w; i++) p[i] = gfx_mix(p[i], c, al); }
}
void gfx_vgrad(int x, int y, int w, int h, px_t top, px_t bot) {
    for (int j = 0; j < h; j++) gfx_fill(x, y + j, w, 1, gfx_mix(top, bot, h > 1 ? j * 256 / (h - 1) : 0));
}
static int isqrt(u32 v) { u32 r = 0, b = 1u << 30; while (b > v) b >>= 2; while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return (int)r; }
/* coverage (0..256) of a rounded rect's corner pixel at offset (dx, dy) beyond the corner centre */
static int corner_cov(int dx, int dy, int r) {
    int d16 = isqrt((u32)(dx * dx + dy * dy) * 256) - r * 16;   /* distance outside the edge, 1/16 px */
    return d16 <= -8 ? 256 : d16 >= 8 ? 0 : (8 - d16) * 16;
}
void gfx_rrect(int x, int y, int w, int h, int r, px_t c, int al) {
    if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2;
    gfx_fill_a(x, y + r, w, h - 2 * r, c, al);                    /* middle band */
    for (int j = 0; j < r; j++) {
        int dy = r - j;                                           /* 1..r */
        for (int side = 0; side < 2; side++) {
            int yy = side ? y + h - 1 - j : y + j;
            if (yy < 0 || yy >= th) continue;
            px_t *row = tgt + yy * tw;
            int full_from = -1;
            for (int i = 0; i < r; i++) {
                int cov = corner_cov(r - i, dy, r);
                if (cov == 256) { full_from = i; break; }
                if (!cov) continue;
                int a = cov * al >> 8, xl = x + i, xr = x + w - 1 - i;
                if (xl >= 0 && xl < tw) row[xl] = gfx_mix(row[xl], c, a);
                if (xr >= 0 && xr < tw) row[xr] = gfx_mix(row[xr], c, a);
            }
            if (full_from >= 0) gfx_fill_a(x + full_from, yy, w - 2 * full_from, 1, c, al);
        }
    }
}
void gfx_rrect_outline(int x, int y, int w, int h, int r, int t, px_t c) {
    /* ring = outer rrect minus inner: draw as many thin rrects is costly; do edge bands + AA corners */
    for (int k = 0; k < t; k++) {
        int rr = r - k < 1 ? 1 : r - k;
        gfx_fill(x + rr + k, y + k, w - 2 * (rr + k), 1, c); gfx_fill(x + rr + k, y + h - 1 - k, w - 2 * (rr + k), 1, c);
        gfx_fill(x + k, y + rr + k, 1, h - 2 * (rr + k), c); gfx_fill(x + w - 1 - k, y + rr + k, 1, h - 2 * (rr + k), c);
    }
    for (int j = -2; j < r + 2; j++) for (int i = -2; i < r + 2; i++) {
        int dx = r - i, dy = r - j;
        if (dx <= 0 || dy <= 0) continue;
        int d16 = isqrt((u32)(dx * dx + dy * dy) * 256);
        int in = d16 - (r - t) * 16, out = r * 16 - d16;          /* inside the ring if both > 0 */
        int cov = (in < 8 ? (in + 8) * 16 : 256); if (cov < 0) cov = 0;
        int cov2 = (out < 8 ? (out + 8) * 16 : 256); if (cov2 < 0) cov2 = 0;
        int a = cov * cov2 >> 8; if (a <= 0) continue;
        int pts[4][2] = { {x + i, y + j}, {x + w - 1 - i, y + j}, {x + i, y + h - 1 - j}, {x + w - 1 - i, y + h - 1 - j} };
        for (int q = 0; q < 4; q++) { int px = pts[q][0], py = pts[q][1];
            if (px >= 0 && px < tw && py >= 0 && py < th) tgt[py * tw + px] = gfx_mix(tgt[py * tw + px], c, a); }
    }
}
void gfx_shadow(int x, int y, int w, int h, int r, int blur, int strength) {
    int x0 = x - blur, y0 = y - blur, x1 = x + w + blur, y1 = y + h + blur;
    /* the falloff per distance, once (no divide instruction on the ARM926: per pixel it dominated) */
    unsigned short fall[129]; if (blur > 128) blur = 128;
    for (int d = 0; d <= blur; d++) fall[d] = (unsigned short)(strength * (blur - d) * (blur - d) / (blur * blur));
    for (int yy = y0; yy < y1; yy++) {
        if (yy < 0 || yy >= th) continue;
        int dy = yy < y + r ? y + r - yy : yy > y + h - r ? yy - (y + h - r) : 0;
        px_t *row = tgt + yy * tw;
        for (int xx = x0; xx < x1; xx++) {
            if (xx < 0 || xx >= tw) continue;
            int dx = xx < x + r ? x + r - xx : xx > x + w - r ? xx - (x + w - r) : 0;
            /* straight edges and the inside need no square root */
            int d = !(dx | dy) ? -1 : !dx ? dy - r : !dy ? dx - r : isqrt((u32)(dx * dx + dy * dy)) - r;
            if (d >= blur) continue;
            int a = d <= 0 ? strength : fall[d];
            row[xx] = gfx_mix(row[xx], 0, a);
        }
    }
}
/* a filled polygon, anti-aliased: pts are n (x, y) pairs in 1/16 pixel, relative to (x, y); each pixel
   is covered by 4x4 samples (even-odd rule). For small shapes (icons). */
void gfx_poly(int x, int y, const int *pts, int n, px_t c) {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int i = 0; i < n; i++) {
        if (pts[2 * i] < x0) x0 = pts[2 * i]; if (pts[2 * i] > x1) x1 = pts[2 * i];
        if (pts[2 * i + 1] < y0) y0 = pts[2 * i + 1]; if (pts[2 * i + 1] > y1) y1 = pts[2 * i + 1];
    }
    for (int py = y0 >> 4; py <= y1 >> 4; py++) {
        int yy = y + py; if (yy < 0 || yy >= th) continue;
        for (int px = x0 >> 4; px <= x1 >> 4; px++) {
            int xx = x + px; if (xx < 0 || xx >= tw) continue;
            int hits = 0;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                int qx = px * 16 + sx * 4 + 2, qy = py * 16 + sy * 4 + 2, in = 0;
                for (int i = 0, j = n - 1; i < n; j = i++) {
                    int ax = pts[2 * i], ay = pts[2 * i + 1], bx = pts[2 * j], by = pts[2 * j + 1];
                    if ((ay > qy) != (by > qy) && qx < ax + (bx - ax) * (qy - ay) / (by - ay)) in = !in;
                }
                hits += in;
            }
            if (hits) tgt[yy * tw + xx] = hits == 16 ? c : gfx_mix(tgt[yy * tw + xx], c, hits * 16);
        }
    }
}
void gfx_circle(int cx, int cy, int r, px_t c, int al) { gfx_rrect(cx - r, cy - r, 2 * r, 2 * r, r, c, al); }

int gfx_text(const font_t *f, int x, int base, const char *s, px_t c) {
    int pen4 = x * 4;
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
        const glyph_t *g = &f->g[ch - FONT_FIRST];
        int gx = (pen4 + 2) / 4 + g->x, gy = base + g->y, rowb = (g->w + 1) / 2;
        const unsigned char *b = f->bits + g->off;
        for (int j = 0; j < g->h; j++) {
            int yy = gy + j; if (yy < 0 || yy >= th) continue;
            px_t *row = tgt + yy * tw;
            for (int i = 0; i < g->w; i++) {
                int xx = gx + i; if (xx < 0 || xx >= tw) continue;
                int v = b[j * rowb + i / 2]; v = i & 1 ? v & 15 : v >> 4;
                if (v) row[xx] = v == 15 ? c : gfx_mix(row[xx], c, v * 17 + 1);
            }
        }
        pen4 += g->adv4;
    }
    return (pen4 + 2) / 4;
}
int gfx_text_w(const font_t *f, const char *s) {
    int w4 = 0;
    for (; *s; s++) { int ch = (unsigned char)*s; if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?'; w4 += f->g[ch - FONT_FIRST].adv4; }
    return (w4 + 2) / 4;
}
void gfx_text_c(const font_t *f, int cx, int base, const char *s, px_t c) { gfx_text(f, cx - gfx_text_w(f, s) / 2, base, s, c); }
void gfx_blit(int x, int y, int w, int h, const px_t *src) {
    for (int j = 0; j < h; j++) {
        int yy = y + j; if (yy < 0 || yy >= th) continue;
        for (int i = 0; i < w; i++) { int xx = x + i; if (xx >= 0 && xx < tw) tgt[yy * tw + xx] = src[j * w + i]; }
    }
}

/* ---- 24-bit colour gradients, dithered to RGB565 (4x4 ordered dither: RGB565 has only 32 levels per
   channel, so smooth gradients band without it) */
static const unsigned char bayer4[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
static inline px_t dith(int r, int g, int b, int x, int y) {   /* r,g,b in 8.4 fixed point */
    int t = bayer4[y & 3][x & 3];
    r = (r + (t << 3)) >> 7; g = (g + (t << 2)) >> 6; b = (b + (t << 3)) >> 7;   /* +0..7.5 / +0..3.75 */
    if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
    return (px_t)(r << 11 | g << 5 | b);
}
static void row_color(rgb24_t top, rgb24_t bot, int j, int h, int *r, int *g, int *b) {
    int f = h > 1 ? j * 4096 / (h - 1) : 0;                       /* 0..4096 */
    int r0 = top >> 16 & 255, g0 = top >> 8 & 255, b0 = top & 255, r1 = bot >> 16 & 255, g1 = bot >> 8 & 255, b1 = bot & 255;
    *r = (r0 * 16) + ((r1 - r0) * f >> 8); *g = (g0 * 16) + ((g1 - g0) * f >> 8); *b = (b0 * 16) + ((b1 - b0) * f >> 8);
}
void gfx_vgrad24(int x, int y, int w, int h, rgb24_t top, rgb24_t bot) {
    int x0 = x, y0 = y, w0 = w, h0 = h;
    if (!clip(&x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        int r, g, b; row_color(top, bot, y + j - y0, h0, &r, &g, &b);
        px_t *row = tgt + (y + j) * tw;
        for (int i = x; i < x + w; i++) row[i] = dith(r, g, b, i, y + j);
    }
    (void)x0; (void)w0;
}
void gfx_rrect_vgrad24(int x, int y, int w, int h, int r, rgb24_t top, rgb24_t bot) {
    if (r * 2 > w) r = w / 2; if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int yy = y + j; if (yy < 0 || yy >= th) continue;
        int cr, cg, cb; row_color(top, bot, j, h, &cr, &cg, &cb);
        px_t *row = tgt + yy * tw;
        int dy = j < r ? r - j : j >= h - r ? j - (h - 1 - r) : 0;
        int full_from = 0;
        if (dy) {
            full_from = r;
            for (int i = 0; i < r; i++) {
                int cov = corner_cov(r - i, dy, r);
                if (cov == 256) { full_from = i; break; }
                if (!cov) continue;
                int xl = x + i, xr = x + w - 1 - i;
                if (xl >= 0 && xl < tw) row[xl] = gfx_mix(row[xl], dith(cr, cg, cb, xl, yy), cov);
                if (xr >= 0 && xr < tw) row[xr] = gfx_mix(row[xr], dith(cr, cg, cb, xr, yy), cov);
            }
        }
        for (int i = x + full_from; i < x + w - full_from; i++) if (i >= 0 && i < tw) row[i] = dith(cr, cg, cb, i, yy);
    }
}

void gfx_bg_rowpat(px_t *row4, rgb24_t top, rgb24_t bot, int y, int h) {
    int r, g, b; row_color(top, bot, y, h, &r, &g, &b);
    for (int i = 0; i < 4; i++) row4[i] = dith(r, g, b, i, y);
}
void gfx_fill_pat(px_t *dst, int n, const px_t *p, int x0) {
    int i = 0;
    for (; n && ((uint32_t)dst & 2); n--, i++) *dst++ = p[(x0 + i) & 3];
    uint32_t w0 = p[(x0 + i) & 3] | (uint32_t)p[(x0 + i + 1) & 3] << 16, w1 = p[(x0 + i + 2) & 3] | (uint32_t)p[(x0 + i + 3) & 3] << 16;
    uint32_t *q = (uint32_t *)dst;
    for (; n >= 8; n -= 8) { q[0] = w0; q[1] = w1; q[2] = w0; q[3] = w1; q += 4; }
    for (; n >= 4; n -= 4) { q[0] = w0; q[1] = w1; q += 2; }
    px_t *d = (px_t *)q; int k = 0;
    for (; n; n--, k++) *d++ = p[(x0 + i + k) & 3];
}
void gfx_copy(px_t *dst, const px_t *src, int n) {
    if ((((uint32_t)dst ^ (uint32_t)src) & 2) == 0) {
        if ((uint32_t)dst & 2) { *dst++ = *src++; n--; }
        uint32_t *d = (uint32_t *)dst; const uint32_t *s = (const uint32_t *)src;
        for (; n >= 16; n -= 16) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d[4] = s[4]; d[5] = s[5]; d[6] = s[6]; d[7] = s[7]; d += 8; s += 8; }
        for (; n >= 2; n -= 2) *d++ = *s++;
        dst = (px_t *)d; src = (const px_t *)s;
    }
    while (n--) *dst++ = *src++;
}

/* an RGB565 icon (sw x sw) into a size x size rounded square at (x, y): area-averaged scaling,
   anti-aliased corners blended over what is already there */
void gfx_icon(int x, int y, int size, int r, const px_t *src, int sw) {
    if (sw == size) {                                     /* 1:1 (the list's 64x64 icons): no averaging */
        for (int j = 0; j < size; j++) {
            int yy = y + j; if (yy < 0 || yy >= th) continue;
            int dy = j < r ? r - j : j >= size - r ? j - (size - 1 - r) : 0;
            const px_t *s = src + j * sw; px_t *d = tgt + yy * tw;
            for (int i = 0; i < size; i++) {
                int xx = x + i; if (xx < 0 || xx >= tw) continue;
                int dx = i < r ? r - i : i >= size - r ? i - (size - 1 - r) : 0;
                if (dx && dy) { int cov = corner_cov(dx, dy, r); if (cov) d[xx] = cov == 256 ? s[i] : gfx_mix(d[xx], s[i], cov); }
                else d[xx] = s[i];
            }
        }
        return;
    }
    for (int j = 0; j < size; j++) {
        int yy = y + j; if (yy < 0 || yy >= th) continue;
        int sy0 = j * sw / size, sy1 = (j + 1) * sw / size; if (sy1 <= sy0) sy1 = sy0 + 1;
        int dy = j < r ? r - j : j >= size - r ? j - (size - 1 - r) : 0;
        for (int i = 0; i < size; i++) {
            int xx = x + i; if (xx < 0 || xx >= tw) continue;
            int dx = i < r ? r - i : i >= size - r ? i - (size - 1 - r) : 0;
            int cov = (dx && dy) ? corner_cov(dx, dy, r) : 256;
            if (!cov) continue;
            int sx0 = i * sw / size, sx1 = (i + 1) * sw / size; if (sx1 <= sx0) sx1 = sx0 + 1;
            int R = 0, G = 0, B = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) for (int sx = sx0; sx < sx1; sx++) {
                px_t p = src[sy * sw + sx]; R += p >> 11; G += p >> 5 & 63; B += p & 31; n++;
            }
            px_t c = (px_t)((R / n) << 11 | (G / n) << 5 | (B / n));
            tgt[yy * tw + xx] = cov == 256 ? c : gfx_mix(tgt[yy * tw + xx], c, cov);
        }
    }
}

/* darken the current target to 9/16 (two RGB565 pixels per word: x/2 + x/16, no carries between
   channels): the dimmed background behind popups */
void gfx_dim_target(void) {
    uint32_t *w = (uint32_t *)tgt; int n = tw * th / 2;
    for (int i = 0; i < n; i++) { uint32_t v = w[i]; w[i] = ((v >> 1) & 0x7bef7befu) + ((v >> 4) & 0x08610861u); }
}
/* halve the brightness of the whole screen (two RGB565 pixels per word: shift, then drop the bit
   that crossed into the next channel) */
void gfx_dim_half(void) {
    uint32_t *w = (uint32_t *)FB;
    for (int i = 0; i < GFX_W * GFX_H / 2; i++) w[i] = (w[i] >> 1) & 0x7bef7befu;
    gfx_present_all();
}
