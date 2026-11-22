/* fs: littlefs in the GamePad's spare copy of its language data (8 MB in one piece).
 * The pad keeps its message screens (pairing, errors, low battery...: pictures, in every language) twice, like its
 * firmware: Lang1 (0x900000-0x10FFFFF) and Lang2 (0x1100000-0x18FFFFF). A selector in its settings memory (the UIC's
 * EEPROM, the firmware's cache of it + 0x256: selector, 3 bytes of version, CRC-16) says which one it uses: 1 = Lang2,
 * anything else = Lang1 (the reader at boot, LVC 0x3a93a). A language update is written to the other copy and the
 * selector flipped (LVC 0x2b146, 0x2baf0): so which copy is in use differs from pad to pad. Checked in wup-emu with
 * every flash read logged (FLASH_LOG): selector 0: 362 reads in Lang1, none in Lang2; selector 1: 264 in Lang2, none in
 * Lang1; and a pad whose copy in use is not language data shows a blank grey screen for every message.
 * So: the filesystem lives in the copy the pad does NOT use, asked of the firmware each time (LVC 0x3ba48); if it
 * cannot say, there is no filesystem: never a guess. (Homebrew Menu 1.0.0 always took Lang2: on a pad using Lang2 it
 * formatted over the screens in use. Such a pad is recognised, the copy in use holding our filesystem and the other
 * one not, and the menu repairs it: the selector is pointed at the other copy, whole and only older, with the
 * firmware's own setter (LVC 0x3baa4); the files stay where they are.)
 * No language update comes to these pads (firmware 7677 is Nintendo's last; the installers send firmware only). Were
 * one sent, it would replace these files with a complete language copy and flip the selector: lost files, a working pad.
 * The rest of ours stays in the gap below Product1 (0x10000-0xFFFFF): the loader, the menu slot, the menu's settings.
 * The flash driver DMAs: data passes through the uncached buffer at 0x220000. */
#include <stdint.h>
#include <string.h>
#include "littlefs/lfs.h"
#include "fs.h"
#include "armcpu.h"
typedef uint32_t u32; typedef uint8_t u8;
typedef int (*rw_fn)(u32 h, u32 addr, void *buf, u32 len, u32 *done);
typedef int (*er_fn)(u32 h, u32 first, u32 count);
#define FL_READ  ((rw_fn)0x00009141)
#define FL_WRITE ((rw_fn)0x00008fb3)
#define FL_ERASE4K ((er_fn)0x000092b9)
#define HANDLE (*(volatile u32 *)0x000bb5f4)
#ifndef FS_BOUNCE
#define FS_BOUNCE 0x00220000u            /* the menu: uncached, DMA-reachable. Apps (libwup): 0x34E000 (include/handoff.h) */
#endif
#define BOUNCE ((u8 *)FS_BOUNCE)

static struct { u32 lo, hi; } ext[1];              /* where the filesystem is: place() (tools: wup-menu/tools/fsimage.c) */
#define NEXT (int)(sizeof ext / sizeof ext[0])
static u32 block_addr(lfs_block_t b) {
    for (int i = 0; i < NEXT; i++) { u32 n = (ext[i].hi - ext[i].lo) >> 12; if (b < n) return ext[i].lo + (b << 12); b -= n; }
    return 0xffffffffu;
}
static u32 total_blocks(void) { u32 n = 0; for (int i = 0; i < NEXT; i++) n += (ext[i].hi - ext[i].lo) >> 12; return n; }

static int bd_read(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, void *buf, lfs_size_t size) {
    (void)c; u32 a = block_addr(b); if (a == 0xffffffffu) return LFS_ERR_IO;
    u8 *d = buf;
    while (size) {
        u32 k = size < 4096 ? size : 4096, done = 0;
        if (FL_READ(HANDLE, a + off, BOUNCE, k, &done)) return LFS_ERR_IO;
        cache_flush(BOUNCE, 4096);                      /* the DMA wrote it: drop any stale cached copy first */
        memcpy(d, BOUNCE, k); d += k; off += k; size -= k;
    }
    return 0;
}
static int bd_prog(const struct lfs_config *c, lfs_block_t b, lfs_off_t off, const void *buf, lfs_size_t size) {
    (void)c; u32 a = block_addr(b); if (a == 0xffffffffu) return LFS_ERR_IO;
    const u8 *s = buf;
    while (size) {
        u32 k = size < 4096 ? size : 4096, done = 0;
        memcpy(BOUNCE, s, k);
        cache_clean(BOUNCE, 4096);                      /* write-back before the DMA reads it */
        if (FL_WRITE(HANDLE, a + off, BOUNCE, k, &done)) return LFS_ERR_IO;
        s += k; off += k; size -= k;
    }
    return 0;
}
static int bd_erase(const struct lfs_config *c, lfs_block_t b) {
    (void)c; u32 a = block_addr(b); if (a == 0xffffffffu) return LFS_ERR_IO;
    return FL_ERASE4K(HANDLE, a >> 12, 1) ? LFS_ERR_IO : 0;
}
static int bd_sync(const struct lfs_config *c) { (void)c; return 0; }

/* ---- which copy: the firmware's selector, and what the two copies hold */
#define LANG1 0x900000u
#define LANG2 0x1100000u
#define LANG_SIZE 0x800000u
typedef struct { u8 selector; u8 pad[3]; u32 version; } lang_t;
#define FW_LANG_GET ((int (*)(lang_t *))0x0003ba49)       /* 0: read (from the EEPROM cache); else not loaded */
#define FW_LANG_SET ((int (*)(const lang_t *))0x0003baa5) /* into the cache and the EEPROM, with its CRC */
static int has_fs(u32 base) {                         /* littlefs's superblock: "littlefs" at +8 of block 0 or 1 */
    u32 done;
    for (int b = 0; b < 2; b++) {
        if (FL_READ(HANDLE, base + (u32)b * 4096, BOUNCE, 16, &done)) return -1;
        cache_flush(BOUNCE, 4096);
        if (!memcmp(BOUNCE + 8, "littlefs", 8)) return 1;
    }
    return 0;
}
static int has_screens(u32 base) {                    /* language data: how many pictures (the boot reader's first word), then their table */
    u32 done, n;
    if (FL_READ(HANDLE, base, BOUNCE, 4, &done)) return 0;
    cache_flush(BOUNCE, 4096);
    memcpy(&n, BOUNCE, 4);
    return n >= 1 && n <= 1024;
}
/* -> ext[]; 0, or < 0: not known (no filesystem). repair: the menu, which may put a 1.0.0-damaged pad right */
static int place(int repair) {
    lang_t l = { 0 };
    if (FW_LANG_GET(&l)) return -1;
    u32 used = l.selector == 1 ? LANG2 : LANG1, spare = used == LANG2 ? LANG1 : LANG2, at = spare;
    int fu = has_fs(used), fsp = has_fs(spare);
    if (fu < 0 || fsp < 0) return -1;
    if (fu == 1 && fsp == 0) {                         /* the copy in use holds our files (1.0.0 on a pad using Lang2) */
        at = used;                                     /* the files stay; with the selector on the other copy it is the spare again */
#ifndef FS_NO_FORMAT
        if (repair && has_screens(spare)) {
            lang_t to = { (u8)(spare == LANG2 ? 1 : 0), { 0 }, l.version }, now = { 0 };
            FW_LANG_SET(&to);
            if (FW_LANG_GET(&now) || now.selector != to.selector) at = used;   /* (not taken: as it was) */
        }
#else
        (void)repair; (void)has_screens;
#endif
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
    return 0;
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
int fs_flash_read(u32 addr, void *dst, u32 len) {        /* raw flash, through the bounce buffer (len <= 4096) */
    u32 done;
    if (len > 4096 || FL_READ(HANDLE, addr, BOUNCE, len, &done)) return -1;
    cache_flush(BOUNCE, 4096);
    memcpy(dst, BOUNCE, len);
    return 0;
}
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
