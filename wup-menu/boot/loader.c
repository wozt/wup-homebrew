/* The loader, run at RAM 0x340000 ("CFW1" header). The boot hook (hook.S) loads it and calls it on every normal boot,
 * from the bundle in the running firmware bank (the Wii U install) or from flash 0x10000 (the UART install). It decides whether homebrew runs:
 *   A held at power-on                       -> the menu
 *   Autoboot set (the menu's settings)       -> the menu
 *   otherwise                                -> return at once: the stock boot
 * and then runs app images, one after another (libwup/include/handoff.h): the menu, and whatever an app stages before it
 * returns WUP_HANDOFF_RUN (the menu stages the app to start; libwup's wup_exec). After any other return the menu is
 * loaded again. The menu is a header sector {"WMNU", stored size, crc32, inflated size, stamp} and the image after it,
 * in two places: the menu slot (0x30000, image 0x31000: updates over Wi-Fi) and the bundle in the running bank
 * (bank + BUNDLE_OFF + 0x2000 / 0x3000: the Wii U install). The newer stamp runs (a missing stamp is the oldest); if
 * that one is damaged, the other. How large the menu slot is, only the tools and the filesystem's layout say (tools/mkflash.py,
 * libwup/src/fs.c): the loader takes the header's size, up to the end of our flash gap. Images are app images (libwup/include/wupfmt.h), usually deflated: inflated with tinf.
 * On any problem with the menu the loader returns, and the pad boots the stock firmware. */
#include <stddef.h>
#include "firmware.h"
#include "armcpu.h"
#include "handoff.h"
#include "wupfmt.h"
#include "tinf.h"

void *memset(void *d, int c, size_t n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, size_t n) {
    u8 *p = d; const u8 *q = s;
    if (!(((u32)p | (u32)q) & 3)) for (; n >= 4; n -= 4, p += 4, q += 4) *(u32 *)p = *(const u32 *)q;
    while (n--) *p++ = *q++;
    return d;
}

#define MENU_HDR   0x00030000u            /* the menu slot: its header sector, then the image */
#define MENU_IMG   0x00031000u
#define MENU_MAX   (0x00100000u - MENU_IMG)  /* (no further than the gap; the CRC says whether it is a menu) */
#define SELECTOR   0x0000f000u            /* the bank selector byte: 1 boots Product2 */
#define BANK_SIZE  0x00400000u
#ifndef BUNDLE_OFF
#error BUNDLE_OFF (tools/mkflash.py)
#endif
#define WMNU       0x554e4d57u            /* "WMNU" */
#define CFG0       0x000b0000u
#define CFG1       0x000b1000u
#define APP_LO     0x00350000u            /* an image's first segment */
#define APP_HI     0x00380000u

/* the menu's config record (menu/config.c): only the autoboot byte is needed here */
typedef struct { u32 magic, version, seq, len, crc; u8 c[76]; } rec_t;
static int cfg_autoboot(void) {
    static rec_t r[2];
    int best = -1;
    for (int i = 0; i < 2; i++) {
        if (flash_read(i ? CFG1 : CFG0, &r[i], sizeof r[i])) continue;
        if (r[i].magic != 0x47464357u || r[i].version != 1 || r[i].len != 76 || r[i].crc != crc32(r[i].c, 76)) continue;
        if (best < 0 || r[i].seq > r[best].seq) best = i;
    }
    return best >= 0 ? r[best].c[1] : 0;                /* config_t: light, autoboot, ... */
}

/* where a menu is: its header sector, and how far its image may reach */
typedef struct { u32 hdr, end; u32 h[5]; } menu_at;
static int menu_header(menu_at *m) {
    if (flash_read(m->hdr, m->h, sizeof m->h)) return 0;
    u32 size = m->h[1];
    if (m->h[0] != WMNU || size < 64 || size > m->end - (m->hdr + 0x1000)) return 0;
    if (m->h[4] == 0xffffffffu) m->h[4] = 0;            /* (no stamp: written before there were stamps) */
    return 1;
}
static int menu_load(volatile wup_handoff_t *pb, const menu_at *m) {
    u32 size = m->h[1];
    if (flash_read(m->hdr + 0x1000, (void *)WUP_HANDOFF_STAGE, size)) { dbg("[loader] flash read failed\n"); return 0; }
    if (crc32((const u8 *)WUP_HANDOFF_STAGE, size) != m->h[2]) { dbg("[loader] menu CRC mismatch\n"); return 0; }
    pb->stage_size = size; pb->stage_raw = m->h[3];
    pb->app_path[0] = 0;                                /* the menu is not an app: a stale app_path would send
                                                           wup__big_load down the .wup branch (netfs not up yet,
                                                           or a bigmem size that is not the menu's) -> it cannot run */
    return 1;
}
/* the menu to the stage, checked; 0 = there is none */
static int stage_menu(volatile wup_handoff_t *pb) {
    static menu_at m[2];
    static u8 sel[4];
    u32 bank = !flash_read(SELECTOR, sel, sizeof sel) && sel[0] == 1 ? 0x00500000u : 0x00100000u;
    m[0].hdr = bank + BUNDLE_OFF + 0x2000; m[0].end = bank + BANK_SIZE;
    m[1].hdr = MENU_HDR;                   m[1].end = MENU_HDR + 0x1000 + MENU_MAX;
    int ok0 = menu_header(&m[0]), ok1 = menu_header(&m[1]);
    int first = ok0 && (!ok1 || m[0].h[4] >= m[1].h[4]) ? 0 : 1;
    if (!ok0 && !ok1) { dbg("[loader] no menu in flash\n"); return 0; }
    if ((first ? ok1 : ok0) && menu_load(pb, &m[first])) return 1;
    return (first ? ok0 : ok1) && menu_load(pb, &m[!first]);
}
/* run the staged image; < 0: it is not one */
static int run_staged(volatile wup_handoff_t *pb) {
    const u8 *img = (const u8 *)WUP_HANDOFF_STAGE;
    u32 len = pb->stage_size;
    if (pb->stage_raw) {                                /* deflated: inflate it into the big memory */
        unsigned n = WUP_HANDOFF_WORK_MAX;
        if (pb->stage_raw > n || tinf_uncompress((void *)WUP_HANDOFF_WORK, &n, img, len) != TINF_OK || n != pb->stage_raw) { dbg("[loader] inflate failed\n"); return -1; }
        img = (const u8 *)WUP_HANDOFF_WORK; len = n;
    }
    const hb_header2 *h = (const hb_header2 *)img;
    u32 n1 = len, n2 = 0, entry;
    if (len < sizeof *h || h->magic != HB_MAGIC) { dbg("[loader] not an app image\n"); return -1; }
    if (h->seg2_magic == HB_SEG2_MAGIC) {
        n1 = h->size; n2 = h->seg2_size;
        if (n1 > len || n1 + n2 != len || h->seg2_addr < HB_SEG2_START || h->seg2_addr + n2 > HB_SEG2_END) { dbg("[loader] bad second segment\n"); return -1; }
    }
    entry = h->entry;
    if (n1 > APP_HI - APP_LO || entry < APP_LO || entry >= APP_LO + n1) { dbg("[loader] bad app header\n"); return -1; }
    u32 a2 = h->seg2_addr;
    dbg("[loader] running "); dbg((const char *)h->name); dbg("\n");
    memcpy((void *)APP_LO, img, n1); sync_code(APP_LO, n1);
    if (n2) { memcpy((void *)a2, img + n1, n2); sync_code(a2, n2); }
    return ((int (*)(void))entry)();
}

/* The firmware's task stack is small (inflating alone needs about 2 KB): the work runs on a stack of our own, the 3 KB
   above the hand-off block (0x34F400..0x350000). Apps switch to their own again. */
static void work(void);
__attribute__((target("arm"), noinline)) static void on_own_stack(void (*f)(void)) {
    __asm__ volatile("mov r4, sp\n\tmov sp, %1\n\tblx %0\n\tmov sp, r4"
                     :: "r"(f), "r"(0x00350000u) : "r0", "r1", "r2", "r3", "r4", "r12", "lr", "memory", "cc");
}
static void loader_main(u8 *app_id) {
    u16 b = buttons();
    int a_held = (b & BTN_A) != 0, autoboot = cfg_autoboot();
    int want = a_held || autoboot;
    if ((u32)app_id == 1) want = 1;                     /* test call from the UART monitor (the hook passes &app_id) */
    if (want) on_own_stack(work);
}
static void work(void) {
    dbg_init();                                         /* UART1 for the log (not on a stock boot: nothing is touched then) */
    dbg("[loader] homebrew boot\n");
    volatile wup_handoff_t *pb = WUP_HANDOFF;
    memset((void *)pb, 0, sizeof *pb);
    pb->magic = WUP_HANDOFF_MAGIC; pb->version = 2;
    for (int staged = 0;;) {
        if (!staged && !stage_menu(pb)) return;         /* (no menu: the stock boot) */
        int menu = !staged;
        int r = run_staged(pb);
        if (r < 0 && menu) return;
        if (!menu) pb->runs++;
        staged = r == WUP_HANDOFF_RUN;                      /* it staged the next one; else back to the menu */
        if (menu && !staged) { dbg("[loader] the menu returned: stock boot\n"); return; }
    }
}

__attribute__((section(".header"), used))
static const struct { u32 magic, entry; } header_ = { 0x31574643u /* "CFW1" */, (u32)loader_main };
