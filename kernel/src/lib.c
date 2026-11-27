/* The few C library pieces the kernel needs (no libc). */
#include "os.h"

void *memset(void *d, int c, size_t n) {
    u8 *p = d;
    u32 w = (u8)c * 0x01010101u;
    while (n && ((u32)p & 3)) { *p++ = (u8)c; n--; }
    while (n >= 16) { ((u32 *)p)[0] = w; ((u32 *)p)[1] = w; ((u32 *)p)[2] = w; ((u32 *)p)[3] = w; p += 16; n -= 16; }
    while (n >= 4) { *(u32 *)p = w; p += 4; n -= 4; }
    while (n--) *p++ = (u8)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    u8 *a = d; const u8 *b = s;
    if (!(((u32)a | (u32)b) & 3)) { while (n >= 4) { *(u32 *)a = *(const u32 *)b; a += 4; b += 4; n -= 4; } }
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    u8 *a = d; const u8 *b = s;
    if (a <= b || a >= b + n) return memcpy(d, s, n);
    while (n--) a[n] = b[n];
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
/* CRC-32 (zlib's crc32(crc, p, n): crc 0 to start), a byte at a time through a table made at its first use: a bit
   at a time, checking the menu's 560 KB took a third of a second at every return to it */
u32 crc32_more(u32 crc, const void *p, u32 n) {
    static u32 table[256];
    const u8 *b = p;
    u32 c = ~crc;
    if (!table[1])
        for (u32 i = 0; i < 256; i++) { u32 v = i; for (int k = 0; k < 8; k++) v = v & 1 ? (v >> 1) ^ 0xedb88320u : v >> 1; table[i] = v; }
    while (n >= 4) {
        c = table[(c ^ b[0]) & 0xff] ^ (c >> 8); c = table[(c ^ b[1]) & 0xff] ^ (c >> 8);
        c = table[(c ^ b[2]) & 0xff] ^ (c >> 8); c = table[(c ^ b[3]) & 0xff] ^ (c >> 8);
        b += 4; n -= 4;
    }
    while (n--) c = table[(c ^ *b++) & 0xff] ^ (c >> 8);
    return ~c;
}
u32 crc32(const void *p, u32 n) { return crc32_more(0, p, n); }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) continue; return r; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (u8)*a - (u8)*b; }
int strncmp(const char *a, const char *b, size_t n) { for (; n && *a && *a == *b; n--) { a++; b++; } return n ? (u8)*a - (u8)*b : 0; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }

/* division: the ARM926 has none (the EABI helpers the compiler calls) */
static u32 udiv(u32 n, u32 d, u32 *r) {
    if (!d) { *r = n; return 0xffffffffu; }
    u32 q = 0, bit = 1;
    while (d < n && !(d & 0x80000000u)) { d <<= 1; bit <<= 1; }
    while (bit) { if (n >= d) { n -= d; q |= bit; } d >>= 1; bit >>= 1; }
    *r = n; return q;
}
u32 __aeabi_uidiv(u32 n, u32 d) { u32 r; return udiv(n, d, &r); }
u64 __aeabi_uidivmod(u32 n, u32 d) { u32 r, q = udiv(n, d, &r); return q | (u64)r << 32; }
s32 __aeabi_idiv(s32 n, s32 d) {
    u32 r, q = udiv(n < 0 ? -(u32)n : (u32)n, d < 0 ? -(u32)d : (u32)d, &r);
    return (n < 0) != (d < 0) ? -(s32)q : (s32)q;
}
u64 __aeabi_idivmod(s32 n, s32 d) {
    u32 r, q = udiv(n < 0 ? -(u32)n : (u32)n, d < 0 ? -(u32)d : (u32)d, &r);
    s32 sq = (n < 0) != (d < 0) ? -(s32)q : (s32)q, sr = n < 0 ? -(s32)r : (s32)r;
    return (u32)sq | (u64)(u32)sr << 32;
}

/* printf: %d %u %x %X %s %c %p, width, '0' and '-' flags */
int vsnprintf(char *b, size_t n, const char *f, va_list ap) {
    size_t o = 0;
#define PUT(c) do { if (o + 1 < n) b[o] = (c); o++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        int zero = 0, left = 0, w = 0;
        for (;; f++) { if (*f == '0') zero = 1; else if (*f == '-') left = 1; else break; }
        while (*f >= '0' && *f <= '9') w = w * 10 + (*f++ - '0');
        int prec = -1;                                   /* (.N: at most N characters of a string) */
        if (*f == '.') { f++; prec = 0; while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
        char tmp[12]; const char *s = tmp; int len = 0, neg = 0;
        switch (*f) {
        case 'd': { s32 v = __builtin_va_arg(ap, s32); u32 u = v < 0 ? -(u32)v : (u32)v; neg = v < 0;
                    do { tmp[11 - len++] = (char)('0' + u % 10); u /= 10; } while (u); s = tmp + 12 - len; break; }
        case 'u': { u32 u = __builtin_va_arg(ap, u32); do { tmp[11 - len++] = (char)('0' + u % 10); u /= 10; } while (u); s = tmp + 12 - len; break; }
        case 'p': case 'x': case 'X': { u32 u = __builtin_va_arg(ap, u32); const char *hx = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
                    if (*f == 'p') { zero = 1; w = 8; }
                    do { tmp[11 - len++] = hx[u & 15]; u >>= 4; } while (u); s = tmp + 12 - len; break; }
        case 's': s = __builtin_va_arg(ap, const char *); if (!s) s = "(null)"; len = (int)strlen(s); if (prec >= 0 && len > prec) len = prec; break;
        case 'c': tmp[0] = (char)__builtin_va_arg(ap, int); len = 1; break;
        case '%': tmp[0] = '%'; len = 1; break;
        default: tmp[0] = '?'; len = 1; break;
        }
        int pad = w - len - neg;
        if (!left && !zero) while (pad-- > 0) PUT(' ');
        if (neg) PUT('-');
        if (!left && zero) while (pad-- > 0) PUT('0');
        for (int i = 0; i < len; i++) PUT(s[i]);
        if (left) while (pad-- > 0) PUT(' ');
        if (!*f) break;
    }
    if (n) b[o < n ? o : n - 1] = 0;
#undef PUT
    return (int)o;
}
int snprintf(char *b, size_t n, const char *f, ...) {
    va_list ap; __builtin_va_start(ap, f);
    int r = vsnprintf(b, n, f, ap);
    __builtin_va_end(ap);
    return r;
}
/* (what the compiler calls for struct copies) */
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
