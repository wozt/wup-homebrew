/* malloc for large apps (app.cfg: large = yes): TLSF (third_party/tlsf, BSD) over every piece of RAM such an app may
 * have, as one heap: its own (from its .bss to WUP_APP_END), and screen 0 (820 KB), which a large app does not need:
 * it shows screen 1 alone (gfx.c: no double buffering). The sound and microphone rings between them stay where the
 * kernel keeps them. TLSF takes several pools and keeps its time constant and its fragmentation low.
 *   These replace the C library's malloc family (picolibc's are not linked then). Threads: one lock, a kernel
 *   semaphore. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "tlsf/tlsf.h"
#include "wup_ram.h"
#include "wupk.h"

extern char __bss_end[];
static tlsf_t heap;
static int lock_sem;
static size_t total;
static void *rest_pool;                                /* the rest of screen 1, once the app is on the 8-bit screen */
static uint32_t rest_lo;

static void init(void) {
    uintptr_t lo = ((uintptr_t)__bss_end + 63) & ~(uintptr_t)63;
    heap = tlsf_create_with_pool((void *)lo, WUP_APP_END - lo);
    tlsf_add_pool(heap, (void *)WUP_FB0, WUP_FB_SIZE);
    total = (WUP_APP_END - lo) + WUP_FB_SIZE;
    lock_sem = WUPK->sem_new(1, 1);
}
static void lock(void) { if (!heap) init(); WUPK->sem_wait(lock_sem, -1); }
static void unlock(void) { WUPK->sem_signal(lock_sem); }

/* a request rounded up to the start of a TLSF size class (1/32 of its power of two: at most 3% more). TLSF looks
   for a block one class above an unrounded request, to be sure of its size at once: a block freed at exactly that
   size (RetroArch's menu frees its frame, then asks for the same again) was then not found. Rounded, the request and
   the block are in the same class. */
static size_t class_size(size_t n) {
    if (n < 256) return n;
    size_t g = (size_t)1 << (31 - __builtin_clz((unsigned)n) - 5);
    return (n + g - 1) & ~(g - 1);
}

void *malloc(size_t n) {
    lock(); void *p = tlsf_malloc(heap, n ? class_size(n) : 1); unlock();
    if (!p) errno = ENOMEM;
    return p;
}
void free(void *p) { if (!p) return; lock(); tlsf_free(heap, p); unlock(); }
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    lock(); void *q = tlsf_realloc(heap, p, class_size(n)); unlock();
    if (!q) errno = ENOMEM;
    return q;
}
void *calloc(size_t a, size_t b) {
    size_t n;
    if (__builtin_mul_overflow(a, b, &n)) { errno = ENOMEM; return NULL; }
    lock(); void *p = tlsf_malloc(heap, n ? class_size(n) : 1); unlock();
    if (!p) errno = ENOMEM;
    if (p) memset(p, 0, n);
    return p;
}
void *memalign(size_t align, size_t n) {
    lock(); void *p = tlsf_memalign(heap, align, n ? class_size(n) : 1); unlock();
    if (!p) errno = ENOMEM;
    return p;
}
void *aligned_alloc(size_t align, size_t n) { return memalign(align, n); }
int posix_memalign(void **out, size_t align, size_t n) {
    void *p = memalign(align, n);
    if (!p) return ENOMEM;
    *out = p; return 0;
}
size_t malloc_usable_size(void *p) { return p ? tlsf_block_size(p) : 0; }

/* what is free (libwup's wup_mem_free, for large apps): every free block of both pools */
static void add_free(void *ptr, size_t size, int used, void *user) { (void)ptr; if (!used) *(size_t *)user += size; }
size_t wup__large_free(void) {
    size_t n = 0;
    lock();
    tlsf_walk_pool(tlsf_get_pool(heap), add_free, &n);
    /* (the second pool: TLSF keeps no list of pools; it starts at screen 0, after the pool header) */
    tlsf_walk_pool((pool_t)WUP_FB0, add_free, &n);
    if (rest_pool) tlsf_walk_pool(rest_pool, add_free, &n);
    unlock();
    return n;
}
size_t wup__large_total(void) { if (!heap) { lock(); unlock(); } return total; }

/* ---- write-back memory for a large app. The kernel maps RAM 2-4 MB write-through (kernel/src/mmu.c): the screens and
   the sound and microphone rings are there, read by the hardware, which then sees what the CPU wrote. A large app's
   heap is there too (the end of the app block, and screen 0), and its stores then all go to RAM: an emulator's state,
   written constantly, ran at a fraction of its speed. While a large app runs, those two 1 MB sections are mapped in
   4 KB pages instead (two coarse page tables): the app's own memory write-back, every page the hardware reads (the
   rings, screen 1) write-through as before. At the app's end (wup.c) the data cache is cleaned to RAM and the
   sections mapped as they were. Apps run in the CPU's supervisor mode: the page table is theirs to change. */
static uint32_t coarse[2][256] __attribute__((aligned(1024)));
static uint32_t saved[2];
static int wb_on;
__attribute__((target("arm"))) static uint32_t irq_off(void) {
    uint32_t c; __asm__ volatile("mrs %0, cpsr\n orr r1, %0, #0xc0\n msr cpsr_c, r1" : "=r"(c) :: "r1", "memory"); return c;
}
__attribute__((target("arm"))) static void irq_restore(uint32_t c) { __asm__ volatile("msr cpsr_c, %0" :: "r"(c) : "memory"); }
__attribute__((target("arm"))) static uint32_t *ttb(void) { uint32_t t; __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(t)); return (uint32_t *)(t & ~0x3fffu); }
__attribute__((target("arm"))) static void clean_all(void) {                          /* (ARM926: test, clean and invalidate the whole data cache) */
    __asm__ volatile("1: mrc p15, 0, APSR_nzcv, c7, c14, 3\n bne 1b\n mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory", "cc");
}
__attribute__((target("arm"))) static void tlb_flush(void) { __asm__ volatile("mcr p15, 0, %0, c8, c7, 0" :: "r"(0) : "memory"); }
static int page_wb(uint32_t a) {                       /* (a page the hardware never reads: write-back) */
    uint32_t e = a + 0x1000;
    return e <= WUP_SOUND_RING || (a >= WUP_MIC_RING + WUP_MIC_RING_SIZE && e <= WUP_FB1) || (rest_lo && a >= rest_lo);
}
void wup__large_wb(int on) {
    if (on == wb_on) return;
    uint32_t c = irq_off(), *t = ttb();
    if (on) {
        for (int s = 0; s < 2; s++) {
            uint32_t mb = 2 + s;
            saved[s] = t[mb];
            for (uint32_t p = 0; p < 256; p++) {
                uint32_t a = mb << 20 | p << 12;
                coarse[s][p] = a | 0xff0u | (page_wb(a) ? 0xcu : 0x8u) | 2u;   /* small page, read/write, C B */
            }
        }
        clean_all();                                   /* (the tables in RAM, where the MMU reads them) */
        for (int s = 0; s < 2; s++) t[2 + s] = (uint32_t)coarse[s] | 1u << 4 | 1u;   /* coarse table, domain 0 */
    } else {
        clean_all();                                   /* what the app wrote, to RAM */
        for (int s = 0; s < 2; s++) t[2 + s] = saved[s];
    }
    clean_all();
    tlb_flush();
    wb_on = on;
    irq_restore(c);
}

/* A large app on the 8-bit screen (wup_screen8: 864 x 480 bytes, at the start of screen 1) does not use the rest of
   screen 1 (404 KB): it joins the heap, write-back like the rest of the app's memory. Call it once wup_screen8() is up;
   from then on the RGB565 screen (wup_screen, gfx_* on it) must not be used. */
void wup_large_take_screen_rest(void) {
    if (rest_pool) return;
    uintptr_t lo = (WUP_FB8 + 864u * 480u + 0xfffu) & ~(uintptr_t)0xfff, hi = WUP_FB1 + WUP_FB_SIZE;
    lock();
    rest_pool = tlsf_add_pool(heap, (void *)lo, hi - lo);
    total += hi - lo;
    unlock();
    if (wb_on) {                                       /* (its pages write-back now: they were write-through) */
        uint32_t c = irq_off();
        for (uint32_t a = lo; a < hi; a += 0x1000) coarse[1][(a >> 12) & 0xff] = a | 0xff0u | 0xcu | 2u;
        rest_lo = (uint32_t)lo;
        clean_all();
        tlb_flush();
        irq_restore(c);
    } else rest_lo = (uint32_t)lo;
}
