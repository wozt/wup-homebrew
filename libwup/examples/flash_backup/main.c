/* Flash Backup: stream the GamePad's whole SPI flash (32 MiB) to a computer.
 *
 * fs_flash_read() reads the raw flash through libwup's bounce buffer (4 KiB per call).
 * The app connects to FLASH_DUMP_HOST:FLASH_DUMP_PORT and sends:
 *   "WUPFLASH1" + u32 total_size (little endian), then the image, chunk after chunk.
 * On the computer: python3 tools/flash-dump-recv.py OUT.bin
 *
 * The dump is the pad's real flash: boot sectors, both firmware banks, settings,
 * littlefs. Keep it safe — it contains the pairing key. B aborts, the menu comes back. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "wup.h"
#include "fs.h"

WUP_APP("Flash Backup");

#define FLASH_DUMP_HOST "192.168.2.100"      /* this computer */
#define FLASH_DUMP_PORT 9998
#define FLASH_SIZE      0x02000000u          /* 32 MiB */
#define CHUNK           4096

static int row;
static void line(const char *fmt, ...) {
    char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    gfx_text(&font_body, 40, 46 + 26 * row++, b, RGB(220, 228, 245));
    gfx_present_all();
}
static void status(int pct, uint32_t done) {
    char b[64]; snprintf(b, sizeof b, "%3d%%  %u / %u MiB", pct, done >> 20, FLASH_SIZE >> 20);
    gfx_text(&font_body, 40, GFX_H - 60, b, RGB(140, 200, 255));
    gfx_present_all();
}
static int send_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w <= 0) return -1;
        b += w; n -= (size_t)w;
    }
    return 0;
}

int main(void) {
    wup_screen();
    gfx_vgrad24(0, 0, GFX_W, GFX_H, C24(24, 44, 96), C24(6, 10, 30));
    gfx_text_c(&font_big, GFX_W / 2, 60, "Flash Backup", RGB(255, 255, 255));
    row = 0;

    if (wup_net_init()) { line("Wi-Fi: the menu was not connected"); goto out; }
    line("Target: %s:%d", FLASH_DUMP_HOST, FLASH_DUMP_PORT);
    line("Waiting for the computer ...");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { line("socket: %s", strerror(errno)); goto out; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(FLASH_DUMP_PORT);
    if (inet_pton(AF_INET, FLASH_DUMP_HOST, &a.sin_addr) != 1) { line("bad host"); close(fd); goto out; }
    /* nonblocking connect with a 5 s deadline; B aborts while waiting */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&a, sizeof a);
    int ok = 0;
    if (r == 0) ok = 1;
    else if (errno == EINPROGRESS) {
        for (int i = 0; i < 50 && !(wup_buttons() & WUP_B); i++) {
            struct pollfd p = { fd, POLLOUT, 0 };
            if (poll(&p, 1, 100) > 0) {
                int e = 0; socklen_t el = sizeof e;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
                if (!e) { ok = 1; }
                break;
            }
        }
    }
    if (!ok) { line("connect failed"); close(fd); goto out; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    line("Connected. Dumping %u MiB ...", FLASH_SIZE >> 20);

    struct { char magic[8]; uint32_t size; } hdr = { { 'W','U','P','F','L','A','S','H' }, FLASH_SIZE };
    if (send_all(fd, &hdr, sizeof hdr)) { line("send header failed"); close(fd); goto out; }

    static uint8_t buf[CHUNK];
    for (uint32_t off = 0; off < FLASH_SIZE; off += CHUNK) {
        if (wup_buttons() & WUP_B) { line("Aborted at %u MiB", off >> 20); close(fd); goto out; }
        if (fs_flash_read(off, buf, CHUNK)) { line("flash read failed at %08x", off); close(fd); goto out; }
        if (send_all(fd, buf, CHUNK)) { line("send failed at %u MiB (%s)", off >> 20, strerror(errno)); close(fd); goto out; }
        status((int)(off * 100 / FLASH_SIZE), off);
    }
    line("Done: %u MiB sent.", FLASH_SIZE >> 20);
    line("Verify the file on the computer.");
    close(fd);
out:
    line("");
    line("Press B to return");
    while (wup_buttons() & WUP_B) wup_sleep(16);
    while (!(wup_buttons() & WUP_B)) wup_sleep(16);
    return 0;
}
