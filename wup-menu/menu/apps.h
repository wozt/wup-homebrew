#pragma once
#include <stdint.h>
#define APPS_MAX 32
#define APP_CODE_MAX 0xd9000u                 /* what fits the stage (libwup/include/handoff.h) */
typedef struct { char path[96], name[32], author[32], version[16], description[96]; uint32_t code_off, code_size, code_crc, icon_off; } app_entry_t;
extern app_entry_t apps_list[APPS_MAX];
extern int apps_count;
int apps_scan(void);
int apps_load_icon(int i, void *dst);         /* 1 = WUP_ICON x WUP_ICON RGB565 loaded */
int apps_remove(int i);
uint64_t apps_footprint(int i);            /* bytes the app takes on the littlefs: the .wup and its folder's data */
int apps_remove_all(int i);                /* the .wup and its folder's data; the system apps stay */
static inline char *strcat_(char *d, const char *s) { char *o = d; while (*d) d++; while ((*d++ = *s++)) continue; return o; }
static inline char *strncat_(char *d, const char *s, int cap) {   /* append within cap characters */
    int n = 0; while (d[n]) n++;
    while (*s && n < cap) d[n++] = *s++;
    d[n] = 0; return d;
}
