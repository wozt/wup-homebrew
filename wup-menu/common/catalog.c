/* catalog: see catalog.h. HTTPS is libwup's (wup_https.h), the JSON is read with jsmn, MD5 is BearSSL's, zip entries
 * are inflated with libwup's wup_inflate (tinf). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "wup.h"
#include "fs.h"
#include "littlefs/lfs.h"
#include "catalog.h"
#include "version.h"
#define JSMN_STATIC
#include "jsmn/jsmn.h"

catalog_pkg catalog[CATALOG_MAX];
int catalog_count;
void (*catalog_idle)(void);
static wup_https_opts opts(void) { wup_https_opts o = { 0 }; o.idle = catalog_idle; return o; }

static const char *const why[] = {
    "Not connected to Wi-Fi", "The time isn't set yet", "The catalog can't be reached", "The connection isn't secure",
    "The catalog can't be read", "The download is damaged", "Not enough storage", "The files could not be written",
    "Not enough memory",
};
const char *catalog_why(int e) { return e >= CATALOG_ENOWIFI && e <= CATALOG_ENOMEM ? why[e - CATALOG_ENOWIFI] : "Something went wrong"; }
static int from_https(int r) {
    return r == WUP_HTTPS_ENOTIME ? CATALOG_ENOTIME : r == WUP_HTTPS_ETLS ? CATALOG_ETLS : r == WUP_HTTPS_ENOMEM ? CATALOG_ENOMEM
         : r >= 0 || r == WUP_HTTPS_EHTTP ? CATALOG_EFORMAT : CATALOG_ENET;
}

int catalog_newer(const char *a, const char *b) { return version_newer(a, b); }

/* ---- downloads: small ones (repo.json) into memory, packages into a file */
typedef struct { uint8_t *p; uint32_t n, max; } buf_t;
static int collect(void *arg, const uint8_t *d, size_t n, long long total) {
    buf_t *b = arg;
    if (b->n + n + 1 > b->max) {                      /* (sized by the length when it is known, else grown) */
        uint32_t m = total > 0 && (uint64_t)total + 1 > b->n + n + 1 ? (uint32_t)total + 1 : (b->n + n + 1) * 2;
        uint8_t *q = realloc(b->p, m); if (!q) return 1;
        b->p = q; b->max = m;
    }
    memcpy(b->p + b->n, d, n); b->n += (uint32_t)n; b->p[b->n] = 0;
    return 0;
}
static int ready(void) {
    if (wup_net_init()) return CATALOG_ENOWIFI;
    wup_net_time();                                    /* (the menu may not have heard back before this app started) */
    for (int t = 0; !wup_time_known() && t < 20000; t += 100) wup_sleep(100);
    return 0;
}
static int result(const char *url, int r, int stopped) {
    if (r != 200) printf("[catalog] %s: %d (tls %d)\n", url, r, wup_https_tls_error());   /* (the log, for whoever looks) */
    return r == 200 ? 0 : r == WUP_HTTPS_ESTOPPED ? stopped : from_https(r);
}
static int get(const char *url, buf_t *b) {
    int r = ready();
    wup_https_opts o = opts();
    if (!r) r = result(url, wup_https_get(url, &o, collect, b), CATALOG_ENOMEM);
    if (r) { free(b->p); b->p = 0; }
    return r;
}

/* ---- for the menu's other pages (Software Update: src/release.c): a URL into memory (0-terminated; free() it), or
   into ZIP_FILE */
int catalog_get_url(const char *url, const char *headers, char **out, uint32_t *n) {
    buf_t b = { 0 };
    int r = ready();
    wup_https_opts o = opts(); o.headers = headers;
    if (!r) r = result(url, wup_https_get(url, &o, collect, &b), CATALOG_ENOMEM);
    if (r) { free(b.p); return r; }
    *out = (char *)b.p; *n = b.n;
    return 0;
}
typedef struct { void (*progress)(int); uint32_t got; int bad; } dl_t;
static int dl_file(void *arg, const uint8_t *d, size_t n, long long total) {
    dl_t *f = arg;
    if (fs_stream_write(d, (uint32_t)n)) { f->bad = CATALOG_EWRITE; return 1; }
    f->got += (uint32_t)n;
    if (f->progress && total > 0) f->progress((int)(f->got * 100LL / total));
    return 0;
}
int catalog_download(const char *url, uint32_t size_kb, void (*progress)(int)) {
    if ((uint64_t)size_kb + 64 > fs_size_kb() - fs_used_kb()) return CATALOG_ESPACE;
    int r = ready();
    if (r) return r;
    lfs_mkdir(&fs_lfs, "/.get");
    if (fs_stream_open(ZIP_FILE)) return CATALOG_EWRITE;
    dl_t f = { .progress = progress };
    wup_https_opts o = opts();
    r = wup_https_get(url, &o, dl_file, &f);
    if (fs_stream_close() && !f.bad) f.bad = CATALOG_EWRITE;
    r = f.bad ? f.bad : result(url, r, CATALOG_EWRITE);
    if (r) fs_remove(ZIP_FILE);
    return r;
}

/* ---- the repositories */
static char repos[REPO_MAX][160], names[REPO_MAX][64];
static int nrepos, loaded;
#define REPOS_FILE "/.get/repos.txt"
static void tidy(char *u) {                           /* no trailing slash, no repo.json */
    size_t n = strlen(u);
    while (n && (u[n - 1] == '/' || u[n - 1] == ' ' || u[n - 1] == '\r')) u[--n] = 0;
    if (n > 10 && !strcmp(u + n - 10, "/repo.json")) { u[n - 10] = 0; tidy(u); }
}
static void name_of(int i) {
    const char *h = strstr(repos[i], "://"); h = h ? h + 3 : repos[i];
    size_t k = strcspn(h, "/"); if (k >= sizeof names[i]) k = sizeof names[i] - 1;
    memcpy(names[i], h, k); names[i][k] = 0;
}
static void repos_save(void) {
    char t[REPO_MAX * 162]; size_t n = 0;
    for (int i = 0; i < nrepos; i++) n += (size_t)snprintf(t + n, sizeof t - n, "%s\n", repos[i]);
    if (!n) t[n++] = '\n';                             /* (none: the file says so; no file is the project's own alone) */
    lfs_mkdir(&fs_lfs, "/.get");
    fs_write(REPOS_FILE, t, (uint32_t)n);
}
int catalog_repos(void) {
    if (loaded) return nrepos;
    loaded = 1; nrepos = 0;
    char t[REPO_MAX * 162]; int n = fs_read(REPOS_FILE, 0, t, sizeof t - 1);
    if (n < 0) { snprintf(repos[0], sizeof repos[0], "%s", CATALOG_URL); tidy(repos[0]); name_of(0); nrepos = 1; }   /* (a new pad) */
    else if (n > 0) {
        t[n] = 0;
        for (char *l = strtok(t, "\n"); l && nrepos < REPO_MAX; l = strtok(0, "\n")) {
            if (strncmp(l, "https://", 8)) continue;
            snprintf(repos[nrepos], sizeof repos[0], "%s", l); tidy(repos[nrepos]); name_of(nrepos); nrepos++;
        }
    }
    return nrepos;
}
const char *catalog_repo(int i) { catalog_repos(); return i >= 0 && i < nrepos ? repos[i] : ""; }
const char *catalog_repo_name(int i) { catalog_repos(); return i >= 0 && i < nrepos ? names[i] : ""; }
int catalog_repo_add(const char *url) {
    catalog_repos();
    char u[160];
    if (!strncmp(url, "https://", 8)) snprintf(u, sizeof u, "%s", url);
    else if (!strstr(url, "://")) snprintf(u, sizeof u, "https://%s", url);   /* (typed without it) */
    else return -1;                                    /* (http: the pad only fetches over HTTPS) */
    tidy(u);
    if (strlen(u) <= 8 || !strchr(u + 8, '.')) return -1;
    for (int i = 0; i < nrepos; i++) if (!strcmp(repos[i], u)) return -2;
    if (nrepos == REPO_MAX) return -3;
    snprintf(repos[nrepos], sizeof repos[0], "%s", u); name_of(nrepos); nrepos++;
    repos_save();
    return 0;
}
void catalog_repo_remove(int i) {
    catalog_repos();
    if (i < 0 || i >= nrepos) return;
    for (int k = i; k < nrepos - 1; k++) { memcpy(repos[k], repos[k + 1], sizeof repos[0]); memcpy(names[k], names[k + 1], sizeof names[0]); }
    nrepos--;
    repos_save();
}
int catalog_repo_failed;

/* ---- repo.json */
static int is(const char *js, const jsmntok_t *t, const char *s) {
    return t->type == JSMN_STRING && (int)strlen(s) == t->end - t->start && !strncmp(js + t->start, s, (size_t)(t->end - t->start));
}
static void copy(char *out, size_t n, const char *js, const jsmntok_t *t) {
    size_t k = (size_t)(t->end - t->start); if (k >= n) k = n - 1;
    memcpy(out, js + t->start, k); out[k] = 0;
}
static int skip(const jsmntok_t *t, int i) {          /* the token after i and everything inside it */
    int n = 1;
    for (int k = 0; k < n; k++) { if (t[i + k].type == JSMN_OBJECT) n += 2 * t[i + k].size; else if (t[i + k].type == JSMN_ARRAY) n += t[i + k].size; }
    return i + n;
}
static void installed_version(catalog_pkg *p) {
    char path[96], js[256]; p->installed[0] = 0;
    snprintf(path, sizeof path, "/.get/packages/%s/info.json", p->name);
    int n = fs_read(path, 0, js, sizeof js - 1);
    if (n <= 0) return;
    js[n] = 0;
    char *v = strstr(js, "\"version\""); if (!v) return;
    v = strchr(v + 9, '"'); if (!v) return;
    char *e = strchr(v + 1, '"'); if (!e || e - v - 1 >= (int)sizeof p->installed) return;
    memcpy(p->installed, v + 1, (size_t)(e - v - 1)); p->installed[e - v - 1] = 0;
}
#ifdef CATALOG_PREVIEW
#include "catalog_preview.h"
#endif
static int load_one(int repo) {                       /* its packages appended to catalog[] */
    char url[192]; snprintf(url, sizeof url, "%s/repo.json", catalog_repo(repo));
    buf_t b = { 0 };
    int r = get(url, &b);
    if (r) return r;
    jsmn_parser jp; jsmn_init(&jp);
    int n = jsmn_parse(&jp, (const char *)b.p, b.n, NULL, 0);   /* (counted first: only the tokens needed) */
    jsmntok_t *t = n > 0 ? malloc((size_t)n * sizeof *t) : 0;
    if (!t) { free(b.p); return n > 0 ? CATALOG_ENOMEM : CATALOG_EFORMAT; }
    jsmn_init(&jp); n = jsmn_parse(&jp, (const char *)b.p, b.n, t, (unsigned)n);
    const char *js = (const char *)b.p;
    r = CATALOG_EFORMAT;
    if (n > 0 && t[0].type == JSMN_OBJECT) {
        for (int i = 1; i < n; i = skip(t, i + 1)) {
            if (!is(js, &t[i], "packages") || t[i + 1].type != JSMN_ARRAY) continue;
            int a = i + 2;
            for (int e = 0; e < t[i + 1].size && catalog_count < CATALOG_MAX; e++) {
                int end = skip(t, a); catalog_pkg *p = &catalog[catalog_count]; memset(p, 0, sizeof *p);
                for (int j = a + 1; j < end; j = skip(t, j + 1)) {
                    const jsmntok_t *k = &t[j], *v = &t[j + 1];
                    if (is(js, k, "name")) copy(p->name, sizeof p->name, js, v);
                    else if (is(js, k, "title")) copy(p->title, sizeof p->title, js, v);
                    else if (is(js, k, "author")) copy(p->author, sizeof p->author, js, v);
                    else if (is(js, k, "version")) copy(p->version, sizeof p->version, js, v);
                    else if (is(js, k, "category")) copy(p->category, sizeof p->category, js, v);
                    else if (is(js, k, "description")) copy(p->description, sizeof p->description, js, v);
                    else if (is(js, k, "md5")) copy(p->md5, sizeof p->md5, js, v);
                    else if (is(js, k, "filesize")) p->filesize_kb = (uint32_t)atol(js + v->start);
                }
                p->repo = (uint8_t)repo;
                if (p->name[0] && !strchr(p->name, '/') && strlen(p->md5) == 32) { installed_version(p); catalog_count++; }
                a = end;
            }
            r = 0;
        }
    }
    free(t); free(b.p);
    return r;
}
int catalog_load(void) {
#ifdef CATALOG_PREVIEW
    return preview_load();
#endif
    catalog_count = 0; catalog_repo_failed = 0;
    if (!catalog_repos()) return 0;                    /* (none: no apps, nothing to fail) */
    int ok = 0, last = CATALOG_ENET;
    for (int i = 0; i < catalog_repos(); i++) {
        int r = load_one(i);
        if (r) { catalog_repo_failed |= 1 << i; last = r; } else ok = 1;
    }
    return ok ? 0 : last;                              /* (one repository that answers is enough) */
}

int catalog_system(const catalog_pkg *p) { return !strcmp(p->category, "system"); }

/* ---- icons: CATALOG_URL/packages/<name>/icon.png, kept as /.get/icons/<name>-<version>.png (older ones removed) */
static int icon_file(void *arg, const uint8_t *d, size_t n, long long total) { (void)arg; (void)total; return fs_stream_write(d, (uint32_t)n) ? 1 : 0; }
const char *catalog_icon(const catalog_pkg *p) {
    static char src[96];
    char path[80], url[192];
    snprintf(path, sizeof path, "/.get/icons/%s-%s.png", p->name, p->version);
    snprintf(src, sizeof src, "A:%s", path);
    if (fs_filesize(path) > 0) return src;
    if (ready()) return NULL;
    lfs_mkdir(&fs_lfs, "/.get"); lfs_mkdir(&fs_lfs, "/.get/icons");
    lfs_dir_t dir; struct lfs_info info; size_t k = strlen(p->name);
    if (!lfs_dir_open(&fs_lfs, &dir, "/.get/icons")) {          /* (an older version's icon) */
        char old[80];
        while (lfs_dir_read(&fs_lfs, &dir, &info) > 0)
            if (!strncmp(info.name, p->name, k) && info.name[k] == '-') { snprintf(old, sizeof old, "/.get/icons/%s", info.name); fs_remove(old); }
        lfs_dir_close(&fs_lfs, &dir);
    }
    snprintf(url, sizeof url, "%s/packages/%s/icon.png", catalog_repo(p->repo), p->name);
    if (fs_stream_open(path)) return NULL;
    wup_https_opts o = opts();
    int r = wup_https_get(url, &o, icon_file, NULL);
    int closed = fs_stream_close();
    if (r != 200 || closed) { fs_remove(path); return NULL; }
    return src;
}

/* ---- a package: its zip into ZIP_FILE, its MD5 checked as it arrives */
typedef struct { br_md5_context md5; void (*progress)(int); uint32_t got; int bad; } fetch_t;
static int to_file(void *arg, const uint8_t *d, size_t n, long long total) {
    fetch_t *f = arg;
    if (fs_stream_write(d, (uint32_t)n)) { f->bad = CATALOG_EWRITE; return 1; }
    br_md5_update(&f->md5, d, n); f->got += (uint32_t)n;
    if (f->progress && total > 0) f->progress((int)(f->got * 100LL / total));
    return 0;
}
int catalog_fetch(const catalog_pkg *p, void (*progress)(int)) {
    char url[256]; snprintf(url, sizeof url, "%s/zips/%s.zip", catalog_repo(p->repo), p->name);
    if ((uint64_t)p->filesize_kb + 64 > fs_size_kb() - fs_used_kb()) return CATALOG_ESPACE;   /* (the zip, then its files) */
    int r = ready();
    if (r) return r;
    lfs_mkdir(&fs_lfs, "/.get");
    if (fs_stream_open(ZIP_FILE)) return CATALOG_EWRITE;
    fetch_t f = { .progress = progress }; br_md5_init(&f.md5);
    wup_https_opts o = opts();
    r = wup_https_get(url, &o, to_file, &f);
    if (fs_stream_close() && !f.bad) f.bad = CATALOG_EWRITE;
    r = f.bad ? f.bad : result(url, r, CATALOG_EWRITE);
    if (!r) {
        uint8_t sum[16]; br_md5_out(&f.md5, sum);
        char hex[33]; for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", sum[i]);
        if (strcasecmp(hex, p->md5)) r = CATALOG_EDAMAGED;
    }
    if (r) fs_remove(ZIP_FILE);
    return r;
}

/* ---- zip: from ZIP_FILE, its central directory, then each entry in 4 KB pieces: stored ones as they are, deflated
   ones inflated a piece at a time (uzlib, with its 32 KB window): a game's data of several MB needs no more memory
   than a small file. (Pieces of 4 KB: Software Update's sink takes each slot's header sector as the first.) */
#include "uzlib.h"
static struct { uint32_t at, end; uint8_t buf[1024]; } zin;
static int zin_more(TINF_DATA *d) {                   /* the compressed data, 1 KB at a time from the file */
    uint32_t n = zin.end - zin.at < sizeof zin.buf ? zin.end - zin.at : sizeof zin.buf;
    if (!n || fs_read(ZIP_FILE, zin.at, zin.buf, n) != (int)n) return -1;
    zin.at += n; d->source = zin.buf + 1; d->source_limit = zin.buf + n;
    return zin.buf[0];
}
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int zread(uint32_t at, void *d, uint32_t n) { return fs_read(ZIP_FILE, at, d, n) == (int)n ? 0 : -1; }
int catalog_unzip(const catalog_sink *sink, void *arg) {
    static uint8_t piece[4096];
    int size = fs_filesize(ZIP_FILE);
    if (size < 22) return CATALOG_EDAMAGED;
    uint32_t tail = size < 1024 ? (uint32_t)size : 1024;      /* the end record (no comment: the last 22 bytes) */
    if (zread((uint32_t)size - tail, piece, tail)) return CATALOG_EDAMAGED;
    int e = (int)tail - 22;
    while (e >= 0 && le32(piece + e) != 0x06054b50u) e--;
    if (e < 0) return CATALOG_EDAMAGED;
    uint32_t count = le16(piece + e + 10), cd_size = le32(piece + e + 12), cd = le32(piece + e + 16);
    if (cd + cd_size > (uint32_t)size || cd_size > 65536) return CATALOG_EDAMAGED;
    uint8_t *dir = malloc(cd_size ? cd_size : 1);
    if (!dir) return CATALOG_ENOMEM;
    int r = zread(cd, dir, cd_size) ? CATALOG_EDAMAGED : 0;
    for (uint32_t i = 0, at = 0; !r && i < count; i++) {
        if (at + 46 > cd_size || le32(dir + at) != 0x02014b50u) { r = CATALOG_EDAMAGED; break; }
        uint32_t method = le16(dir + at + 10), csize = le32(dir + at + 20), usize = le32(dir + at + 24);
        uint32_t nlen = le16(dir + at + 28), xlen = le16(dir + at + 30), clen = le16(dir + at + 32), lho = le32(dir + at + 42);
        char path[128];
        if (nlen >= sizeof path || at + 46 + nlen > cd_size) { r = CATALOG_EDAMAGED; break; }
        memcpy(path, dir + at + 46, nlen); path[nlen] = 0;
        at += 46 + nlen + xlen + clen;
        if (!nlen || path[nlen - 1] == '/') continue;           /* (a folder) */
        if (strstr(path, "..") || path[0] == '/') { r = CATALOG_EDAMAGED; break; }   /* (only inside the filesystem) */
        uint8_t lh[30];
        if (zread(lho, lh, 30) || le32(lh) != 0x04034b50u) { r = CATALOG_EDAMAGED; break; }
        uint32_t data = lho + 30 + le16(lh + 26) + le16(lh + 28);
        if (data + csize > (uint32_t)size) { r = CATALOG_EDAMAGED; break; }
        if ((r = sink->begin(arg, path, usize))) break;
        if (method == 0 && csize == usize) {                    /* stored (the .wup files: deflated inside already) */
            for (uint32_t k = 0; !r && k < usize; k += sizeof piece) {
                uint32_t m = usize - k < sizeof piece ? usize - k : sizeof piece;
                r = zread(data + k, piece, m) ? CATALOG_EDAMAGED : sink->data(arg, piece, m);
                if (catalog_idle) catalog_idle();
            }
        } else if (method == 8) {                               /* deflated: a piece at a time */
            static uint8_t window[32768];
            static TINF_DATA d;
            uzlib_init(); uzlib_uncompress_init(&d, window, sizeof window);
            zin.at = data; zin.end = data + csize;
            d.source = d.source_limit = NULL; d.source_read_cb = zin_more;
            uint32_t done = 0;
            while (!r && done < usize) {
                d.dest = d.dest_start = piece;
                d.dest_limit = piece + (usize - done < sizeof piece ? usize - done : sizeof piece);
                int res = uzlib_uncompress(&d);
                uint32_t n = (uint32_t)(d.dest - piece);
                if (res < 0 || (!n && res != TINF_DONE)) { r = CATALOG_EDAMAGED; break; }
                if (n) { r = sink->data(arg, piece, n); done += n; }
                if (res == TINF_DONE) break;
                if (catalog_idle) catalog_idle();
            }
            if (!r && done != usize) r = CATALOG_EDAMAGED;
        } else r = CATALOG_EDAMAGED;
        if (!r) r = sink->end(arg);
    }
    free(dir);
    return r;
}

/* ---- install and remove, the libget way */
static void mkdirs(const char *path) {                /* the folders above a file */
    char p[128]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; lfs_mkdir(&fs_lfs, p); *s = '/'; }
}
typedef struct { char manifest[2048]; size_t mn; char path[136]; } inst_t;
static int put_begin(void *arg, const char *path, uint32_t size) {
    inst_t *x = arg; (void)size;
    snprintf(x->path, sizeof x->path, "/%s", path);
    mkdirs(x->path);
    if (fs_stream_open(x->path)) return CATALOG_EWRITE;
    x->mn += (size_t)snprintf(x->manifest + x->mn, sizeof x->manifest - x->mn, "U: %s\n", path);
    return 0;
}
static int put_data(void *arg, const uint8_t *d, uint32_t n) { (void)arg; return fs_stream_write(d, n) ? CATALOG_EWRITE : 0; }
static int put_end(void *arg) { (void)arg; return fs_stream_close() ? CATALOG_EWRITE : 0; }
int catalog_install(const catalog_pkg *p, void (*progress)(int)) {
#ifdef CATALOG_PREVIEW
    return preview_install(p, progress);
#endif
    int r = catalog_fetch(p, progress);
    if (r) return r;
    catalog_pkg q = *p; catalog_remove(&q);            /* (an update: the old files go first) */
    inst_t *x = calloc(1, sizeof *x);
    if (!x) r = CATALOG_ENOMEM;
    else {
        static const catalog_sink sink = { put_begin, put_data, put_end };
        r = catalog_unzip(&sink, x);
        if (r) {                                       /* (cut short: what it had written goes again, folders too) */
            fs_stream_close();
            for (char *l = strtok(x->manifest, "\n"); l; l = strtok(NULL, "\n")) {
                if (strncmp(l, "U: ", 3)) continue;
                char path[136]; snprintf(path, sizeof path, "/%s", l + 3);
                fs_remove(path);
                for (char *e; (e = strrchr(path, '/')) && e != path; ) {   /* (its emptied folders, not /apps itself) */
                    *e = 0;
                    if (!strchr(path + 1, '/') || lfs_remove(&fs_lfs, path)) break;
                }
            }
        }
        if (!r) {
            char dir[96], path[128], info[128];
            snprintf(dir, sizeof dir, "/.get/packages/%s/x", p->name); mkdirs(dir);
            snprintf(path, sizeof path, "/.get/packages/%s/manifest.install", p->name); fs_write(path, x->manifest, (uint32_t)x->mn);
            int k = snprintf(info, sizeof info, "{\"version\": \"%s\"}\n", p->version);
            snprintf(path, sizeof path, "/.get/packages/%s/info.json", p->name);
            if (fs_write(path, info, (uint32_t)k) != k) r = CATALOG_EWRITE;
        }
        free(x);
    }
    fs_remove(ZIP_FILE);
    return r;
}
int catalog_remove(const catalog_pkg *p) {
    char path[128], m[2048];
    snprintf(path, sizeof path, "/.get/packages/%s/manifest.install", p->name);
    int n = fs_read(path, 0, m, sizeof m - 1);
    if (n > 0) {
        m[n] = 0;
        for (char *l = strtok(m, "\n"); l; l = strtok(0, "\n")) {
            if (l[0] && l[1] == ':' && l[2] == ' ') {
                char f[136]; snprintf(f, sizeof f, "/%s", l + 3); fs_remove(f);
                for (char *e = strrchr(f, '/'); e && e > f; e = strrchr(f, '/')) { *e = 0; if (fs_remove(f)) break; }   /* (its folders, once empty) */
            }
        }
    }
    fs_remove(path);
    snprintf(path, sizeof path, "/.get/packages/%s/info.json", p->name); fs_remove(path);
    snprintf(path, sizeof path, "/.get/packages/%s", p->name); fs_remove(path);
    return 0;
}
