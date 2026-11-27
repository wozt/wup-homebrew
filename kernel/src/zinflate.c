/* inflate_raw: a raw deflate stream (an app image as stored, the menu's) inflated whole into dst (*n: its room, then
 * its size). zlib's inflate, its state from a small arena here (Z_SOLO: no C library); the output is its own window.
 * (A file of its own: zlib.h names a crc32 of its own, not the kernel's.) */
#include <stddef.h>
#include "zlib.h"

void *memset(void *d, int c, size_t n);

static unsigned char zarena[12 * 1024] __attribute__((aligned(8)));
static unsigned zused;
static voidpf zal(voidpf o, uInt items, uInt size) {
    (void)o;
    unsigned n = (items * size + 7u) & ~7u;
    if (zused + n > sizeof zarena) return Z_NULL;
    zused += n;
    return zarena + zused - n;
}
static void zfr(voidpf o, voidpf p) { (void)o; (void)p; }

int inflate_raw(unsigned char *dst, unsigned *n, const unsigned char *src, unsigned len) {
    z_stream z;
    memset(&z, 0, sizeof z);
    zused = 0; z.zalloc = zal; z.zfree = zfr;
    if (inflateInit2(&z, -15) != Z_OK) return -1;
    z.next_in = (Bytef *)src; z.avail_in = len; z.next_out = dst; z.avail_out = *n;
    int r = inflate(&z, Z_FINISH);
    *n = (unsigned)z.total_out;
    inflateEnd(&z);
    return r == Z_STREAM_END ? 0 : -1;
}
