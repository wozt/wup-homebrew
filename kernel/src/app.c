/* The Homebrew Menu and its apps, one after another (the hand-off block: libwup/include/handoff.h). The menu comes
 * from the kernel's menu slot (0x10000: header {"WMNU", stored size, crc32, inflated size, stamp}, image 0x11000);
 * images are app images (wupfmt.h), usually deflated, inflated straight into the app block (wup_ram.h) by zlib (its
 * table-driven inflate: tinf, a bit at a time, took most of a second for a 1 MB app). An app
 * that returns WUP_HANDOFF_RUN has staged the next one; after any other the menu runs again, also after the menu
 * itself ends: the way out is holding Power (the choice is at power-on). */
#include "os.h"
#include "../../libwup/include/handoff.h"
#include "../../libwup/include/wupfmt.h"

#define MENU_HDR   0x00010000u              /* the menu's slot (wup-menu/tools/mkflash.py) */
#define MENU_END   0x000b0000u
#define WMNU       0x554e4d57u
#define KERN_HDR   0x000b2000u              /* the kernel's slot (wup-menu/boot/early.c) */
#define KERN_END   0x000d2000u
#define GPST       0x54535047u
#define BUNDLE     0x002c9000u              /* a Wii U install's bundle in the bank that runs (wup-menu/boot/early.c) */
#define KB_MENU    0x00020000u

/* A Wii U install (wup-menu-installer) can only write a firmware bank: it brings the kernel and the menu as a bundle
   in it, which the early hook starts from. Here, on that first start, both go into their slots, each header last and
   checked; then the bundle's menu header is erased, and from then on the pad is as one installed over UART (Software
   Update, the early hook: the slots). Cut short, it is all done again at the next start. About ten seconds, once. */
static u8 sec[0x1000] __attribute__((aligned(4)));
static int slot_crc(u32 img, u32 size, u32 *crc) {
    u32 c = ~0u;
    for (u32 o = 0; o < size; o += sizeof sec) {
        u32 k = size - o < sizeof sec ? size - o : sizeof sec;
        if (flash_read(img + o, sec, k)) return -1;
        for (u32 i = 0; i < k; i++) { c ^= sec[i]; for (int b = 0; b < 8; b++) c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1; }
    }
    *crc = ~c; return 0;
}
static int copy_slot(u32 from, u32 to, u32 size, int blocks) {   /* header sector + size bytes of image; blocks: 64 KB erases */
    u32 n = 0x1000 + size;
    if (blocks) { for (u32 o = 0; o < n; o += 0x10000) if (flash_erase64k(to + o)) return -1; }
    else for (u32 o = 0; o < n; o += 0x1000) if (flash_erase4k(to + o)) return -1;
    for (u32 o = 0x1000; o < n; o += sizeof sec) {
        u32 k = n - o < sizeof sec ? n - o : sizeof sec;
        if (flash_read(from + o, sec, k) || flash_program(to + o, sec, k)) return -1;
    }
    return 0;
}
static void bundle_apply(void) {
    u8 sel; u32 kh[4], mh[5];
    if (flash_read(0xf000u, &sel, 1)) return;
    u32 b = (sel == 1 ? 0x00500000u : 0x00100000u) + BUNDLE;
    if (flash_read(b, kh, sizeof kh) || flash_read(b + KB_MENU, mh, sizeof mh)) return;
    if (kh[0] != GPST || kh[1] < 0x100 || kh[1] > KERN_END - KERN_HDR - 0x1000) return;
    if (mh[0] != WMNU || mh[1] < 64 || mh[1] > MENU_END - MENU_HDR - 0x1000) return;
    u32 c;
    if (slot_crc(b + 0x1000, kh[1], &c) || c != kh[2] || slot_crc(b + KB_MENU + 0x1000, mh[1], &c) || c != mh[2]) { klog("bundle: damaged\n"); return; }
    klog("bundle: a Wii U install: into the slots\n");
    if (copy_slot(b, KERN_HDR, kh[1], 0) || slot_crc(KERN_HDR + 0x1000, kh[1], &c) || c != kh[2]
        || flash_read(b, sec, 0x1000) || flash_program(KERN_HDR, sec, 0x1000)) { klog("bundle: kernel slot failed\n"); return; }
    if (copy_slot(b + KB_MENU, MENU_HDR, mh[1], 1) || slot_crc(MENU_HDR + 0x1000, mh[1], &c) || c != mh[2]
        || flash_read(b + KB_MENU, sec, 0x1000) || flash_program(MENU_HDR, sec, 0x1000)) { klog("bundle: menu slot failed\n"); return; }
    if (flash_erase4k(b + KB_MENU)) { klog("bundle: could not remove it\n"); return; }   /* (the hook no longer takes it) */
    klog("bundle: done\n");
}

typedef struct { u32 hdr, end; u32 h[5]; } menu_at;
static int menu_header(menu_at *m) {
    if (flash_read(m->hdr, m->h, sizeof m->h)) return 0;
    u32 size = m->h[1];
    if (m->h[0] != WMNU || size < 64 || size > m->end - (m->hdr + 0x1000)) return 0;
    if (m->h[4] == 0xffffffffu) m->h[4] = 0;
    return 1;
}
static int menu_load(volatile wup_handoff_t *pb, const menu_at *m) {
    u32 size = m->h[1];
    if (size > WUP_HANDOFF_STAGE_MAX || flash_read(m->hdr + 0x1000, (void *)WUP_HANDOFF_STAGE, size)) { klog("apps: flash read failed\n"); return 0; }
    if (crc32((const u8 *)WUP_HANDOFF_STAGE, size) != m->h[2]) { klog("apps: menu CRC mismatch\n"); return 0; }
    pb->stage_size = size; pb->stage_raw = m->h[3];
    return 1;
}
static int stage_menu(volatile wup_handoff_t *pb) {
    static menu_at m = { MENU_HDR, MENU_END, { 0 } };
    if (!menu_header(&m)) { klog("apps: no menu in flash\n"); return 0; }
    return menu_load(pb, &m);
}

static void sync_code(u32 a, u32 n) {           /* the written code out of the D-cache, the I-cache dropped */
    dcache_clean((void *)a, n);
    __asm__ volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
}

/* run the staged image; < 0: it is not one. An app image is one piece, linked at WUP_APP_BASE (libwup's wupbuild.py):
   its HBRW header first, then code, constants and data; its .bss and heap follow it up to WUP_APP_END */
static int run_staged(volatile wup_handoff_t *pb) {
    const u8 *img = (const u8 *)WUP_HANDOFF_STAGE;
    u32 len = pb->stage_size;
    u8 *app = (u8 *)WUP_APP_BASE;
    if (len > WUP_HANDOFF_STAGE_MAX) { klog("apps: staged image too big\n"); return -1; }
    if (pb->stage_raw) {
        unsigned n = WUP_HANDOFF_WORK_MAX;
        if (pb->stage_raw > n || inflate_raw(app, &n, img, len) || n != pb->stage_raw) { klog("apps: inflate failed\n"); return -1; }
        len = n;
    } else memmove(app, img, len);
    const hb_header *h = (const hb_header *)app;
    if (len < sizeof *h || h->magic != HB_MAGIC) { klog("apps: not an app image\n"); return -1; }
    if (h->entry < WUP_APP_BASE || h->entry >= WUP_APP_BASE + len) { klog("apps: bad app header\n"); return -1; }
    u32 entry = h->entry;
    klog("apps: running %s (%u KB)\n", (const char *)h->name, len >> 10);
    sync_code(WUP_APP_BASE, len);
    return ((int (*)(void))entry)();
}

void apps_run(void *unused) {
    (void)unused;
    volatile wup_handoff_t *pb = WUP_HANDOFF;
    memset((void *)pb, 0, sizeof *pb);
    pb->magic = WUP_HANDOFF_MAGIC; pb->version = 2;
    bundle_apply();
    for (int staged = 0;;) {
        if (!staged && !stage_menu(pb)) break;
        int menu = !staged;
        int r = run_staged(pb);
        rumble(0); mic_close(); audio_stop(); video_off();   /* (whatever the app left on) */
        if (r < 0 && menu) break;
        if (!menu) pb->runs++;
        staged = r == WUP_HANDOFF_RUN;
        if (menu && !staged) klog("apps: the menu ended: again\n");   /* (the way out is holding Power) */
    }
    klog("apps: no menu to run\n");
    for (;;) sched_sleep(1000);
}
