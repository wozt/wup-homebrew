/* fs: littlefs in the GamePad's spare copy of its language data (8 MB in one piece).
 * The pad keeps its message screens (pairing, errors, low battery...: pictures, in every language) twice, like its
 * firmware: Lang1 (0x900000-0x10FFFFF) and Lang2 (0x1100000-0x18FFFFF). A selector in the power chip's EEPROM (0x1256:
 * 1 = Lang2, anything else = Lang1) says which one Nintendo's firmware uses; a language update would be written to the
 * other copy and the selector flipped, so which copy is in use differs from pad to pad (checked in wup-emu with every
 * flash read logged: the copy not selected is never read).
 * So the filesystem lives in the copy the pad does NOT use: the one that already holds it, or, on a pad without one,
 * the copy the selector does not name, and only if the one it names really holds Nintendo's screens. In any doubt
 * there is no filesystem, never a guess. (No language update comes to these pads: firmware 7677 is Nintendo's last.)
 * The flash is the kernel's (wupk.h). */
#include <stdint.h>
#include <string.h>
#include "littlefs/lfs.h"
#include "fs.h"
typedef uint32_t u32; typedef uint8_t u8;
#include "wupk.h"                                  /* (the flash: the kernel's) */

static struct { u32 lo, hi; } ext[1];              /* where the filesystem is: place() (tools: wup-menu/tools/fsimage.c) */
#define NEXT (int)(sizeof ext / sizeof ext[0])
static u32 block_addr(lfs_block_t b) {
    for (int i = 0; i < NEXT; i++) { u32 n = (ext[i].hi - ext[i].lo) >> 12; if (b < n) return ext[i].lo + (b << 12); b -= n; }
    return 0xffffffffu;
}
static u32 total_blocks(void) { u32 n = 0; for (int i = 0; i < NEXT; i++) n += (ext[i].hi - ext[i].lo) >> 12; return n; }

static int bd_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t size) {
    (void)c; u32 a = block_addr(b); if (a == 0xffffffffu) return LFS_ERR_IO;
    return WUPK->flash_read(a + off, buf, size) ? LFS_ERR_IO : 0;
}
/* Erasing is what writing costs on this flash: 0.24 s for each 4 KB block (the pad's Micron), so a 4 MB file took
   minutes. So: a block that is blank already is not erased again (checked: ~1 ms); and where a block's whole 64 KB
   holds nothing (used: every block the filesystem uses, from its own list, refreshed before each write and marked as
   blocks are written), those 64 KB are erased at once (~0.7 s, a 16th of the time a block): the blocks after it are
   then blank when their turn comes. A block that holds data is never erased that way. */
#define MAX_BLOCKS 2048
static u8 used[MAX_BLOCKS / 8];
static int used_ok;
static void mark(lfs_block_t b) { if (b < MAX_BLOCKS) used[b >> 3] |= (u8)(1u << (b & 7)); }
static int is_used(lfs_block_t b) { return b >= MAX_BLOCKS || (used[b >> 3] >> (b & 7) & 1); }
static int blank(u32 a) {
    static u32 w[64];
    for (u32 o = 0; o < 4096; o += sizeof w) {
        if (WUPK->flash_read(a + o, w, sizeof w)) return 0;
        for (unsigned k = 0; k < sizeof w / 4; k++) if (w[k] != 0xffffffffu) return 0;
    }
    return 1;
}
static int bd_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t size) {
    (void)c; u32 a = block_addr(b); if (a == 0xffffffffu) return LFS_ERR_IO;
    mark(b);
    return WUPK->flash_write(a + off, buf, size) ? LFS_ERR_IO : 0;
}
static int bd_erase(const struct lfs_config *c, lfs_block_t b) {
    (void)c; u32 a = block_addr(b); if (a == 0xffffffffu) return LFS_ERR_IO;
    if (blank(a)) return 0;
    lfs_block_t g = b & ~15u;                          /* (its 64 KB: the filesystem's places are 64 KB aligned) */
    if (used_ok && WUPK->version >= 5 && (block_addr(g) & 0xffffu) == 0 && block_addr(g + 15) == block_addr(g) + 15 * 4096) {
        int all_free = 1;
        for (lfs_block_t k = g; k < g + 16 && all_free; k++) all_free = !is_used(k);
        if (all_free) return WUPK->flash_erase64k(block_addr(g)) ? LFS_ERR_IO : 0;
    }
    return WUPK->flash_erase4k(a) ? LFS_ERR_IO : 0;
}
static int bd_sync(const struct lfs_config *c) { (void)c; return 0; }

/* ---- which copy: the firmware's selector, and what the two copies hold */
#define LANG1 0x900000u
#define LANG2 0x1100000u
#define LANG_SIZE 0x800000u
static int has_fs(u32 base) {                         /* littlefs's superblock: "littlefs" at +8 of block 0 or 1 */
    u8 h[16];
    for (int b = 0; b < 2; b++) {
        if (WUPK->flash_read(base + (u32)b * 4096, h, 16)) return -1;
        if (!memcmp(h + 8, "littlefs", 8)) return 1;
    }
    return 0;
}
static int has_screens(u32 base) {                    /* language data: how many pictures (the boot reader's first word), then their table */
    u32 n;
    if (WUPK->flash_read(base, &n, 4)) return 0;
    return n >= 1 && n <= 1024;
}
/* -> ext[]. The copy that holds our filesystem; on a pad that has none yet, the one the
   selector does not use (the UIC's EEPROM 0x1256: 1 = Nintendo uses Lang2), and only if the one in use really holds
   Nintendo's screens: in any doubt no filesystem, rather than a wrong copy formatted */
static int place(int repair) {
    (void)repair;
    int f1 = has_fs(LANG1), f2 = has_fs(LANG2);
    if (f1 < 0 || f2 < 0) return -1;
    u32 at;
    if (f1 == 1 && f2 != 1) at = LANG1;
    else if (f2 == 1 && f1 != 1) at = LANG2;
    else if (f1 == 1 && f2 == 1) return -1;           /* (both: which is ours is not known) */
    else {
        u8 q[4] = { 0x03, 0x12, 0x56, 1 }, sel = 0xff;
        if (WUPK->uic(q, 4, &sel, 1) || sel > 1) return -1;
        u32 used = sel == 1 ? LANG2 : LANG1;
        if (!has_screens(used)) return -1;
        at = used == LANG2 ? LANG1 : LANG2;
    }
    ext[0].lo = at; ext[0].hi = at + LANG_SIZE;
    return 0;
}

static u8 rbuf[256], pbuf[256], lbuf[64] __attribute__((aligned(4)));
static struct lfs_config cfg = {
    .read = bd_read, .prog = bd_prog, .erase = bd_erase, .sync = bd_sync,
    .read_size = 16, .prog_size = 16, .block_size = 4096, .block_count = 0,
    .cache_size = 256, .lookahead_size = 64, .block_cycles = 500,
    .read_buffer = rbuf, .prog_buffer = pbuf, .lookahead_buffer = lbuf,
};
lfs_t fs_lfs;
static int mounted;

int fs_mount(int *formatted) {
    *formatted = 0;
    if (place(1)) return LFS_ERR_IO;                  /* (which copy is the spare is not known: nothing is touched) */
    cfg.block_count = total_blocks();
    int r = lfs_mount(&fs_lfs, &cfg);
#ifdef FS_NO_FORMAT
    if (r) return r;                                  /* apps never format: only the menu may */
#endif
    if (r) {                                          /* first use: an empty filesystem */
        r = lfs_format(&fs_lfs, &cfg);
        if (r) return r;
        r = lfs_mount(&fs_lfs, &cfg);
        if (r) return r;
        *formatted = 1;
    }
    lfs_mkdir(&fs_lfs, "/apps");
    lfs_mkdir(&fs_lfs, "/save");
    mounted = 1;
    used_ok = 0;                                      /* (what the filesystem uses: found before a write, below: finding
                                                         it reads a little of every block of every file, a fifth of a
                                                         second at every app's start for a map only writes use) */
    return 0;
}
static int used_cb(void *d, lfs_block_t b) { (void)d; mark(b); return 0; }
void fs__used_refresh(void) {                         /* (before writing: what the filesystem uses now, bd_erase) */
    if (!mounted || cfg.block_count > MAX_BLOCKS) { used_ok = 0; return; }
    memset(used, 0, sizeof used);
    used_ok = lfs_fs_traverse(&fs_lfs, used_cb, NULL) == 0;
}
#ifndef FS_NO_FORMAT
int fs_erase(void (*tick)(void)) {                    /* an empty filesystem (the menu, when there is none to walk); mounted again
                                                         at the next start. The old blocks are not erased one by one (minutes):
                                                         they are free, and erased as they are used again */
    (void)tick;
    if (!ext[0].lo && place(0)) return LFS_ERR_IO;    /* (never mounted: where it would be; not known: nothing touched) */
    if (mounted) { lfs_unmount(&fs_lfs); mounted = 0; }
    cfg.block_count = total_blocks();
    return lfs_format(&fs_lfs, &cfg);
}
#endif
int fs_ok(void) { return mounted; }
int fs_flash_read(u32 addr, void *dst, u32 len) { return WUPK->flash_read(addr, dst, len) ? -1 : 0; }   /* raw flash */
u32 fs_size_kb(void) { return cfg.block_count * 4; }
u32 fs_used_kb(void) { lfs_ssize_t n = lfs_fs_size(&fs_lfs); return n < 0 ? 0 : (u32)n * 4; }

static u8 fbuf[256] __attribute__((aligned(4)));
static struct lfs_file_config fcfg = { .buffer = fbuf };
int fs_read(const char *path, u32 off, void *dst, u32 len) {
    lfs_file_t f; int r = lfs_file_opencfg(&fs_lfs, &f, path, LFS_O_RDONLY, &fcfg);
    if (r) return r;
    if (off) lfs_file_seek(&fs_lfs, &f, off, LFS_SEEK_SET);
    lfs_ssize_t n = lfs_file_read(&fs_lfs, &f, dst, len);
    lfs_file_close(&fs_lfs, &f);
    return (int)n;
}
int fs_write(const char *path, const void *src, u32 len) {
    fs__used_refresh();
    lfs_file_t f; int r = lfs_file_opencfg(&fs_lfs, &f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &fcfg);
    if (r) return r;
    lfs_ssize_t n = lfs_file_write(&fs_lfs, &f, src, len);
    r = lfs_file_close(&fs_lfs, &f);
    return n < 0 ? (int)n : r ? r : (int)n;
}
int fs_remove(const char *path) { return lfs_remove(&fs_lfs, path); }
/* one file written piece by piece (wuplink receiving a large app) */
static lfs_file_t sf; static int sf_open;
static u8 sbuf[256] __attribute__((aligned(4)));
static struct lfs_file_config scfg = { .buffer = sbuf };
int fs_stream_open(const char *path) {
    if (sf_open) { lfs_file_close(&fs_lfs, &sf); sf_open = 0; }
    fs__used_refresh();
    int r = lfs_file_opencfg(&fs_lfs, &sf, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &scfg);
    sf_open = r == 0;
    return r;
}
int fs_stream_write(const void *src, u32 len) {
    if (!sf_open) return -1;
    lfs_ssize_t n = lfs_file_write(&fs_lfs, &sf, src, len);
    return n == (lfs_ssize_t)len ? 0 : -1;
}
int fs_stream_close(void) {
    if (!sf_open) return -1;
    sf_open = 0;
    return lfs_file_close(&fs_lfs, &sf);
}
int fs_filesize(const char *path) { struct lfs_info i; int r = lfs_stat(&fs_lfs, path, &i); return r ? r : (int)i.size; }
