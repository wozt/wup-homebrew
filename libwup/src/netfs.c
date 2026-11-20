/* netfs: the client half of the /net mount (include/netfs.h, tools/netfs-server/PROTOCOL.md).
 *
 * A read-only view of a folder on a computer, reached over one TCP connection (lwIP's sockets). The
 * connection is made lazily on the first /net access — never at startup, never blocking forever: every
 * socket wait has a timeout, a broken link drops the connection and the next operation reconnects
 * (with a short backoff so an absent server is not hammered). Reads carry absolute offsets, so a
 * reconnect only means re-AUTH + re-OPEN; handles keep their path and reopen themselves once.
 *
 * The settings come from /config/netfs in the pad's flash (the Homebrew Menu writes them; apps only
 * read). A small read cache (32 KiB by default) serves sequential reads; bigger requests go straight
 * through. Compiled into every app: without sockets (app.cfg has no net = yes) every call fails
 * cleanly with ENODEV. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "netfs.h"
#include "fs.h"
#include "wup.h"

int netfs_path(const char *p) { return !strcmp(p, "/net") || !strncmp(p, "/net/", 5); }

#if !defined(WUP_NET) || !defined(WUP_NET_SOCKETS)
/* ---- no sockets in this app: /net exists but is always unavailable ---- */
static int no_net(void) { errno = ENODEV; return -1; }
int  netfs_open(const char *p, int f, void **o) { (void)p; (void)f; (void)o; return no_net(); }
int  netfs_read(void *h, void *b, size_t n) { (void)h; (void)b; (void)n; return no_net(); }
int  netfs_seek(void *h, int64_t o, int w) { (void)h; (void)o; (void)w; return no_net(); }
int  netfs_size(void *h, uint64_t *s) { (void)h; (void)s; return no_net(); }
int64_t netfs_tell(void *h) { (void)h; return no_net(); }
int  netfs_close(void *h) { (void)h; return no_net(); }
int  netfs_fstat(void *h, uint64_t *s, int *d) { (void)h; (void)s; (void)d; return no_net(); }
int  netfs_stat(const char *p, uint64_t *s, int *d) { (void)p; (void)s; (void)d; return no_net(); }
void *netfs_opendir(const char *p) { (void)p; return 0; }
int  netfs_readdir(void *d, char *n, size_t z, uint64_t *s, int *i) { (void)d; (void)n; (void)z; (void)s; (void)i; return no_net(); }
void netfs_closedir(void *d) { (void)d; }
int  netfs_errno(void) { return ENODEV; }
int  netfs_connected(void) { return 0; }
void netfs_close_all(void) {}
#else
/* ---- sockets (lwIP, src/net.c): one connection, request/response, absolute offsets ---- */
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t i64;

#define CMD_AUTH    1
#define CMD_STAT    3
#define CMD_OPENDIR 4
#define CMD_READDIR 5
#define CMD_FREEDIR 6
#define CMD_OPEN    7
#define CMD_READ    8
#define CMD_CLOSE   9
#define NFS_VER     1

#define NF_HANDLES 6
#define NF_DIRS    3
#define NF_MAX_PATH 448
#define NF_MAX_READ (256 * 1024)          /* one READ's payload cap (the server's) */
#define NF_PAGE     24576                 /* one READDIR page buffer */
#define NF_DIR_MAX  48                    /* entries asked per READDIR */
#define CONN_MS     4000                   /* connect + auth */
#define IO_MS       8000                   /* any single socket wait */
#define RETRY_MS    2000                   /* backoff after a failed connect */
#define CACHE_MAX   (64 * 1024)
#define CACHE_MIN   4096

typedef struct {
    int used, dead, is_dir;
    u32 id;
    u64 pos, size;
    char *path;                             /* malloc'd: lets a dead handle reopen once */
} hf_t;
static hf_t hf[NF_HANDLES];

typedef struct {
    int used, eof;
    u32 cookie;
    u8 *page;                               /* malloc'd on first use */
    u32 page_len, page_pos;
} nd_t;
static nd_t dirs[NF_DIRS];

/* the read cache: one window of one file */
static u8 *cache;
static u32 cache_cap, cache_tried;
static u64 c_h = ~(u64)0, c_off, c_len;

static int sock = -1, xsem = -2, lerr, authed;
static uint32_t fail_at;                    /* wup_us() of the last failed connect */
static int status_written = -1;             /* what /config/netfs_status last said */
static void drop(void);

/* ---- little-endian framing (the protocol is little-endian on the wire) ---- */
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, (u16)v); put16(p + 2, (u16)(v >> 16)); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }
static u16 get16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 get32(const u8 *p) { return (u32)get16(p) | (u32)get16(p + 2) << 16; }
static u64 get64(const u8 *p) { return (u64)get32(p) | (u64)get32(p + 4) << 32; }

static int poll_fd(int fd, short ev, int ms) {
    struct pollfd p = { fd, ev, 0 };
    int r;
    do { r = poll(&p, 1, ms); } while (r < 0 && errno == EINTR);
    return r;
}
/* on a transport failure the link is dropped here: callers only see -1 */
static int send_all(const void *buf, u32 n) {
    const u8 *p = buf;
    while (n) {
        if (poll_fd(sock, POLLOUT, IO_MS) <= 0) { lerr = ETIMEDOUT; drop(); return -1; }
        ssize_t k = send(sock, p, n, 0);
        if (k < 0) { if (errno == EINTR) continue; lerr = errno ? errno : EIO; drop(); return -1; }
        if (!k) { lerr = EPIPE; drop(); return -1; }
        p += k; n -= (u32)k;
    }
    return 0;
}
static int recv_all(void *buf, u32 n) {
    u8 *p = buf;
    while (n) {
        if (poll_fd(sock, POLLIN, IO_MS) <= 0) { lerr = ETIMEDOUT; drop(); return -1; }
        ssize_t k = recv(sock, p, n, 0);
        if (k < 0) { if (errno == EINTR) continue; lerr = errno ? errno : EIO; drop(); return -1; }
        if (!k) { lerr = ECONNRESET; drop(); return -1; }
        p += k; n -= (u32)k;
    }
    return 0;
}
static void req_head(u8 *h, u8 cmd, u32 rqlen) {
    h[0] = cmd; h[1] = NFS_VER; h[2] = h[3] = 0; put32(h + 4, rqlen);
}
/* send a request; on success read the 8-byte response header into h and return its payload length
   (h[0] = status). -1: transport error (link already dropped). */
static int transact_head(u8 cmd, const void *rq, u32 rqlen, u8 *h) {
    u8 th[8];
    req_head(th, cmd, rqlen);
    if (send_all(th, 8) || (rqlen && send_all(rq, rqlen)) || recv_all(h, 8)) return -1;
    if (h[1] != NFS_VER) { lerr = EPROTO; drop(); return -1; }
    return (int)get32(h + 4);
}
/* small reply into a caller buffer: returns the payload length, or -1 (transport: dropped);
   *st gets the server status (255 after a transport error) */
static int transact(u8 cmd, const void *rq, u32 rqlen, u8 *out, u32 cap, u8 *st) {
    u8 h[8];
    int n = transact_head(cmd, rq, rqlen, h);
    if (n < 0) { *st = 255; return -1; }
    *st = h[0];
    if ((u32)n > cap) { lerr = EFBIG; drop(); return -1; }
    if (n && recv_all(out, (u32)n)) return -1;
    return n;
}
/* a reply whose payload streams into out: returns payload bytes, or -1 (transport dropped);
   *st gets the server status (nonzero: payload is absent, *st is the errno) */
static int transact_data(u8 cmd, const void *rq, u32 rqlen, u8 *out, u32 cap, u8 *st) {
    u8 h[8];
    int n = transact_head(cmd, rq, rqlen, h);
    if (n < 0) { *st = 255; return -1; }
    *st = h[0];
    if (*st) return 0;                             /* an error status: no payload */
    if ((u32)n > cap) { lerr = EFBIG; drop(); return -1; }
    if (n && recv_all(out, (u32)n)) return -1;
    return n;
}

/* ---- the configuration (/config/netfs, written by the menu) ---- */
static int cfg_loaded, cfg_on;
static char cfg_ip[NETFS_IP_LEN];
static u16 cfg_port;
static char cfg_token[NETFS_TOKEN_LEN + 1];
static u32 cfg_cache_kb;

static void load_cfg(void) {
    netfs_cfg_t c;
    memset(&c, 0, sizeof c);
    cfg_loaded = cfg_on = 0;
    if (!wup_fs_ok()) return;
    if (fs_read(NETFS_PATH, 0, &c, sizeof c) != (int)sizeof c || c.magic != NETFS_MAGIC || !c.on) return;
    int last = 0;
    for (int i = 0; i < NETFS_IP_LEN - 1 && c.ip[i]; i++) {
        if ((c.ip[i] < '0' || c.ip[i] > '9') && c.ip[i] != '.') break;
        last = i + 1;
    }
    if (!last) return;
    memcpy(cfg_ip, c.ip, NETFS_IP_LEN); cfg_ip[NETFS_IP_LEN - 1] = 0;
    cfg_port = c.port ? c.port : NETFS_PORT_DEF;
    int tl = c.token_len > NETFS_TOKEN_LEN ? NETFS_TOKEN_LEN : c.token_len;
    memcpy(cfg_token, c.token, (size_t)tl); cfg_token[tl] = 0;
    cfg_cache_kb = c.cache_kb == 255 ? 0 : c.cache_kb ? c.cache_kb : 32;
    cfg_on = 1; cfg_loaded = 1;
}

static void note_status(int result, int error) {     /* what the menu shows (written once per change) */
    int key = result * 4096 + (error & 4095);
    if (status_written == key || !wup_fs_ok()) return;
    netfs_status_t s;
    s.magic = 0x4f53464eu;                           /* "NFSO" */
    s.result = result; s.error = error;
    fs_write(NETFS_STATUS_PATH, &s, sizeof s);
    status_written = key;
}

static void drop(void) {
    if (sock >= 0) { shutdown(sock, 2); close(sock); sock = -1; }
    for (int i = 0; i < NF_HANDLES; i++) if (hf[i].used) hf[i].dead = 1;
    for (int i = 0; i < NF_DIRS; i++) {
        free(dirs[i].page); dirs[i].page = 0;
        dirs[i].used = dirs[i].eof = 0; dirs[i].cookie = 0; dirs[i].page_len = dirs[i].page_pos = 0;
    }
    c_len = 0; c_h = ~(u64)0;
    authed = 0;
}

static int do_connect(void) {                        /* 0 or -1 (lerr set); sock set on success */
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg_port);
    if (inet_pton(AF_INET, cfg_ip, &a.sin_addr) != 1) { lerr = EINVAL; return -1; }
    int loop = (ntohl(a.sin_addr.s_addr) >> 24) == 0x7f;
    if (!loop && !wup_net_online()) { lerr = ENETUNREACH; return -1; }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { lerr = errno ? errno : ENETDOWN; return -1; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if (r < 0 && errno != EINPROGRESS) { lerr = errno; close(s); return -1; }
    if (r < 0) {
        struct pollfd p = { s, POLLOUT, 0 };
        int pr;
        do { pr = poll(&p, 1, CONN_MS); } while (pr < 0 && errno == EINTR);
        if (pr <= 0) { lerr = ETIMEDOUT; close(s); return -1; }
        int e = 0; socklen_t el = sizeof e;
        getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el);
        if (e) { lerr = e; close(s); return -1; }
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
    sock = s;
    /* authenticate: token length byte + token (empty = none) */
    u8 rq[1 + NETFS_TOKEN_LEN], st;
    u32 tl = cfg_token[0] ? (u32)strlen(cfg_token) : 0;
    rq[0] = (u8)tl;
    memcpy(rq + 1, cfg_token, tl);
    if (transact(CMD_AUTH, rq, 1 + tl, 0, 0, &st)) { lerr = EIO; drop(); return -1; }
    if (st) { lerr = st; drop(); return -1; }
    authed = 1;
    return 0;
}

static int ensure(void) {                             /* the connection, or -1 (lerr set) */
    if (sock >= 0) return 0;
    wup_net_init();                                  /* bring the stack up (Wi-Fi via the handoff, or loopback) */
    u32 now = wup_us();
    if (fail_at && now - fail_at < RETRY_MS) { lerr = ETIMEDOUT; return -1; }
    if (!cfg_loaded) load_cfg();
    if (!cfg_on) { lerr = ENODEV; return -1; }
    if (do_connect()) { fail_at = now; note_status(-1, lerr); return -1; }
    fail_at = 0;
    note_status(0, 0);
    return 0;
}

static int xsem_take(void) {
    if (xsem == -2) xsem = wup_sem_new(1, 1);
    if (xsem < 0) return 1;                          /* no semaphore: single-threaded anyway */
    return wup_sem_wait(xsem, -1) == 0;
}
static void xsem_give(void) { if (xsem >= 0) wup_sem_signal(xsem); }

/* every public op: lock, connect, work, unlock */
#define NF_ENTER() do { if (!xsem_take()) { lerr = ENOMEM; return -1; } if (ensure()) { xsem_give(); return -1; } } while (0)
#define NF_LEAVE() xsem_give()

/* the server path for a client path: "/net" -> "/", "/net/x" -> "/x" */
static int srv_path(const char *p, char *out, u32 n) {
    const char *s = !strcmp(p, "/net") ? "/" : p + 4;
    u32 l = (u32)strlen(s);
    if (!l || l >= n || l > NF_MAX_PATH) { lerr = l ? ENAMETOOLONG : ENOENT; return -1; }
    memcpy(out, s, l + 1);
    return 0;
}

static int remote_stat(const char *path, u64 *size, int *is_dir) {
    char sp[NF_MAX_PATH + 8]; u8 rq[NF_MAX_PATH + 8], rs[16], st;
    if (srv_path(path, sp, sizeof sp)) return -1;
    u32 l = (u32)strlen(sp);
    if (l > NF_MAX_PATH) { lerr = ENAMETOOLONG; return -1; }
    memcpy(rq, sp, l);
    int n = transact(CMD_STAT, rq, l, rs, sizeof rs, &st);
    if (n < 0) return -1;
    if (st) { lerr = st; return -1; }
    if (n < 9) { lerr = EPROTO; return -1; }
    *is_dir = rs[0]; *size = get64(rs + 1);
    return 0;
}

int netfs_stat(const char *path, u64 *size, int *is_dir) {
    NF_ENTER();
    int r = remote_stat(path, size, is_dir);
    NF_LEAVE();
    return r;
}

static int remote_open(const char *path, hf_t *h) {  /* fill h->id/size/is_dir; 0 or -1 (lerr set) */
    char sp[NF_MAX_PATH + 8]; u8 rq[NF_MAX_PATH + 8], rs[16], st;
    if (srv_path(path, sp, sizeof sp)) return -1;
    u32 l = (u32)strlen(sp);
    if (l + 1 > sizeof rq) { lerr = ENAMETOOLONG; return -1; }
    memcpy(rq, sp, l); rq[l] = 0;                       /* flags byte: 0 = read-only */
    int n = transact(CMD_OPEN, rq, l + 1, rs, sizeof rs, &st);
    if (n < 0) return -1;
    if (st) { lerr = st; return -1; }
    if (n < 13) { lerr = EPROTO; return -1; }
    h->id = get32(rs); h->is_dir = rs[4]; h->size = get64(rs + 5);
    return 0;
}

static hf_t *handle_new(const char *path) {
    int i = 0; while (i < NF_HANDLES && hf[i].used) i++;
    if (i == NF_HANDLES) return 0;
    hf_t *h = &hf[i];
    memset(h, 0, sizeof *h);
    h->used = 1;
    h->path = malloc(strlen(path) + 1);
    if (!h->path) { h->used = 0; return 0; }
    strcpy(h->path, path);
    return h;
}
static void handle_free(hf_t *h) { free(h->path); h->path = 0; h->used = h->dead = h->is_dir = 0; }
static hf_t *handle_get(void *p) {
    uintptr_t i = (uintptr_t)p;
    if (!i || i > NF_HANDLES || !hf[i - 1].used) { lerr = EBADF; return 0; }
    return &hf[i - 1];
}

int netfs_open(const char *path, int flags, void **out) {
    (void)flags;                                       /* read-only: the caller checked the mode */
    NF_ENTER();
    hf_t *h = handle_new(path);
    if (!h) { lerr = ENOMEM; NF_LEAVE(); return -1; }
    if (remote_open(path, h)) { handle_free(h); NF_LEAVE(); return -1; }
    *out = (void *)(uintptr_t)((h - hf) + 1);
    NF_LEAVE();
    return 0;
}

/* reopen a dead handle by its path */
static int handle_ensure(hf_t *h) {
    if (!h->dead) return 0;
    if (remote_open(h->path, h)) return -1;
    h->dead = 0;
    return 0;
}

static int cache_setup(void) {                        /* the read window, once */
    if (cache_tried) return cache != 0;
    cache_tried = 1;
    if (!cfg_cache_kb) return 0;                      /* cache_kb 255 in the config: off */
    u32 bytes = cfg_cache_kb * 1024;
    if (bytes > CACHE_MAX) bytes = CACHE_MAX;
    u32 cap = CACHE_MIN;
    while (cap * 2 <= bytes) cap *= 2;                 /* a power of two, for the aligned window */
    cache = malloc(cap);
    if (!cache) return 0;
    cache_cap = cap;
    return 1;
}

int netfs_read(void *p, void *buf, size_t n) {
    hf_t *h = handle_get(p);
    if (!h) return -1;
    if (h->is_dir) { lerr = EISDIR; return -1; }
    NF_ENTER();
    int r = 0, retried = 0;
    u8 *out = buf;
    u64 want = n;
    while (want) {
        u64 idx = (u64)(h - hf);
        if (cache && cache_cap && c_h == idx && h->pos >= c_off && h->pos < c_off + c_len) {
            u64 avail = c_off + c_len - h->pos;
            u32 k = want < avail ? (u32)want : (u32)avail;
            memcpy(out, cache + (u32)(h->pos - c_off), k);
            out += k; h->pos += k; want -= k; r += (int)k;
            continue;
        }
        if (handle_ensure(h)) { if (r) break; r = -1; break; }
        if (!cache_setup() || want >= cache_cap) {     /* straight through (or no cache) */
            u32 chunk = want > NF_MAX_READ ? NF_MAX_READ : (u32)want;
            u8 rq[20], st;
            put32(rq, h->id); put64(rq + 4, h->pos); put32(rq + 12, chunk);
            int got = transact_data(CMD_READ, rq, 20, out, chunk, &st);
            if (got < 0) { if (r) break; r = -1; break; }
            if (st) {
                if (st == EBADF && !retried) { retried = 1; h->dead = 1; continue; }
                lerr = st;
                if (r) break; r = -1; break;
            }
            if (!got) break;                          /* EOF */
            out += got; h->pos += (u32)got; want -= (u32)got; r += got;
            if ((u32)got < chunk) break;               /* short: EOF */
            continue;
        }
        /* fill the cache window covering pos */
        u64 base = h->pos & ~(u64)(cache_cap - 1);
        u8 rq[20], st;
        put32(rq, h->id); put64(rq + 4, base); put32(rq + 12, cache_cap);
        int got = transact_data(CMD_READ, rq, 20, cache, cache_cap, &st);
        if (got < 0) { if (r) break; r = -1; break; }
        if (st) {
            if (st == EBADF && !retried) { retried = 1; h->dead = 1; continue; }
            lerr = st;
            if (r) break; r = -1; break;
        }
        if (!got) break;                              /* EOF right at pos */
        c_h = idx; c_off = base; c_len = (u32)got;
        /* loop: the cache branch serves pos now */
    }
    NF_LEAVE();
    return r;
}

int netfs_seek(void *p, i64 off, int whence) {
    hf_t *h = handle_get(p);
    if (!h) return -1;
    i64 base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (i64)h->pos : (i64)h->size;
    i64 np = base + off;
    if (np < 0) { lerr = EINVAL; return -1; }       /* past the end is fine: reads there give EOF */
    h->pos = (u64)np;
    return 0;
}
int netfs_size(void *p, u64 *size) {
    hf_t *h = handle_get(p);
    if (!h) return -1;
    *size = h->size;
    return 0;
}
int64_t netfs_tell(void *p) {
    hf_t *h = handle_get(p);
    if (!h) return -1;
    return (int64_t)h->pos;
}
int netfs_fstat(void *p, u64 *size, int *is_dir) {
    hf_t *h = handle_get(p);
    if (!h) return -1;
    *size = h->size; *is_dir = h->is_dir;
    return 0;
}
int netfs_close(void *p) {
    hf_t *h = handle_get(p);
    if (!h) return -1;
    if (xsem_take()) {
        if (sock >= 0 && !h->dead) {
            u8 rq[4], st;
            put32(rq, h->id);
            transact(CMD_CLOSE, rq, 4, 0, 0, &st);
        }
        xsem_give();
    }
    if (c_h == (u64)(h - hf)) { c_len = 0; c_h = ~(u64)0; }
    handle_free(h);
    return 0;
}

/* ---- directories: OPENDIR -> cookie, READDIR pages of entries ---- */
void *netfs_opendir(const char *path) {
    if (!xsem_take()) { lerr = ENOMEM; return 0; }
    if (ensure()) { xsem_give(); return 0; }
    char sp[NF_MAX_PATH + 8]; u8 rq[NF_MAX_PATH + 8], rs[8], st;
    if (srv_path(path, sp, sizeof sp)) { NF_LEAVE(); return 0; }
    u32 l = (u32)strlen(sp);
    if (l > NF_MAX_PATH) { lerr = ENAMETOOLONG; NF_LEAVE(); return 0; }
    memcpy(rq, sp, l);
    int n = transact(CMD_OPENDIR, rq, l, rs, sizeof rs, &st);
    if (n < 0 || st || n < 4) { if (!n) lerr = st ? st : EPROTO; NF_LEAVE(); return 0; }
    int i = 0; while (i < NF_DIRS && dirs[i].used) i++;
    if (i == NF_DIRS) { lerr = EMFILE; NF_LEAVE(); return 0; }
    nd_t *d = &dirs[i];
    memset(d, 0, sizeof *d);
    d->used = 1; d->cookie = get32(rs);
    NF_LEAVE();
    return (void *)(uintptr_t)(i + 1);
}

static nd_t *dir_get(void *p) {
    uintptr_t i = (uintptr_t)p;
    if (!i || i > NF_DIRS || !dirs[i - 1].used) { lerr = EBADF; return 0; }
    return &dirs[i - 1];
}

/* the next page of entries into d->page; 1 = entries follow, 0 = end of dir, < 0 error */
static int dir_page(nd_t *d) {
    if (d->eof) return 0;
    if (!d->page) { d->page = malloc(NF_PAGE); if (!d->page) { lerr = ENOMEM; return -1; } }
    u8 rq[8], st;
    put32(rq, d->cookie); put32(rq + 4, NF_DIR_MAX);
    int got = transact_data(CMD_READDIR, rq, 8, d->page, NF_PAGE, &st);
    if (got < 0) return -1;
    if (st) { lerr = st; return -1; }
    if (got < 4) { lerr = EPROTO; return -1; }
    u32 next = get32(d->page + got - 4);
    d->page_len = (u32)got - 4; d->page_pos = 0;
    if (!next) d->eof = 1;
    return 1;
}

int netfs_readdir(void *p, char *name, size_t n, u64 *size, int *is_dir) {
    nd_t *d = dir_get(p);
    if (!d) return -1;
    for (;;) {
        if (d->page_pos < d->page_len) {
            const u8 *e = d->page + d->page_pos;
            u32 left = d->page_len - d->page_pos;
            if (left < 10) { lerr = EPROTO; return -1; }
            u32 nl = e[0];
            if (nl > 255 || 10 + nl > left) { lerr = EPROTO; return -1; }
            *is_dir = e[1]; *size = get64(e + 2);
            u32 k = nl < n - 1 ? nl : (u32)n - 1;
            memcpy(name, e + 10, k); name[k] = 0;
            d->page_pos += 10 + nl;
            return 1;
        }
        if (d->eof) return 0;
        NF_ENTER();
        int r = dir_page(d);
        NF_LEAVE();
        if (r <= 0) return r;
    }
}
void netfs_closedir(void *p) {
    nd_t *d = dir_get(p);
    if (!d) return;
    if (!d->eof && d->cookie && xsem_take()) {
        u8 rq[4], st;
        put32(rq, d->cookie);
        transact(CMD_FREEDIR, rq, 4, 0, 0, &st);
        xsem_give();
    }
    free(d->page); d->page = 0;
    memset(d, 0, sizeof *d);
}
int netfs_errno(void) { return lerr ? lerr : EIO; }
int netfs_connected(void) { return sock >= 0; }
void netfs_close_all(void) {
    drop();
    free(cache); cache = 0; cache_cap = 0; cache_tried = 0;
}
#endif
