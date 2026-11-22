/* Code in WUP_BIGMEM (app.cfg: text_in_big). A large app's code goes in two segments the loader places (192 KB at
 * 0x350000, 368 KB in WUP_SPARE); what does not fit is stored in the .wup after them (wupfmt.h, big_*), raw-deflated,
 * and put at the start of WUP_BIGMEM here, before main() (the app's .bss and heap follow it). Only built into apps
 * that have such code (tools/wupbuild.py adds this file and tinf).
 *
 * The menu is not a .wup: its app_path is empty and its WUP_BIGMEM code is in its own flash slot instead, right
 * after the stored image (tools/mkflash.py writes it there: a {"WUPB", stored, raw, crc} header and the code,
 * stored == raw: uncompressed — read straight into WUP_BIGMEM). The loader runs the newer of the menu slot's and the running bank's bundle menu; the same choice is made
 * here from their WMNU headers, so a menu updated over Wi-Fi finds its own blob. */
#include <stdlib.h>
#include <string.h>
#include "wup.h"
#include "wupfmt.h"
#include "armcpu.h"
#include "fs.h"
#include "tinf.h"
#include "heatshrink_decoder.h"

extern uint8_t __big_start[], __big_end[];

#define BIGB    0x42505557u                     /* "WUPB": the menu slot's bigmem blob header */
#define WMNU    0x554e4d57u                     /* "WMNU": a menu's header sector */
#define BUNDLE_OFF 0x2c9000u                    /* tools/mkflash.py: the bundle in a firmware bank */
#define BANK1   0x00100000u
#define BANK2   0x00500000u
#define SELECTOR 0x0000f000u                     /* the bank selector byte: 1 boots Product2 */

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t align4k(uint32_t n) { return (n + 0xfff) & ~0xfffu; }

/* the menu's blob: -> its flash address, 0 = none (the headers or the blob are not there) */
static uint32_t menu_big_addr(void) {
    uint8_t sel[4], h[20];
    if (fs_flash_read(SELECTOR, sel, 4)) return 0;
    uint32_t bank = sel[0] == 1 ? BANK2 : BANK1;
    uint32_t best = 0, stamp = 0;
    for (int i = 0; i < 2; i++) {                 /* the bundle's menu, then the slot's: the newer stamp wins */
        uint32_t at = i ? 0x30000u : bank + BUNDLE_OFF + 0x2000;
        if (fs_flash_read(at, h, sizeof h) || get32(h) != WMNU) continue;
        uint32_t size = get32(h + 4);
        if (size < 64 || size > 0x000f0000u) continue;
        uint32_t st = get32(h + 16) == 0xffffffffu ? 0 : get32(h + 16);
        if (st > stamp) { stamp = st; best = at; }   /* a tie goes to the bundle, as in the loader */
    }
    if (!best) return 0;
    if (fs_flash_read(best, h, sizeof h)) return 0;
    return best + 0x1000 + align4k(get32(h + 4)); /* the blob follows the stored image, on a sector boundary */
}

static int big_from_flash(uint32_t at, uint32_t want) {
    uint8_t bh[16];
    if (!at) return -1;
    if (fs_flash_read(at, bh, sizeof bh) || get32(bh) != BIGB) { wup_log("[libwup] the menu's WUP_BIGMEM code is missing\n"); return -1; }
    uint32_t stored = get32(bh + 4), raw = get32(bh + 8), crc = get32(bh + 12);
    if (raw != want) { wup_log("[libwup] the menu's WUP_BIGMEM code is missing\n"); return -1; }

    if (stored == raw) {
        /* the old layout: uncompressed — read it straight into WUP_BIGMEM, 4 KB at a time */
        for (uint32_t off = 0; off < raw; off += 4096) {
            uint32_t k = raw - off < 4096 ? raw - off : 4096;
            if (fs_flash_read(at + 16 + off, __big_start + off, k)) { wup_log("[libwup] the menu's WUP_BIGMEM code is damaged\n"); return -3; }
        }
    } else {
        /* heatshrink (window 12, lookahead 8): streaming from flash, output straight into WUP_BIGMEM —
           a few KB of decoder state, no copy of the compressed blob needed */
        heatshrink_decoder *d = heatshrink_decoder_alloc(2048, 12, 8);
        if (!d) { wup_log("[libwup] the menu's WUP_BIGMEM code is damaged\n"); return -2; }
        static uint8_t in[512];
        uint32_t ip = 0, produced = 0;
        for (;;) {
            while (ip < stored) {
                uint32_t k = stored - ip < sizeof in ? stored - ip : sizeof in;
                if (fs_flash_read(at + 16 + ip, in, k)) { heatshrink_decoder_free(d); wup_log("[libwup] the menu's WUP_BIGMEM code is damaged\n"); return -3; }
                ip += k;
                size_t sunk = 0;
                HSD_sink_res sr = heatshrink_decoder_sink(d, in, k, &sunk);
                ip -= (uint32_t)(k - sunk);               /* what did not fit stays for the next round */
                if (sr < 0 || !sunk) break;
            }
            if (produced < want) {
                size_t on = 0;
                HSD_poll_res pr = heatshrink_decoder_poll(d, __big_start + produced, want - produced, &on);
                produced += (uint32_t)on;
                if (pr < 0) { heatshrink_decoder_free(d); wup_log("[libwup] the menu's WUP_BIGMEM code is damaged\n"); return -4; }
            }
            if (ip >= stored) {
                HSD_finish_res fr = heatshrink_decoder_finish(d);
                if (fr == HSDR_FINISH_DONE) break;
                if (fr < 0) { heatshrink_decoder_free(d); wup_log("[libwup] the menu's WUP_BIGMEM code is damaged\n"); return -4; }
            }
            if (produced >= want && ip >= stored) break;   /* (a valid stream ends with finish DONE; guard anyway) */
        }
        heatshrink_decoder_free(d);
        if (produced != want) { wup_log("[libwup] the menu's WUP_BIGMEM code is damaged\n"); return -4; }
    }

    if (crc32(__big_start, raw) != crc) { wup_log("[libwup] the menu's WUP_BIGMEM code is damaged\n"); return -4; }
    sync_code((uint32_t)__big_start, want);
    return 0;
}

int wup__big_load(void) {                          /* 0, or < 0: the app cannot run (its code is missing) */
    wup_hdr_t h; uint32_t want = (uint32_t)(__big_end - __big_start);
    if (!want) return 0;

    if (!wup_app_path()[0]) return big_from_flash(menu_big_addr(), want);   /* the menu: its blob is in the flash slot */
    if (fs_read(wup_app_path(), 0, &h, sizeof h) != (int)sizeof h || h.magic != WUP_MAGIC || h.hdr_size < WUP_HDR_V4
        || h.big_addr != (uint32_t)__big_start || h.big_raw != want) { wup_log("[libwup] this app's code for WUP_BIGMEM is missing\n"); return -1; }
    uint8_t *z = malloc(h.big_size);
    if (!z) return -2;
    int r = fs_read(wup_app_path(), h.big_off, z, h.big_size) == (int)h.big_size && crc32(z, h.big_size) == h.big_crc ? 0 : -3;
    unsigned n = want;
    if (!r && (tinf_uncompress(__big_start, &n, z, h.big_size) != TINF_OK || n != want)) r = -4;
    free(z);
    if (r) { wup_log("[libwup] this app's code for WUP_BIGMEM is damaged\n"); return r; }
    sync_code((uint32_t)__big_start, want);
    return 0;
}
