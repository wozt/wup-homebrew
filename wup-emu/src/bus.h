#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define RAM_BASE 0x00000000u
#define RAM_SIZE 0x02000000u   /* SRAM: SPL sections reach past 16 MiB; try 32 MiB window */

typedef uint32_t (*mmio_read_fn)(void *ctx, uint32_t off, int size);
typedef void     (*mmio_write_fn)(void *ctx, uint32_t off, uint32_t val, int size);

extern uint8_t *page_r[1 << 20];
extern uint8_t *page_w[1 << 20];
extern uint8_t *ram;
extern uint8_t *bootrom;
extern int log_mmio;

void bus_init(void);
void bus_map_rom(bool mapped);
void bus_register(uint32_t base, uint32_t size, const char *name, void *ctx, mmio_read_fn r, mmio_write_fn w);

uint32_t bus_mmio_read(uint32_t pa, int size);
void     bus_mmio_write(uint32_t pa, uint32_t val, int size);

static inline uint32_t bus_read32(uint32_t pa) {
    uint8_t *p = page_r[pa >> 12];
    if (p) return *(uint32_t *)(p + (pa & 0xffc));
    return bus_mmio_read(pa & ~3u, 4);
}
static inline uint32_t bus_read16(uint32_t pa) {
    uint8_t *p = page_r[pa >> 12];
    if (p) return *(uint16_t *)(p + (pa & 0xffe));
    return bus_mmio_read(pa & ~1u, 2);
}
static inline uint32_t bus_read8(uint32_t pa) {
    uint8_t *p = page_r[pa >> 12];
    if (p) return p[pa & 0xfff];
    return bus_mmio_read(pa, 1);
}
static inline void bus_write32(uint32_t pa, uint32_t v) {
    uint8_t *p = page_w[pa >> 12];
    if (p) { *(uint32_t *)(p + (pa & 0xffc)) = v; return; }
    bus_mmio_write(pa & ~3u, v, 4);
}
static inline void bus_write16(uint32_t pa, uint32_t v) {
    uint8_t *p = page_w[pa >> 12];
    if (p) { *(uint16_t *)(p + (pa & 0xffe)) = (uint16_t)v; return; }
    bus_mmio_write(pa & ~1u, v & 0xffff, 2);
}
static inline void bus_write8(uint32_t pa, uint32_t v) {
    uint8_t *p = page_w[pa >> 12];
    if (p) { p[pa & 0xfff] = (uint8_t)v; return; }
    bus_mmio_write(pa, v & 0xff, 1);
}

/* physical-address helpers for DMA engines */
static inline uint8_t *bus_ptr(uint32_t pa) {
    uint8_t *p = page_w[pa >> 12];
    return p ? p + (pa & 0xfff) : NULL;
}
void bus_dma_write(uint32_t pa, const uint8_t *src, uint32_t len);
void bus_dma_read(uint32_t pa, uint8_t *dst, uint32_t len);
