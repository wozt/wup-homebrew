/* Netfs Browse: a walk through the /net mount, the way an emulator will use it.
 *
 * 1. opendir("/net/ROMs") + readdir: the remote folder, like a local one.
 * 2. open() the first file found, fstat() its size.
 * 3. read() 4 KiB blocks at several offsets (start, middle, end): what fseek+fread become
 *    for a ROM loaded on demand — the blocks carry absolute offsets, no full copy in RAM.
 * 4. Each block gets a checksum shown on screen; identical reads at the same offset must
 *    match (the file on the computer is the reference).
 *
 * Needs the Network Storage configured in the menu (Settings > Network > Network Storage)
 * and tools/netfs-server running on the computer. B returns to the menu. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "wup.h"

WUP_APP("Netfs Browse");

static int row;
static void line(const char *fmt, ...) {
    char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    gfx_text(&font_body, 40, 46 + 26 * row++, b, RGB(220, 228, 245));
    gfx_present_all();
}
static uint32_t fnv(const void *p, size_t n) {          /* FNV-1a: a cheap verifiable checksum */
    const uint8_t *b = p; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

int main(void) {
    wup_screen();
    gfx_vgrad24(0, 0, GFX_W, GFX_H, C24(24, 44, 96), C24(6, 10, 30));
    gfx_text_c(&font_big, GFX_W / 2, 60, "/net", RGB(255, 255, 255));
    row = 0;

    line("Listing /net/ROMs ...");
    DIR *d = opendir("/net/ROMs");
    if (!d) { line("opendir failed: %s", strerror(errno)); goto out; }
    struct dirent *de;
    char first[256] = "";
    int nfiles = 0, ndirs = 0;
    while ((de = readdir(d))) {
        if (de->d_type == DT_DIR) ndirs++;
        else if (!first[0] && de->d_type == DT_REG) snprintf(first, sizeof first, "%s", de->d_name);
        if (row < 12) line("  %c %s", de->d_type == DT_DIR ? 'd' : 'f', de->d_name);
        nfiles += de->d_type != DT_DIR;
    }
    closedir(d);
    line("%d file(s), %d folder(s)", nfiles, ndirs);
    if (!first[0]) { line("No file in /net/ROMs"); goto out; }

    char path[320]; snprintf(path, sizeof path, "/net/ROMs/%s", first);
    line("Opening %s", first);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { line("open failed: %s", strerror(errno)); goto out; }
    struct stat st;
    if (fstat(fd, &st)) { line("fstat failed: %s", strerror(errno)); close(fd); goto out; }
    line("size: %llu bytes", (unsigned long long)st.st_size);

    /* blocks at several offsets: the random-access reads an emulator needs */
    static uint8_t blk[4096];
    uint64_t offs[4] = { 0, st.st_size / 2, st.st_size > 4096 ? st.st_size - 4096 : 0, 65536 };
    for (int i = 0; i < 4 && row < 20; i++) {
        uint64_t o = offs[i];
        if (o >= (uint64_t)st.st_size) continue;
        if (lseek(fd, (off_t)o, SEEK_SET) < 0) { line("lseek %llu failed", (unsigned long long)o); continue; }
        ssize_t n = read(fd, blk, sizeof blk);
        if (n <= 0) { line("read at %llu: %s", (unsigned long long)o, n ? strerror(errno) : "EOF"); continue; }
        line("  @%-10llu %5d B  fnv=%08x", (unsigned long long)o, (int)n, fnv(blk, (size_t)n));
        /* read it again: the same offset must give the same bytes */
        if (lseek(fd, (off_t)o, SEEK_SET) == 0) {
            static uint8_t blk2[4096];
            ssize_t n2 = read(fd, blk2, (size_t)n);
            line("    again: %s", n2 == n && !memcmp(blk, blk2, (size_t)n) ? "identical" : "MISMATCH");
        }
    }
    close(fd);
out:
    line("");
    line("Press B to return");
    while (wup_buttons() & WUP_B) wup_sleep(16);
    while (!(wup_buttons() & WUP_B)) wup_sleep(16);
    return 0;
}
