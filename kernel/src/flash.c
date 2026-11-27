/* The SPI flash (32 MiB; chip select 0xF00050F8, spi.c). The commands are the stock firmware's and the UART flasher's (both
 * used on the pad): 0x9F ID, 0x06 write enable, 0x05 status (bit 0 busy), 0xB7 / 0xE9 4-byte addresses on / off
 * (LVC switches it on; it reads with 0x03 + 4 address bytes), 0x02 program a page (256 bytes), 0x20 erase 4 KB:
 * in 4-byte mode these take 4 address bytes. NOT 0x12 / 0x21 (the dedicated 4-byte opcodes): the pad's Micron N25Q256
 * ignores them - erase rejected, program busy but nothing stored (proven on the pad 2026-11-19, flag status 0x81).
 * flash_done() switches 4-byte addressing off again before anything else boots: the boot ROM and stage 2 read with
 * 3-byte addresses, and a soft reset does not reset the chip.
 *
 * flash_erase4k / flash_program write anywhere apps could under Nintendo's firmware, except the start of the flash
 * (flash_may_write): Nintendo's stage 2 and error log (0x0-0xEFFF) are never written. */
#include "os.h"

#define CS 0xf00050f8u

static int cmd1(u8 c) { return spi_xfer(CS, &c, 1, NULL, 0, NULL, 0); }
static void put_addr(u8 *b, u8 c, u32 a) { b[0] = c; b[1] = (u8)(a >> 24); b[2] = (u8)(a >> 16); b[3] = (u8)(a >> 8); b[4] = (u8)a; }

static int wait_ready(u32 limit_us) {
    static const u8 c = 0x05;
    u8 st;
    for (u32 t = time_us(); ; ) {
        if (spi_xfer(CS, &c, 1, NULL, 0, &st, 1)) return -1;
        if (!(st & 1)) return 0;
        u32 waited = time_us() - t;
        if (waited > limit_us) return -2;
        if (waited > 2000 && sched_current() >= 0) sched_sleep(1);   /* (an erase: 0.24 s for 4 KB, 0.7 s for 64 KB;
                                                                        the other tasks run meanwhile) */
    }
}

int flash_init(u8 id[3]) {
    static const u8 c = 0x9f;
    int e = spi_xfer(CS, &c, 1, NULL, 0, id, 3);
    if (e) return e;
    if (wait_ready(100000)) return -2;
    cmd1(0x06);                                           /* (the UART flasher enables writing first: some chips need it) */
    return cmd1(0xb7);
}
void flash_done(void) { wait_ready(100000); cmd1(0x06); cmd1(0xe9); }

int flash_read(u32 addr, void *p, u32 n) {
    u8 b[5];
    put_addr(b, 0x03, addr);
    return spi_xfer(CS, b, 5, NULL, 0, p, n);
}

int flash_may_write(u32 addr, u32 n) {
    return addr >= 0xf000u && addr + n <= 0x2000000u && addr + n >= addr;
}

static int erase_raw(u32 addr) {
    u8 b[5];
    if (wait_ready(100000) || cmd1(0x06)) return -1;
    put_addr(b, 0x20, addr);
    if (spi_xfer(CS, b, 5, NULL, 0, NULL, 0)) return -1;
    return wait_ready(3000000);                         /* (the pad's Micron: 236 ms) */
}
static int program_raw(u32 addr, const void *p, u32 n) {
    const u8 *s = p;
    while (n) {
        u32 k = 0x100 - (addr & 0xff);
        if (k > n) k = n;
        u8 b[5];
        if (wait_ready(100000) || cmd1(0x06)) return -1;
        put_addr(b, 0x02, addr);
        if (spi_xfer(CS, b, 5, s, k, NULL, 0)) return -1;
        if (wait_ready(100000)) return -2;
        addr += k; s += k; n -= k;
    }
    return 0;
}

int flash_erase4k(u32 addr) {
    if ((addr & 0xfff) || !flash_may_write(addr, 0x1000)) return -9;
    return erase_raw(addr);
}
int flash_erase64k(u32 addr) {                         /* (a 64 KB block: about 3 times a 4 KB sector's time, for 16 times as much) */
    if ((addr & 0xffff) || !flash_may_write(addr, 0x10000)) return -9;
    u8 b[5];
    if (wait_ready(100000) || cmd1(0x06)) return -1;
    put_addr(b, 0xd8, addr);
    if (spi_xfer(CS, b, 5, NULL, 0, NULL, 0)) return -1;
    return wait_ready(5000000);
}
int flash_program(u32 addr, const void *p, u32 n) {
    if (!flash_may_write(addr, n)) return -9;
    return program_raw(addr, p, n);
}
