/* UARTs (0xF0004C00 + n*0x40, n = 0..2), as used by the boot ROM's UART loader (ROM 0x3c4: UART1,
 * polls status bit 0, reads the RX register) and by UART-boot payloads such as wuptool / wupbootdump
 * (register names from their regs.h):
 *   +0x00 RX   +0x04 TX   +0x08 IRQ enable   +0x0C IRQ status (2 = TX ready)
 *   +0x1C status: bit 0 = RX data available, bits 5/6 = TX ready
 * TX can also be fed by the SPI DMA engine (peripheral 4..6 = UART0..2, see devices.c).
 *
 * --uart-pty (or UART_PTY=1) bridges UART1 to a host pseudo-terminal, so host tools that expect a
 * USB serial adapter (e.g. wuptool.py) can talk to the emulated GamePad. */
#define _DARWIN_C_SOURCE 1
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE 1
#include "dev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

static uint32_t regs[3][0x40 / 4];
static int pty = -1, pty_slave = -1;
#define RXQ (1 << 20)
static uint8_t rxq[RXQ]; static uint32_t rx_head, rx_n;
static uint64_t tx_count, rx_count;

static void rx_poll(void) {
    if (pty < 0 || rx_n > RXQ - 4096) return;
    uint8_t buf[4096];
    ssize_t n = read(pty, buf, sizeof buf);
    for (ssize_t i = 0; i < n; i++) { rxq[(rx_head + rx_n) % RXQ] = buf[i]; rx_n++; }
    rx_count += n > 0 ? n : 0;
}

static uint8_t txbuf[4096]; static int tx_n;
static void tx_flush(void) {
    int o = 0;
    while (o < tx_n) {
        ssize_t w = write(pty, txbuf + o, tx_n - o);
        if (w > 0) o += w; else if (errno == EAGAIN || errno == EINTR) usleep(100); else break;
    }
    tx_n = 0;
}
void uart_tx_byte(int ch, uint8_t b) {
    tx_count++;
    if (ch == 1 && pty >= 0) { txbuf[tx_n++] = b; if (tx_n == sizeof txbuf) tx_flush(); }
    else if (getenv("UART_LOG")) fputc(b, stderr);
}

static uint32_t uart_read(void *ctx, uint32_t off, int size) {
    int ch = off / 0x40; uint32_t r = off & 0x3f;
    if (ch > 2) return 0;
    switch (r) {
    case 0x00:
        if (ch == 1) { if (!rx_n) rx_poll(); if (rx_n) { uint8_t b = rxq[rx_head]; rx_head = (rx_head + 1) % RXQ; rx_n--; return b; } }
        return 0;
    case 0x0c: return 2;                                       /* TX ready, nothing pending */
    case 0x1c: if (ch == 1 && !rx_n) rx_poll(); return (ch == 1 && rx_n ? 1 : 0) | 0x60;
    }
    return regs[ch][r / 4];
}
static void uart_write(void *ctx, uint32_t off, uint32_t val, int size) {
    int ch = off / 0x40; uint32_t r = off & 0x3f;
    if (ch > 2) return;
    if (r == 0x04) { uart_tx_byte(ch, (uint8_t)val); return; }
    regs[ch][r / 4] = val;
}

void uart_tick(void) { static int n; if (++n >= 64) { n = 0; rx_poll(); if (tx_n) tx_flush(); } }

void uart_init(void) {
    bus_register(0xf0004c00, 0xc0, "uart", NULL, uart_read, uart_write);
    if (!getenv("UART_PTY")) return;
    pty = posix_openpt(O_RDWR | O_NOCTTY);
    if (pty < 0 || grantpt(pty) || unlockpt(pty)) { perror("[uart] pty"); pty = -1; return; }
    const char *name = ptsname(pty);
    pty_slave = open(name, O_RDWR | O_NOCTTY);              /* keep one end open; raw mode */
    struct termios t; if (pty_slave >= 0 && !tcgetattr(pty_slave, &t)) { cfmakeraw(&t); tcsetattr(pty_slave, TCSANOW, &t); }
    fcntl(pty, F_SETFL, O_NONBLOCK);
    fprintf(stderr, "[uart] UART1 is on %s\n", name);
    FILE *f = getenv("UART_PTY_FILE") ? fopen(getenv("UART_PTY_FILE"), "w") : NULL;
    if (f) { fprintf(f, "%s\n", name); fclose(f); }
}
void uart_stats(uint64_t *tx, uint64_t *rx) { *tx = tx_count; *rx = rx_count; }
