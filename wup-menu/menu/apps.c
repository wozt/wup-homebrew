/* apps: the installed apps on the filesystem (libwup/src/fs.c), format libwup/include/wupfmt.h. Same rules as the Switch's
 * nx-hbmenu for sdmc:/switch:
 *   /apps/<x>.wup                  a single-file app
 *   /apps/<dir>/<dir>.wup          an app with its own folder (its data lives next to it), or, if that
 *   /apps/<dir>/<one>.wup          name does not exist, the only .wup in the folder
 * The app is told its path (the hand-off block app_path, like argv[0]) and finds its folder from it. */
#include <stdio.h>
#include <string.h>
#include "fs.h"
#include "wupfmt.h"
#include "apps.h"
#include "armcpu.h"
typedef uint32_t u32; typedef uint8_t u8;

app_entry_t apps_list[APPS_MAX];
int apps_count, apps_gen;                 /* apps_gen bumps on every scan: caches keyed on it stay honest */

static int ends_wup(const char *n) { int l = (int)strlen(n); return l > 4 && !strcmp(n + l - 4, ".wup"); }

static void add(const char *path, u32 size) {
    if (apps_count >= APPS_MAX || strlen(path) >= sizeof apps_list[0].path) return;
    app_entry_t *a = &apps_list[apps_count];
    strcpy(a->path, path);
    wup_hdr_t h;
    if (fs_read(a->path, 0, &h, sizeof h) != (int)sizeof h || h.magic != WUP_MAGIC) return;
    if (!h.code_size || h.code_size > APP_CODE_MAX || h.code_off + h.code_size > size) return;
    memcpy(a->name, h.name, 32); a->name[31] = 0;
    memcpy(a->author, h.author, 32); a->author[31] = 0;
    memcpy(a->version, h.version, 16); a->version[15] = 0;
    if (h.hdr_size >= WUP_HDR_V2) { memcpy(a->description, h.description, 96); a->description[95] = 0; } else a->description[0] = 0;
    if (!a->name[0]) { const char *n = path; for (const char *q = path; *q; q++) if (*q == '/') n = q + 1; strncpy(a->name, n, 31); a->name[31] = 0; }
    a->code_off = h.code_off; a->code_size = h.code_size; a->code_crc = h.code_crc;
    a->icon_off = h.icon_w == WUP_ICON && h.icon_h == WUP_ICON && h.icon_off + WUP_ICON * WUP_ICON * 2 <= size ? h.icon_off : 0;
    apps_count++;
}
/* a folder: <dir>/<dir>.wup, else its only .wup */
static void add_folder(const char *dir) {
    static char p[96]; struct lfs_info info;
    if (strlen(dir) > 40) return;
    strcpy(p, "/apps/"); strcat_(p, dir); strcat_(p, "/"); strcat_(p, dir); strcat_(p, ".wup");
    if (!lfs_stat(&fs_lfs, p, &info) && info.type == LFS_TYPE_REG) { add(p, info.size); return; }
    strcpy(p, "/apps/"); strcat_(p, dir);
    lfs_dir_t d; static char one[64]; int n = 0; u32 sz = 0;
    if (lfs_dir_open(&fs_lfs, &d, p)) return;
    while (lfs_dir_read(&fs_lfs, &d, &info) > 0)
        if (info.type == LFS_TYPE_REG && ends_wup(info.name) && strlen(info.name) < sizeof one) { if (++n > 1) break; strcpy(one, info.name); sz = info.size; }
    lfs_dir_close(&fs_lfs, &d);
    if (n != 1) return;
    strcat_(p, "/"); strcat_(p, one);
    add(p, sz);
}
int apps_scan(void) {
    apps_count = 0; apps_gen++;
    if (!fs_ok()) return 0;
    lfs_dir_t d; struct lfs_info info;
    if (lfs_dir_open(&fs_lfs, &d, "/apps")) return 0;
    static char p[96];
    while (apps_count < APPS_MAX && lfs_dir_read(&fs_lfs, &d, &info) > 0) {
        if (info.name[0] == '.') continue;
        if (info.type == LFS_TYPE_DIR) { add_folder(info.name); continue; }
        if (!ends_wup(info.name) || strlen(info.name) > 80) continue;
        strcpy(p, "/apps/"); strcat_(p, info.name);
        add(p, info.size);
    }
    lfs_dir_close(&fs_lfs, &d);
    return apps_count;
}
int apps_load_icon(int i, void *dst) {
    const app_entry_t *a = &apps_list[i];
    if (!a->icon_off) return 0;
    return fs_read(a->path, a->icon_off, dst, WUP_ICON * WUP_ICON * 2) == WUP_ICON * WUP_ICON * 2;
}
int apps_load_icon_rows(int i, int y0, void *dst, int nrows) {   /* nrows of the 64x64 icon, from row y0 */
    const app_entry_t *a = &apps_list[i];
    if (!a->icon_off || y0 < 0 || y0 + nrows > WUP_ICON) return 0;
    return fs_read(a->path, a->icon_off + (uint32_t)y0 * WUP_ICON * 2, dst, (uint32_t)nrows * WUP_ICON * 2) == nrows * WUP_ICON * 2;
}
int apps_remove(int i) { return fs_remove(apps_list[i].path); }

/* ---- an app's footprint on the littlefs, and removing it whole ----
 * An app is /apps/<x>.wup or /apps/<dir>/<file>.wup; in the second form the folder holds its data too. */
static void app_folder(const char *path, char *out) {   /* "/apps/<dir>/<file>.wup" -> "/apps/<dir>"; else "" */
    out[0] = 0;
    const char *s = path;
    if (strncmp(s, "/apps/", 6)) return;
    s += 6;
    const char *slash = strchr(s, '/');
    if (!slash || slash == s) return;
    size_t n = (size_t)(slash - s);
    if (n > 80) return;
    memcpy(out, "/apps/", 6); memcpy(out + 6, s, n); out[6 + n] = 0;
}
static uint64_t dir_bytes(const char *dir, int depth) {  /* every regular file under dir, the .wup included */
    if (depth >= 8) return 0;
    lfs_dir_t d; struct lfs_info inf; uint64_t n = 0;
    if (lfs_dir_open(&fs_lfs, &d, dir)) return 0;
    while (lfs_dir_read(&fs_lfs, &d, &inf) > 0) {
        if (!strcmp(inf.name, ".") || !strcmp(inf.name, "..")) continue;
        char p[192]; snprintf(p, sizeof p, "%s/%s", dir, inf.name);
        if (inf.type == LFS_TYPE_DIR) n += dir_bytes(p, depth + 1); else n += inf.size;
    }
    lfs_dir_close(&fs_lfs, &d);
    return n;
}
uint64_t apps_footprint(int i) {
    if (i < 0 || i >= apps_count) return 0;
    char d[96]; app_folder(apps_list[i].path, d);
    if (d[0]) return dir_bytes(d, 0);
    int s = fs_filesize(apps_list[i].path);
    return s > 0 ? (uint64_t)s : 0;
}
/* everything inside dir, then dir itself; one entry at a time (the listing changes as it goes) */
static int wipe_dir(const char *dir, int depth) {
    if (depth >= 8) return -1;
    static char path[8][192]; int r = 0;
    for (;;) {
        lfs_dir_t d; struct lfs_info inf; int found = 0;
        if (lfs_dir_open(&fs_lfs, &d, dir)) return -1;
        while (lfs_dir_read(&fs_lfs, &d, &inf) > 0) {
            if (!strcmp(inf.name, ".") || !strcmp(inf.name, "..")) continue;
            snprintf(path[depth], sizeof path[0], "%s/%s", dir, inf.name); found = 1; break;
        }
        lfs_dir_close(&fs_lfs, &d);
        if (!found) break;
        if (inf.type == LFS_TYPE_DIR) wipe_dir(path[depth], depth + 1);
        if (lfs_remove(&fs_lfs, path[depth])) r = -1;
    }
    return r;
}
int apps_remove_all(int i) {                        /* the .wup and its folder's data; the system apps stay */
    if (i < 0 || i >= apps_count) return -1;
    app_entry_t *a = &apps_list[i];
    if (!strncmp(a->path, "/apps/app_store/", 18) || !strncmp(a->path, "/apps/software_update/", 24)) return -1;
    char d[96]; app_folder(a->path, d);
    int r = fs_remove(a->path);
    if (d[0]) { if (wipe_dir(d, 0)) r = -1; if (lfs_remove(&fs_lfs, d)) r = -1; }
    return r;
}
