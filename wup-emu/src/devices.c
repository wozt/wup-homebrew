#include "dev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ generic stub */
int stub_quiet = -1;
static bool seen_first(uint32_t key) {
    static uint32_t tab[1 << 16];
    if (stub_quiet < 0) stub_quiet = getenv("DRC_DEBUG") == NULL;
    if (stub_quiet) return false;
    uint32_t h = (key * 2654435761u) >> 16;
    for (;;) {
        if (tab[h] == key) return false;
        if (tab[h] == 0) { tab[h] = key; return true; }
        h = (h + 1) & 0xffff;
    }
}
static uint32_t stub_read(void *ctx, uint32_t off, int size) {
    Stub *s = ctx;
    uint32_t v = s->regs[off >> 2];
    if (size == 1) v = (v >> ((off & 3) * 8)) & 0xff;
    else if (size == 2) v = (v >> ((off & 2) * 8)) & 0xffff;
    if (seen_first((s->base + off) | 1)) { fprintf(stderr, "[%s] read%d +%03x -> %08x pc=%08x\n", s->name, size * 8, off, v, cpu.cur_pc); }
    return v;
}
static void stub_write(void *ctx, uint32_t off, uint32_t val, int size) {
    Stub *s = ctx;
    uint32_t *r = &s->regs[off >> 2];
    if (size == 4) *r = val;
    else if (size == 2) { int sh = (off & 2) * 8; *r = (*r & ~(0xffffu << sh)) | (val << sh); }
    else { int sh = (off & 3) * 8; *r = (*r & ~(0xffu << sh)) | (val << sh); }
    if (seen_first((s->base + off) | 2)) { fprintf(stderr, "[%s] write%d +%03x <- %08x pc=%08x\n", s->name, size * 8, off, val, cpu.cur_pc); }
}
void stub_register(Stub *s, const char *name, uint32_t base, uint32_t size) {
    s->name = name; s->base = base; s->size = size; s->regs = calloc(size / 4, 4);
    bus_register(base, size, name, s, stub_read, stub_write);
}

void irq_raise(int n);
void irq_lower(int n);

/* ------------------------------------------------------------ SPI NOR flash */
typedef struct {
    uint8_t cmd[8]; int ncmd;
    uint32_t addr; bool reading; bool four_byte; uint8_t ear;
    uint8_t status; bool wel;
    int rdid_pos;
} SpiFlash;
static SpiFlash sf;

static uint32_t sf_rd_start, sf_rd_n;
static void sf_cs_release(void) {
    if (sf.reading && sf_rd_n) {
        static int fl = -1; if (fl < 0) fl = getenv("FLASH_LOG") != NULL;
        extern CPU cpu;
        if (fl) {
            fprintf(stderr, "[flash] read %08x +%x (pc %08x lr %08x", sf_rd_start, sf_rd_n, cpu.cur_pc, cpu.r[14]);
            if (getenv("FLASH_LOG_STACK")) {
                extern uint32_t cpu_peek32(CPU *, uint32_t);
                fprintf(stderr, "; stack");
                for (int i = 0; i < 40; i++) { uint32_t v = cpu_peek32(&cpu, cpu.r[13] + i * 4); if ((v & 1) && v > 0x10000 && v < 0xc0000) fprintf(stderr, " %08x", v); }
            }
            fprintf(stderr, ")\n");
        }
    }
    sf_rd_n = 0;
    if (sf.ncmd > 0) {
        uint8_t c = sf.cmd[0];
        if (c == 0x06) sf.wel = true;
        else if (c == 0x04) sf.wel = false;
        else if (c == 0xb7) sf.four_byte = true;
        else if (c == 0xe9) sf.four_byte = false;
    }
    sf.ncmd = 0; sf.reading = false; sf.rdid_pos = 0;
}

static int sf_addr_bytes(uint8_t c) {
    if (c == 0x13 || c == 0x0c || c == 0x12 || c == 0x21 || c == 0xdc) return 4;
    return sf.four_byte ? 4 : 3;
}

static void sf_write_byte(uint8_t b) {
    if (sf.reading) {
        /* page program data */
        if (sf.cmd[0] == 0x02 || sf.cmd[0] == 0x12) {
            uint32_t a = sf.addr % flash_size;
            flash[a] &= b;
            sf.addr = (sf.addr & ~0xffu) | ((sf.addr + 1) & 0xff);
        }
        return;
    }
    if (sf.ncmd < 8) sf.cmd[sf.ncmd++] = b;
    uint8_t c = sf.cmd[0];
    int ab = sf_addr_bytes(c);
    switch (c) {
    case 0x03: case 0x13: case 0x02: case 0x12:
        if (sf.ncmd == 1 + ab) {
            sf.addr = 0;
            for (int i = 1; i <= ab; i++) sf.addr = (sf.addr << 8) | sf.cmd[i];
            if (ab == 3) sf.addr |= (uint32_t)sf.ear << 24;
            sf.reading = true;
            if (getenv("FLASH_LOG")) fprintf(stderr, "[flash] cmd %02x addr %08x (pc %08x)\n", c, sf.addr, cpu.cur_pc);
        }
        break;
    case 0x0b: case 0x0c:
        if (sf.ncmd == 2 + ab) {
            sf.addr = 0;
            for (int i = 1; i <= ab; i++) sf.addr = (sf.addr << 8) | sf.cmd[i];
            if (ab == 3) sf.addr |= (uint32_t)sf.ear << 24;
            sf.reading = true;
            if (getenv("FLASH_LOG")) fprintf(stderr, "[flash] cmd %02x addr %08x (pc %08x)\n", c, sf.addr, cpu.cur_pc);
        }
        break;
    case 0x20: case 0x21: case 0xd8: case 0xdc: case 0x52:
        if (sf.ncmd == 1 + ab) {
            uint32_t a = 0;
            for (int i = 1; i <= ab; i++) a = (a << 8) | sf.cmd[i];
            if (ab == 3) a |= (uint32_t)sf.ear << 24;
            uint32_t sz = (c == 0x20 || c == 0x21) ? 0x1000 : (c == 0x52 ? 0x8000 : 0x10000);
            a &= ~(sz - 1);
            if (a < flash_size) memset(flash + a, 0xff, sz);
            fprintf(stderr, "[flash] erase %08x size %x\n", a, sz);
        }
        break;
    case 0xc5:
        if (sf.ncmd == 2) sf.ear = sf.cmd[1];
        break;
    }
}

static uint8_t sf_read_byte(void) {
    uint8_t c = sf.ncmd ? sf.cmd[0] : 0;
    if (sf.reading && c != 0x02 && c != 0x12) {
        if (!sf_rd_n++) sf_rd_start = sf.addr;
        uint8_t v = flash[sf.addr % flash_size];
        sf.addr++;
        return v;
    }
    switch (c) {
    case 0x9f: { static const uint8_t id[] = { 0xc2, 0x20, 0x19 }; return id[sf.rdid_pos++ % 3]; } /* MX25L256 */
    case 0x05: return sf.wel ? 2 : 0;
    case 0xc8: return sf.ear;
    case 0x15: case 0x35: return sf.four_byte ? 0x20 : 0;
    }
    return 0xff;
}

/* ------------------------------------------------------------ SPI controller */
uint8_t uic_xfer(uint8_t out, bool rx);
void uic_cs(bool active);

static Stub spi_stub;
static uint32_t spi_rx_rem;
static bool spi_cs_active;
static uint32_t spi_ists;
static Stub misc_stub;
static bool pin_low(uint32_t addr) {
    uint32_t v = misc_stub.regs[(addr - 0xf0000000) / 4];
    return (v & 0x200) && !(v & 0x100);
}
static int spi_sel(void) {
    if (pin_low(0xf00050fc)) return 2;   /* UIC chip select (GPIO) */
    if (pin_low(0xf0005108)) return 1;   /* flash chip select (GPIO) */
    return spi_stub.regs[0x24 / 4] & 0xff;
}

static void spi_irq_update(void) {
    uint32_t ie = spi_stub.regs[0x18 / 4] & 0xc0;
    if (spi_ists & ie) irq_raise(6);
}
static void spi_set_cs(bool active) {
    if (active == spi_cs_active) return;
    spi_cs_active = active;
    int d = spi_sel();
    if (d == 1) { if (!active) sf_cs_release(); }
    else if (d == 2) uic_cs(active);
}
static void spi_tx(uint8_t b) {
    int d = spi_sel();
    if (d == 1) sf_write_byte(b);
    else if (d == 2) uic_xfer(b, false);
    else fprintf(stderr, "[spi] tx %02x to unknown device %d\n", b, d);
}
uint8_t spi_rx_byte(void) {
    int d = spi_sel();
    if (d == 1) return sf_read_byte();
    if (d == 2) return uic_xfer(0xff, true);
    return 0xff;
}
static uint32_t spi_read(void *ctx, uint32_t off, int size) {
    switch (off) {
    case 0x08: return spi_ists;
    case 0x0c: return 0x10 | ((spi_rx_rem > 16 ? 16 : spi_rx_rem) << 8);
    case 0x10: {
        uint8_t v = spi_rx_byte();
        if (spi_rx_rem) spi_rx_rem--;
        return v;
    }
    }
    return stub_read(ctx, off, size);
}
static void spi_write(void *ctx, uint32_t off, uint32_t val, int size) {
    if (off == 0x08) { spi_ists &= ~val; return; }
    stub_write(ctx, off, val, size);
    switch (off) {
    case 0x04:
        if (val & 0x40) spi_rx_rem = 0;
        spi_set_cs(!(val & 0x200));
        break;
    case 0x10:
        spi_tx(val);
        spi_ists |= 0x80;
        spi_irq_update();
        break;
    case 0x18:
        spi_irq_update();
        break;
    case 0x20:
        if ((spi_stub.regs[1] & 2) && val) { spi_rx_rem = val; spi_ists |= 0x40; spi_irq_update(); }
        break;
    }
}

/* ------------------------------------------------------------ DMA (SPI) */
static Stub dma_stub;
static uint32_t dma_read(void *ctx, uint32_t off, int size) {
    Stub *s = ctx;
    if (off >= 0x40 && off < 0x1c0 && (off & 0x1f) == 0) return s->regs[off / 4] & ~1u;
    return stub_read(ctx, off, size);
}
static int dma2d_log = -1;
extern uint64_t emu_total_cycles;
static void dma2d_start(Stub *s, int k) {
    /* +04 mode (bit 0x400: fill with constant +20), +08 line width, +0C src stride, +10 dst stride,
       +14 total bytes - 1, +18 src, +1C dst, +20 fill value */
    uint32_t *r = &s->regs[(0x100 + 0x40 * k) / 4];
    uint32_t mode = r[1], w = r[2], sstr = r[3], dstr = r[4], total = r[5] + 1, src = r[6], dst = r[7];
    if (dma2d_log < 0) dma2d_log = getenv("DRC_DEBUG") ? 8 : (getenv("DMA2D_LOG") ? 100000 : 0);
    if (dma2d_log > 0) { dma2d_log--; fprintf(stderr, "[dma2d] t=%.3f ch%d mode %x w %u count %u src %08x/%u dst %08x/%u opt %x %x\n", emu_total_cycles / 108e6, k + 2, mode, w, total, src, sstr, dst, dstr, r[8], r[9]); }
    if (total > 0x400000) total = 0x400000;
    if (!w || w >= total) { w = total; sstr = dstr = total; }   /* linear */
    uint32_t lines = total / w;
    static uint8_t tmp[0x10000];
    if (w > sizeof tmp) w = sizeof tmp;
    for (uint32_t y = 0; y < lines; y++) {
        if (mode & 0x400) memset(tmp, r[8] & 0xff, w);
        else bus_dma_read(src + y * sstr, tmp, w);
        bus_dma_write(dst + y * dstr, tmp, w);
    }
    r[5] = 0xffffffff;   /* row counter ran out */
    static const int irqmap[3] = { 13, 14, 12 };
    irq_raise(irqmap[k]);
}
static void dma_write(void *ctx, uint32_t off, uint32_t val, int size) {
    Stub *s = ctx;
    stub_write(ctx, off, val, size);
    if (off >= 0x100 && off < 0x1c0 && (off & 0x3f) == 0 && (val & 1)) { s->regs[off / 4] &= ~1u; dma2d_start(s, (off - 0x100) >> 6); return; }
    if (off >= 0x40 && off < 0xc0 && (off & 0x1f) == 0 && (val & 1)) {
        int ch = (off - 0x40) >> 5;
        uint32_t *r = &s->regs[off / 4];
        uint32_t mode = r[1], cnt = r[4] + 1, addr = r[5];
        if (mode == 4) {
            for (uint32_t i = 0; i < cnt; i++) {
                uint8_t b = spi_rx_byte();
                bus_dma_write(addr + i, &b, 1);
            }
            spi_rx_rem = 0;
            spi_ists |= 0x40;
            spi_irq_update();
        } else if ((mode & 1) && ((mode >> 1) & 7) >= 4 && ((mode >> 1) & 7) <= 6) {   /* memory -> UART0..2 */
            extern void uart_tx_byte(int, uint8_t);
            for (uint32_t i = 0; i < cnt; i++) { uint8_t b; bus_dma_read(addr + i, &b, 1); uart_tx_byte(((mode >> 1) & 7) - 4, b); }
        } else {
            if (getenv("DRC_DEBUG")) fprintf(stderr, "[dma] ch%d mode %x cnt %x addr %08x (tx)\n", ch, mode, cnt, addr);
            for (uint32_t i = 0; i < cnt; i++) {
                uint8_t b; bus_dma_read(addr + i, &b, 1);
                spi_tx(b);
            }
            spi_ists |= 0x80;
            spi_irq_update();
        }
        r[4] = 0xffffffff;   /* counter ran out */
        irq_raise(8 + ch);
    }
}

/* ------------------------------------------------------------ system controller */
static Stub sys_stub;
/* boot straps (0xF0000000 bits 16-19), as the boot ROM reads them: bit 18 set = boot from SPI flash
   (ROM loader 0x224), bit 18 clear = UART loader (0x3c4, UART1: 4-byte LE length, then that many
   bytes to RAM 0, then soft reset into it). Grounding the GamePad's "UART boot" pad clears bit 18.
   --uart-boot selects the UART loader. */
uint32_t boot_straps = 0x00040000u;
static uint32_t sys_read(void *ctx, uint32_t off, int size) {
    if (off == 0x00) return (stub_read(ctx, off, size) & ~0x000f0000u) | boot_straps;
    return stub_read(ctx, off, size);
}
static void sys_write(void *ctx, uint32_t off, uint32_t val, int size) {
    stub_write(ctx, off, val, size);
    if (off == 0x04 && (val & 1)) {
        if (getenv("DRC_DEBUG")) fprintf(stderr, "[sys] soft reset -> RAM boot\n");
        bus_map_rom(false);
        cpu.reset_req = true;
    }
}

/* ------------------------------------------------------------ interrupt controller */
#define NIRQ 64
static Stub ic_stub;
static uint64_t irq_pending;
int ic_debug = 0;

static bool irq_enabled(int n) {
    if (n >= 40) return true;
    uint8_t cfg = (ic_stub.regs[(0x008 + 4 * n) / 4]) & 0xff;
    return (cfg & 0x40) == 0;
}
static int irq_prio(int n) {
    if (n >= 40) return 15;
    return ic_stub.regs[(0x008 + 4 * n) / 4] & 0x0f;
}
static int irq_best(void) {
    int best = -1, bp = 99;
    for (int n = 0; n < NIRQ; n++) {
        if (!(irq_pending & (1ull << n)) || !irq_enabled(n)) continue;
        int p = irq_prio(n);
        if (p < bp) { bp = p; best = n; }
    }
    return best;
}
static void ic_update(void) {
    cpu.irq_line = irq_best() >= 0;
}
void irq_raise(int n) {
    if (!(irq_pending & (1ull << n)) && ic_debug) fprintf(stderr, "[ic] raise %d\n", n);
    irq_pending |= 1ull << n; ic_update();
}
void irq_lower(int n) { irq_pending &= ~(1ull << n); ic_update(); }

static uint32_t ic_read(void *ctx, uint32_t off, int size) {
    if (off == 0x1f0) {
        int n = irq_best();
        if (ic_debug) fprintf(stderr, "[ic] ack -> %d pc=%08x\n", n, cpu.cur_pc);
        if (n < 0) return 0xff;
        irq_pending &= ~(1ull << n);
        ic_update();
        return n;
    }
    return stub_read(ctx, off, size);
}
static void ic_write(void *ctx, uint32_t off, uint32_t val, int size) {
    if (off == 0x000) { irq_pending &= ~(uint64_t)val; ic_update(); return; }
    if (off == 0x004) { irq_pending &= ~((uint64_t)val << 32); ic_update(); return; }
    stub_write(ctx, off, val, size);
    ic_update();
}

/* ------------------------------------------------------------ timers */
#define NTMR 8
static Stub tmr_stub;
typedef struct { uint32_t ctrl, count, cmp; double frac; } Timer;
static Timer tmr[NTMR];
#define CPU_HZ 108000000.0
static double us_frac; static uint32_t us_count;
uint32_t timer_us_count(void) { return us_count; }

static uint32_t tmr_read(void *ctx, uint32_t off, int size) {
    if (off == 0x08) return us_count;
    if (off >= 0x10 && off < 0x10 + NTMR * 0x10) {
        Timer *t = &tmr[(off - 0x10) >> 4];
        switch (off & 0xf) {
        case 0x0: return t->ctrl;
        case 0x4: return t->count;
        case 0x8: return t->cmp;
        }
    }
    return stub_read(ctx, off, size);
}
static void tmr_write(void *ctx, uint32_t off, uint32_t val, int size) {
    stub_write(ctx, off, val, size);
    if (off == 0x08) { us_count = val; return; }
    if (off >= 0x10 && off < 0x10 + NTMR * 0x10) {
        Timer *t = &tmr[(off - 0x10) >> 4];
        switch (off & 0xf) {
        case 0x0: if (!(val & 3)) t->count = 0; t->ctrl = val; break;
        case 0x4: t->count = val; break;
        case 0x8: t->cmp = val; break;
        }
    }
}
static void tmr_tick(int cycles) {
    double base = CPU_HZ / ((tmr_stub.regs[0] & 0xffff) + 1);
    /* the free-running "1 MHz" counter (0x408) really ticks at 9973/10000 MHz: the firmware's TSF sync
       (LVC 0x1cd4e, called for every Broadcom TSF event) adds elapsed*27/9973 to it each time before
       comparing it with the console's clock. At exactly 1 MHz the pad's clock runs 0.27% fast and
       saw-tooths up to 8 ms (the hard-resync threshold) ahead of the console. */
    us_frac += cycles / (double)((tmr_stub.regs[1] & 0xffff) + 1) * (9973.0 / 10000.0);
    uint32_t un = (uint32_t)us_frac; us_frac -= un; us_count += un;
    for (int i = 0; i < NTMR; i++) {
        Timer *t = &tmr[i];
        if (!(t->ctrl & 3)) continue;
        int p = (t->ctrl >> 4) & 7;
        t->frac += cycles * (base / (2 << p)) / CPU_HZ;
        uint32_t n = (uint32_t)t->frac;
        t->frac -= n;
        while (n) {
            uint32_t left = t->cmp - t->count + 1;   /* counts until wrap past cmp */
            if (t->cmp == 0xffffffff && t->count == 0) left = 0xffffffff;
            if (n < left) { t->count += n; break; }
            n -= left;
            t->count = 0;
            irq_raise(i);
        }
    }
}

void devices_init(void) {
    stub_register(&misc_stub, "mmio", 0xf0000000, 0x100000);
    extern void lcd_init(void); lcd_init();
    extern void i2c_init(void); i2c_init();
    extern void uic_init(void); uic_init();
    extern void wifi_init(void); wifi_init();
    extern void audio_init(void); audio_init();
    extern void uart_init(void); uart_init();
    stub_register(&sys_stub, "sys", 0xf0000000, 0x400);
    sys_stub.regs[0] = 0;
    bus_register(0xf0000000, 0x400, "sys", &sys_stub, sys_read, sys_write);
    stub_register(&ic_stub, "intc", 0xf0001200, 0x400);
    bus_register(0xf0001200, 0x400, "intc", &ic_stub, ic_read, ic_write);
    stub_register(&tmr_stub, "timer", 0xf0000400, 0x400);
    bus_register(0xf0000400, 0x400, "timer", &tmr_stub, tmr_read, tmr_write);
    stub_register(&dma_stub, "spidma", 0xf0004000, 0x400);
    bus_register(0xf0004000, 0x400, "spidma", &dma_stub, dma_read, dma_write);
    stub_register(&spi_stub, "spi", 0xf0004400, 0x400);
    bus_register(0xf0004400, 0x400, "spi", &spi_stub, spi_read, spi_write);
}

uint64_t emu_total_cycles;
void devices_tick(int cycles) {
    emu_total_cycles += cycles;
    tmr_tick(cycles);
    extern void wifi_tick(void); wifi_tick();
    extern void audio_tick(int); audio_tick(cycles);
    extern void lcd_tick(int); lcd_tick(cycles);
    extern void uart_tick(void); uart_tick();
}
