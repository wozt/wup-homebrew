/* menu_slot: a new Homebrew Menu into the menu slot (include/wup_system.h; system apps only). The slot is wup-menu's
 * (tools/mkflash.py, boot/loader.c): header sector 0x30000, image 0x31000 up to 0x8FFFF, and the menu's WUP_BIGMEM
 * code as a {"WUPB", stored, raw, crc} blob right after the image on a sector boundary (libwup's big.c finds it).
 * Each 4 KB sector is erased, written and read back through the firmware's flash driver; the header goes last.
 * homebrew_menu.bin is the header sector, then the stored image (the header's size), then the blob: past it the
 * extra bytes land at the next sector boundary, where big.c looks for them. */
#include <string.h>
#include "wup_system.h"
#include "armcpu.h"
typedef uint32_t u32; typedef uint8_t u8;
typedef int (*rw_fn)(u32 h, u32 addr, void *buf, u32 len, u32 *done);
typedef int (*er_fn)(u32 h, u32 first, u32 count);
#define FL_READ  ((rw_fn)0x00009141)
#define FL_WRITE ((rw_fn)0x00008fb3)
#define FL_ERASE4K ((er_fn)0x000092b9)
#define HANDLE (*(volatile u32 *)0x000bb5f4)
#define BUF ((u8 *)0x0034e000u)                       /* uncached, DMA-reachable: the flash bounce buffer (handoff.h) */
#define SLOT_HDR 0x00030000u
#define SLOT_IMG 0x00031000u
#define SLOT_END 0x00090000u
#define BIGB     0x42505557u                          /* "WUPB": the blob's header magic */

static u8 hdr[20], sec[4096];
static u32 fill, crc, size, open_;
static u32 img_done;                                 /* image bytes accepted so far (size + 1: the blob's phase) */
static u32 xbase, xat, xfill;                        /* the blob: base, bytes accepted (pending included), pending */
static u8 xmagic[4]; static u32 xmagic_n; static int xok;   /* the blob's first bytes, checked against BIGB */

static int sector(u32 addr, const u8 *d, u32 n) {
    u32 done;
    if (FL_ERASE4K(HANDLE, addr >> 12, 1)) return -1;
    if (!n) return 0;
    memcpy(BUF, d, n);
    if (FL_WRITE(HANDLE, addr, BUF, n, &done)) return -1;
    memset(BUF, 0, n);
    return FL_READ(HANDLE, addr, BUF, n, &done) || memcmp(BUF, d, n) ? -1 : 0;
}
static u32 align4k(u32 n) { return (n + 0xfff) & ~0xfffu; }
/* n bytes at flash `addr` through the sector buffer; *done counts them all, *pend the buffered tail */
static int put(u32 addr, const u8 *d, u32 n, u32 *done, u32 *pend) {
    while (n) {
        u32 k = n < 4096 - *pend ? n : 4096 - *pend;
        memcpy(sec + *pend, d, k); *pend += k; d += k; n -= k; *done += k;
        if (*pend == 4096) { if (sector(addr + *done - 4096, sec, 4096)) return -1; *pend = 0; }
    }
    return 0;
}
int wup_menu_slot_begin(const uint8_t header[20]) {
    u32 h[5]; memcpy(h, header, 20);
    if (h[0] != 0x554e4d57u || h[1] < 64 || SLOT_IMG + h[1] > SLOT_END) return -1;   /* "WMNU", and it fits */
    memcpy(hdr, header, 20); size = h[1];
    fill = crc = img_done = xbase = xat = xfill = xmagic_n = xok = 0; open_ = 1;
    return sector(SLOT_HDR, 0, 0);                    /* the old menu's header goes first */
}
int wup_menu_slot_write(const void *data, uint32_t n) {
    const u8 *d = data;
    if (!open_) return -1;
    if (img_done <= size) {                           /* the stored image, at SLOT_IMG */
        u32 k = size - img_done; if (k > n) k = n;
        crc = crc32_update(crc, d, k);
        if (put(SLOT_IMG, d, k, &img_done, &fill)) return -2;
        d += k; n -= k;
        if (img_done == size) {                       /* complete: its last sector may be partial; the blob's boundary */
            if (fill && sector(SLOT_IMG + img_done - fill, sec, fill)) return -2;
            fill = 0; xbase = SLOT_IMG + align4k(size); img_done = size + 1;
        }
        if (!n) return 0;
    }
    /* the extra bytes: the bigmem blob, from xbase on */
    if (!xok) {                                       /* its first bytes must be the WUPB magic */
        u32 k = 4 - xmagic_n < n ? 4 - xmagic_n : n;
        memcpy(xmagic + xmagic_n, d, k); xmagic_n += k; d += k; n -= k;
        if (xmagic_n < 4) return 0;
        u32 m; memcpy(&m, xmagic, 4);
        if (m != BIGB) return -1;
        if (put(xbase, xmagic, 4, &xat, &xfill)) return -2;   /* the magic is part of the blob */
        xok = 1;
        if (!n) return 0;
    }
    if (xbase + align4k(xat + n) > SLOT_END) return -1;
    if (put(xbase, d, n, &xat, &xfill)) return -2;
    return 0;
}
int wup_menu_slot_end(void) {
    if (!open_) return -1;
    open_ = 0;
    if (img_done != size + 1 || (xmagic_n && !xok)) return -3;   /* the image was not complete (or a truncated blob) */
    u32 h[5]; memcpy(h, hdr, 20);
    if (crc != h[2]) return -3;
    if (xfill && sector(xbase + xat - xfill, sec, xfill)) return -2;
    static u8 page[4096];
    memset(page, 0xff, sizeof page); memcpy(page, hdr, 20);
    return sector(SLOT_HDR, page, 4096) ? -2 : 0;     /* the new menu is valid from here on */
}
