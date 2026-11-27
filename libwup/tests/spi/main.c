/* tests/spi: the flash's read speed against the SPI clock (app.cfg). The transactions are the kernel's own
 * (kernel/src/spi.c: DMA through a buffer of whole cache lines), made here with interrupts off, and only while neither
 * chip on the bus is selected (the kernel's tasks use it too: the UIC, the filesystem). Read commands alone. */
#include <stdio.h>
#include <string.h>
#include "wup.h"
#include "gfx.h"
#include "wupk.h"
#include "armcpu.h"

WUP_APP("SPI Test");

#define REG(a)  (*(volatile uint32_t *)(a))
#define SPI(o)  REG(0xf0004400u + (o))
#define DMA(o)  REG(0xf0004040u + (o))
#define CS_FLASH 0xf00050f8u
#define CS_UIC   0xf00050fcu
#define AT      0x00200000u                           /* (in the filesystem: any data) */
#define N       0x10000u
#define CHUNK   4096u

static uint8_t buf[CHUNK] __attribute__((aligned(32)));
static uint8_t ref[N], got[N];

__attribute__((target("arm"))) static uint32_t irq_off(void) {
    uint32_t c; __asm__ volatile("mrs %0, cpsr\n orr r1, %0, #0xc0\n msr cpsr_c, r1" : "=r"(c) :: "r1", "memory"); return c;
}
__attribute__((target("arm"))) static void irq_on(uint32_t c) { __asm__ volatile("msr cpsr_c, %0" :: "r"(c) : "memory"); }
static void wait_us(uint32_t us) { for (uint32_t t = wup_us(); wup_us() - t < us; ) ; }
static int dma(uint32_t mode, uint32_t n) {
    uint32_t t; int err = 0;
    for (t = wup_us(); DMA(0x00) & 1; ) if (wup_us() - t > 20000) break;
    DMA(0x00) = 2; DMA(0x04) = mode; DMA(0x08) = 0; DMA(0x0c) = 0; DMA(0x10) = n - 1; DMA(0x14) = (uint32_t)buf; DMA(0x00) = 1;
    for (t = wup_us(); DMA(0x00) & 1; ) if (wup_us() - t > 20000) { err = -1; break; }
    DMA(0x00) = 2;
    return err;
}
/* one transaction: the command bytes out, n bytes in; -3: the bus was in use */
static int xfer(uint32_t clock, const uint8_t *out, uint32_t nout, uint8_t *in, uint32_t n) {
    uint32_t c = irq_off();
    int err = 0;
    if ((REG(CS_FLASH) & 0x100) == 0 || (REG(CS_UIC) & 0x100) == 0) { irq_on(c); return -3; }
    REG(0xf0004000u) |= 0x8003;
    SPI(0x00) = clock;
    REG(CS_FLASH) = 0xc200;
    SPI(0x04) &= ~0x200u; SPI(0x04) &= ~0x2u;
    memcpy(buf, out, nout); cache_clean(buf, nout);
    SPI(0x18) |= 0x80;
    err = dma(5, nout);
    for (uint32_t t = wup_us(); !err && !(SPI(0x08) & 0x80); ) if (wup_us() - t > 20000) err = -1;
    SPI(0x08) = 0x80; SPI(0x18) &= ~0x80u;
    if (!err) {
        SPI(0x04) |= 0x40; wait_us(60); SPI(0x04) &= ~0x40u;
        SPI(0x04) |= 0x2;
        SPI(0x20) = n;
        cache_flush(buf, n);
        err = dma(4, n) ? -2 : 0;
        cache_flush(buf, n);
        if (!err) memcpy(in, buf, n);
        SPI(0x20) = 0;
    }
    SPI(0x04) |= 0x200;
    REG(CS_FLASH) = 0xc300;
    SPI(0x00) = 0x8018;                                /* (the kernel's own, as it sets it) */
    irq_on(c);
    return err;
}
static int xfer_retry(uint32_t clock, const uint8_t *out, uint32_t nout, uint8_t *in, uint32_t n) {
    for (int i = 0; i < 200; i++) { int r = xfer(clock, out, nout, in, n); if (r != -3) return r; wup_sleep(1); }
    return -3;
}

int main(void) {
    static char line[96], all[4096];
    int y = 40, len = 0;
    wup_screen();
    gfx_fill(0, 0, GFX_W, GFX_H, RGB(0, 0, 0));
    gfx_text(&font_body, 20, y, "Flash read speed (reads only)...", RGB(255, 255, 255)); gfx_present_all();
    uint32_t t0 = wup_us();
    int r = WUPK->flash_read(AT, ref, N);
    uint32_t tk = wup_us() - t0;
    uint8_t id[4] = { 0 }; const uint8_t c9f = 0x9f;
    xfer_retry(0x8018, &c9f, 1, id, 3);
    len += snprintf(all + len, sizeof all - len, "kernel read: %d, %u KB/s; flash id %02x %02x %02x; cs %08x %08x; clock reg %08x\n",
                    r, (unsigned)(N * 1000u / (tk ? tk : 1)), id[0], id[1], id[2], (unsigned)REG(CS_FLASH), (unsigned)REG(CS_UIC), (unsigned)SPI(0x00));
    static const uint8_t divs[] = { 0x18, 0x10, 0x0c, 0x08, 0x06, 0x04, 0x03, 0x02, 0x01 };
    for (int fast = 0; fast < 2; fast++)
        for (unsigned d = 0; d < sizeof divs; d++) {
            uint32_t bad = 0, us = 0; int err = 0;
            for (int pass = 0; pass < 3 && !err; pass++) {
                uint32_t t = wup_us();
                for (uint32_t o = 0; o < N && !err; o += CHUNK) {
                    uint32_t a = AT + o;
                    uint8_t cmd[6] = { fast ? 0x0b : 0x03, (uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, 0 };
                    err = xfer_retry(0x8000u | divs[d], cmd, fast ? 6 : 5, got + o, CHUNK);
                }
                us += wup_us() - t;
                if (!err) for (uint32_t i = 0; i < N; i++) bad += got[i] != ref[i];
            }
            snprintf(line, sizeof line, "%s div 0x%02x: err %d, %u wrong bytes, %u KB/s", fast ? "fast 0x0B" : "read 0x03", divs[d], err, (unsigned)bad,
                     (unsigned)(us ? (uint64_t)N * 3 * 1000u / us : 0));
            len += snprintf(all + len, sizeof all - len, "%s\n", line);
            y += 22; gfx_text(&font_small, 20, y, line, bad || err ? RGB(255, 120, 120) : RGB(160, 255, 160)); gfx_present_all();
        }
    FILE *f = fopen("/spitest.txt", "w");
    if (f) { fwrite(all, 1, (size_t)len, f); fclose(f); }
    gfx_text(&font_body, 20, GFX_H - 16, f ? "Saved to /spitest.txt. Press any button." : "Could not save. Press any button.", RGB(255, 255, 255)); gfx_present_all();
    while (wup_buttons()) wup_sleep(16);
    while (!wup_buttons()) wup_sleep(16);
    return 0;
}
