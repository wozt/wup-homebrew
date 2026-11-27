#pragma once
#include <stdint.h>
#include "littlefs/lfs.h"
#ifdef __cplusplus                                   /* (C++ apps: these are C) */
extern "C" {
#endif

extern lfs_t fs_lfs;                 /* for directory walks (file sharing, app list) */
int fs_mount(int *formatted);        /* mounts, formatting the first time; creates /apps and /save */
int fs_erase(void (*tick)(void));    /* everything gone: an empty filesystem, unmounted (the menu only; 0 = done) */
int fs_ok(void);
int fs_flash_read(uint32_t addr, void *dst, uint32_t len);   /* raw flash (up to 4096 bytes): 0 = read */
uint32_t fs_size_kb(void), fs_used_kb(void);
int fs_read(const char *path, uint32_t off, void *dst, uint32_t len);   /* bytes read or < 0 */
int fs_write(const char *path, const void *src, uint32_t len);          /* whole file */
int fs_remove(const char *path);
void fs__used_refresh(void);                                            /* (libwup: before writing) */
int fs_stream_open(const char *path);                                   /* write one file in pieces */
int fs_stream_write(const void *src, uint32_t len);                     /* 0 or < 0 */
int fs_stream_close(void);
int fs_filesize(const char *path);

#ifdef __cplusplus
}
#endif
