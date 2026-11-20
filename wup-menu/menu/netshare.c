/* netshare.c — see netshare.h. One TCP connection at a time, lock-step request/reply (protocol v1). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "netshare.h"
#include "fs.h"
#include "wup.h"
#include "wupfmt.h"
#include "handoff.h"
#include "armcpu.h"
#include "lwip/tcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"

#define CMD_AUTH 1
#define CMD_PING 2
#define CMD_STAT 3
#define CMD_OPENDIR 4
#define CMD_READDIR 5
#define CMD_FREEDIR 6
#define CMD_OPEN 7
#define CMD_READ 8
#define CMD_CLOSE 9
#define CMD_WRITE 10
#define NFS_VER 1

#define RX_CAP 8448                          /* one READDIR page (24 entries max) plus header room */
#define PAGE_MAX 24
#define DEADLINE_MS 8000
#define UP_DEADLINE_MS 60000                 /* an upload is many round trips: give it room */
#define UP_CHUNK 8192                        /* one WRITE payload's data (plus its 12-byte header) */

static struct tcp_pcb *pcb;
static int st = NS_IDLE, result, op, phase;   /* op: 0 test, 1 browse */
static uint32_t t0;
static char cfg_token[NETFS_TOKEN_LEN + 1];
static uint16_t cfg_port;
static int cfg_tok_len;

/* the reply stream: bytes accumulate until a whole framed message is there */
static uint8_t rx[RX_CAP + 16];
static uint32_t rx_len;
static uint8_t msg[RX_CAP];                  /* the last complete payload */
static uint32_t msg_len;
static int msg_st;

static char bpath[320];                      /* the directory being listed (server-side path) */
static uint32_t cookie;
static ns_ent_t ents[NS_MAX_ENTRIES];
static int nent, truncated;

/* upload state (op == 2): the littlefs file goes to the share, chunk by chunk */
static char up_local[96], up_remote[320];
static uint64_t up_size, up_done;
static uint32_t up_handle, up_chunk;
static uint8_t up_buf[12 + UP_CHUNK];        /* WRITE payload: handle u32, offset u64, data */

/* fetch state (op == 3): a .wup's code segment streams into WUP_HANDOFF_STAGE, ready for the loader */
#define F_RD 4096                               /* one READ's payload (the reply must fit RX_CAP) */
static char f_remote[320];
static uint32_t f_handle, f_done, f_total, f_next, f_crc, f_raw;
static uint64_t f_fsize;

static void fail(int e) { result = e; st = NS_FAIL; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

static void close_pcb(void) {
    if (!pcb) return;
    tcp_arg(pcb, 0); tcp_recv(pcb, 0); tcp_err(pcb, 0);
    if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    pcb = 0;
}
void ns_abort(void) { close_pcb(); rx_len = 0; if (st == NS_WORK) { result = ECANCELED; st = NS_FAIL; } }
int ns_state(void) { return st; }
int ns_result(void) { return result; }
int ns_op(void) { return op; }
const ns_ent_t *ns_entries(int *count) { *count = nent; return ents; }
const char *ns_browse_path(void) { return bpath; }
int ns_truncated(void) { return truncated; }

static int send_frame(uint8_t cmd, const void *payload, uint32_t len) {
    if (!pcb) return -1;
    uint8_t h[8];
    h[0] = cmd; h[1] = NFS_VER; h[2] = h[3] = 0;
    h[4] = (uint8_t)len; h[5] = (uint8_t)(len >> 8); h[6] = (uint8_t)(len >> 16); h[7] = (uint8_t)(len >> 24);
    if (tcp_write(pcb, h, 8, TCP_WRITE_FLAG_COPY) != ERR_OK) return -1;
    if (len && tcp_write(pcb, payload, (u16_t)len, TCP_WRITE_FLAG_COPY) != ERR_OK) return -1;
    tcp_output(pcb);
    return 0;
}
static int send_path(uint8_t cmd, const char *path) { return send_frame(cmd, path, (uint32_t)strlen(path)); }
static void send_readdir(void) {
    uint8_t rq[8];
    rq[0] = (uint8_t)cookie; rq[1] = (uint8_t)(cookie >> 8); rq[2] = (uint8_t)(cookie >> 16); rq[3] = (uint8_t)(cookie >> 24);
    rq[4] = PAGE_MAX; rq[5] = rq[6] = rq[7] = 0;
    if (send_frame(CMD_READDIR, rq, 8)) fail(EIO);
}
/* upload: OPEN(path, flags=write|create/truncate), WRITE chunks at absolute offsets, CLOSE */
static void send_up_open(void) {
    char rq[321];
    int n = (int)strlen(up_remote); if (n > 300) n = 300;
    memcpy(rq, up_remote, (size_t)n); rq[n] = 3;
    if (send_frame(CMD_OPEN, rq, (uint32_t)n + 1)) fail(EIO);
}
static void send_up_write(void) {
    uint32_t want = (uint32_t)(up_size - up_done);
    if (want > UP_CHUNK) want = UP_CHUNK;
    int n = fs_read(up_local, (uint32_t)up_done, up_buf + 12, want);
    if (n <= 0) { fail(EIO); return; }
    up_buf[0] = (uint8_t)up_handle; up_buf[1] = (uint8_t)(up_handle >> 8);
    up_buf[2] = (uint8_t)(up_handle >> 16); up_buf[3] = (uint8_t)(up_handle >> 24);
    for (int i = 0; i < 8; i++) up_buf[4 + i] = (uint8_t)(up_done >> (8 * i));
    up_chunk = (uint32_t)n;
    if (send_frame(CMD_WRITE, up_buf, 12 + (uint32_t)n)) fail(EIO);
}
static void send_up_close(void) {
    uint8_t b[4] = { (uint8_t)up_handle, (uint8_t)(up_handle >> 8), (uint8_t)(up_handle >> 16), (uint8_t)(up_handle >> 24) };
    if (send_frame(CMD_CLOSE, b, 4)) fail(EIO);
}
/* fetch: OPEN(path, flags=0), READ the header then the code into the stage, CLOSE */
static void send_f_open(void) {
    char rq[321];
    int n = (int)strlen(f_remote); if (n > 300) n = 300;
    memcpy(rq, f_remote, (size_t)n); rq[n] = 0;
    if (send_frame(CMD_OPEN, rq, (uint32_t)n + 1)) fail(EIO);
}
static void send_f_read(uint32_t off, uint32_t len) {
    uint8_t rq[16];
    rq[0] = (uint8_t)f_handle; rq[1] = (uint8_t)(f_handle >> 8); rq[2] = (uint8_t)(f_handle >> 16); rq[3] = (uint8_t)(f_handle >> 24);
    for (int i = 0; i < 8; i++) rq[4 + i] = (uint8_t)((uint64_t)off >> (8 * i));
    rq[12] = (uint8_t)len; rq[13] = (uint8_t)(len >> 8); rq[14] = (uint8_t)(len >> 16); rq[15] = (uint8_t)(len >> 24);
    if (send_frame(CMD_READ, rq, 16)) fail(EIO);
}
static void send_f_close(void) {
    uint8_t b[4] = { (uint8_t)f_handle, (uint8_t)(f_handle >> 8), (uint8_t)(f_handle >> 16), (uint8_t)(f_handle >> 24) };
    if (send_frame(CMD_CLOSE, b, 4)) fail(EIO);
}

/* the state machine after each complete reply (msg_st/msg_len/msg).
   phase = which request is outstanding: 0 AUTH, then test 1 PING / 2 STAT, browse 1 OPENDIR / 2 READDIR / 3 FREEDIR */
static void advance(void) {
    if (msg_st) { fail(msg_st); return; }
    if (phase == 0) {                                /* AUTH answered: the first real request */
        phase = 1;
        if (op == 0) send_frame(CMD_PING, 0, 0);
        else if (op == 2) send_up_open();
        else if (op == 3) send_f_open();
        else send_path(CMD_OPENDIR, bpath[0] ? bpath : "/");
        return;
    }
    if (op == 3) {                                   /* fetch: OPEN, header READ, code READs into the stage, CLOSE */
        if (phase == 1) {                            /* OPEN reply: <IBQ handle, type, size */
            if (msg_len < 13) { fail(EPROTO); return; }
            f_handle = get32(msg);
            f_fsize = get64(msg + 5);
            phase = 2;
            send_f_read(0, WUP_HDR_V4);
            return;
        }
        if (phase == 2) {                            /* the .wup header */
            wup_hdr_t h;
            if (msg_len < WUP_HDR_V4) { fail(EPROTO); return; }
            memcpy(&h, msg, sizeof h);
            if (h.magic != WUP_MAGIC || h.hdr_size < WUP_HDR_V1 || h.hdr_size > WUP_HDR_V4
                || !h.code_size || h.code_size > 0xd9000u
                || (uint64_t)h.code_off + h.code_size > f_fsize) { fail(EINVAL); return; }
            if (h.hdr_size >= WUP_HDR_V3 && h.code_raw > WUP_HANDOFF_WORK_MAX) { fail(EFBIG); return; }
            if (h.hdr_size >= WUP_HDR_V4 && h.big_size) { fail(ENOTSUP); return; }   /* its WUP_BIGMEM part can't stream (v1) */
            f_total = h.code_size; f_next = h.code_off; f_done = 0;
            f_crc = h.code_crc; f_raw = h.hdr_size >= WUP_HDR_V3 ? h.code_raw : 0;
            phase = 3;
            send_f_read(f_next, f_total < F_RD ? f_total : F_RD);
            f_next += f_total < F_RD ? f_total : F_RD;
            return;
        }
        if (phase == 3) {                            /* a code chunk: straight into the stage */
            uint32_t chunk = f_total - f_done < F_RD ? f_total - f_done : F_RD;
            if (msg_len != chunk) { fail(EIO); return; }        /* short: the file ended early */
            memcpy((void *)(WUP_HANDOFF_STAGE + f_done), msg, chunk);
            f_done += chunk;
            if (f_done < f_total) {
                send_f_read(f_next, f_total - f_done < F_RD ? f_total - f_done : F_RD);
                f_next += f_total - f_done < F_RD ? f_total - f_done : F_RD;
            } else {
                if (crc32((const void *)WUP_HANDOFF_STAGE, f_total) != f_crc) { fail(EIO); return; }
                phase = 4;
                send_f_close();
            }
            return;
        }
        result = 0; st = NS_DONE;                    /* phase 4: CLOSE answered: the stage holds the image */
        return;
    }
    if (op == 2) {                                   /* upload: OPEN answered, then WRITE pages, then CLOSE */
        if (phase == 1) {
            if (msg_len < 9) { fail(EPROTO); return; }
            up_handle = get32(msg);
            phase = 2;
            if (up_done < up_size) send_up_write(); else send_up_close();
            return;
        }
        if (phase == 2) {
            up_done += up_chunk;
            if (up_done < up_size) send_up_write(); else { phase = 3; send_up_close(); }
            return;
        }
        result = 0; st = NS_DONE;                    /* phase 3: CLOSE answered */
        return;
    }
    if (op == 0) {
        if (phase == 1) { phase = 2; send_path(CMD_STAT, "/"); }
        else { result = 0; st = NS_DONE; }
        return;
    }
    if (phase == 1) {                                /* OPENDIR reply: the cookie */
        if (msg_len < 4) { fail(EPROTO); return; }
        cookie = get32(msg);
        phase = 2;
        send_readdir();
        return;
    }
    if (phase == 2) {                                /* a READDIR page */
        if (msg_len < 4) { fail(EPROTO); return; }
        uint32_t next = get32(msg + msg_len - 4);
        uint32_t pos = 0, end = msg_len - 4;
        while (pos + 10 <= end) {
            uint32_t nl = msg[pos];
            if (pos + 10 + nl > end) { fail(EPROTO); return; }
            if (nent < NS_MAX_ENTRIES) {
                ns_ent_t *e = &ents[nent];
                e->is_dir = msg[pos + 1]; e->size = get64(msg + pos + 2);
                uint32_t k = nl < 255 ? nl : 255;
                memcpy(e->name, msg + pos + 10, k); e->name[k] = 0;
                nent++;
            } else truncated = 1;
            pos += 10 + nl;
        }
        if (pos != end) { fail(EPROTO); return; }
        if (!next) {
            phase = 3;
            uint32_t ck = cookie;
            if (send_frame(CMD_FREEDIR, &ck, 4)) fail(EIO);
        } else send_readdir();
        return;
    }
    /* phase 3: FREEDIR answered */
    result = 0; st = NS_DONE;
}

/* lwIP callbacks */
static err_t on_connected(void *arg, struct tcp_pcb *pcb_, err_t err) {
    (void)arg; (void)pcb_;
    if (err != ERR_OK) { fail(err == ERR_ABRT ? ECONNABORTED : ECONNREFUSED); return ERR_OK; }
    uint8_t rq[1 + NETFS_TOKEN_LEN];                  /* AUTH: token_len u8 + token */
    rq[0] = (uint8_t)cfg_tok_len;
    memcpy(rq + 1, cfg_token, (size_t)cfg_tok_len);
    if (send_frame(CMD_AUTH, rq, 1 + (uint32_t)cfg_tok_len)) fail(EIO);
    return ERR_OK;
}
static err_t on_recv(void *arg, struct tcp_pcb *pcb_, struct pbuf *p, err_t err) {
    (void)arg;
    if (err != ERR_OK || !p) { if (p) pbuf_free(p); if (st == NS_WORK) fail(ECONNRESET); return ERR_OK; }
    uint32_t tot = p->tot_len;
    for (struct pbuf *q = p; q; q = q->next) {
        uint32_t n = q->len;
        if (rx_len + n > sizeof rx) { pbuf_free(p); fail(EPROTO); return ERR_OK; }
        pbuf_copy_partial(q, rx + rx_len, n, 0);
        rx_len += n;
    }
    pbuf_free(p);
    tcp_recved(pcb_, (u16_t)tot);
    /* complete messages? (requests are lock-step: at most one outstanding) */
    while (st == NS_WORK && rx_len >= 8) {
        uint8_t cmd = rx[0], ver = rx[1];
        uint32_t ln = (uint32_t)rx[4] | (uint32_t)rx[5] << 8 | (uint32_t)rx[6] << 16 | (uint32_t)rx[7] << 24;
        if (ver != NFS_VER || ln > RX_CAP) { fail(EPROTO); break; }
        if (rx_len < 8 + ln) break;
        msg_st = cmd; msg_len = ln;
        memcpy(msg, rx + 8, ln);
        memmove(rx, rx + 8 + ln, rx_len - 8 - ln); rx_len -= 8 + ln;
        advance();
    }
    return ERR_OK;
}
static void on_err(void *arg, err_t err) { (void)arg; if (st == NS_WORK) fail(err == ERR_ABRT ? ECONNABORTED : EIO); }

/* ---- the public starts ---- */
static int begin(const netfs_cfg_t *cfg, int browse, const char *path) {
    ns_abort();
    if (!wup_net_ip()) { result = ENETUNREACH; return NS_FAIL; }   /* the stack is up but has no address yet */
    ip4_addr_t ip;
    if (ip4addr_aton(cfg->ip, &ip) == 0) { result = EINVAL; return NS_FAIL; }
    cfg_port = cfg->port ? cfg->port : NETFS_PORT_DEF;
    int tl = cfg->token_len > NETFS_TOKEN_LEN ? NETFS_TOKEN_LEN : cfg->token_len;
    memcpy(cfg_token, cfg->token, (size_t)tl); cfg_token[tl] = 0; cfg_tok_len = tl;
    op = browse; phase = 0; nent = 0; truncated = 0; rx_len = 0; cookie = 0;
    if (browse) snprintf(bpath, sizeof bpath, "%s", path && path[0] ? path : "/");
    pcb = tcp_new();
    if (!pcb) { result = ENOMEM; return NS_FAIL; }
    tcp_arg(pcb, 0); tcp_err(pcb, on_err); tcp_recv(pcb, on_recv);
    err_t e = tcp_connect(pcb, &ip, cfg_port, on_connected);
    if (e != ERR_OK) { close_pcb(); result = EIO; return NS_FAIL; }
    t0 = wup_us(); st = NS_WORK; result = 0;
    return NS_WORK;
}
int ns_begin_test(const netfs_cfg_t *cfg) { return begin(cfg, 0, 0); }
int ns_begin_browse(const netfs_cfg_t *cfg, const char *path) { return begin(cfg, 1, path); }
int ns_begin_upload(const netfs_cfg_t *cfg, const char *local, const char *remote) {
    int sz = fs_filesize(local);
    if (sz <= 0 || !remote || remote[0] != '/') { result = EINVAL; return NS_FAIL; }
    snprintf(up_local, sizeof up_local, "%s", local);
    snprintf(up_remote, sizeof up_remote, "%s", remote);
    up_size = (uint32_t)sz; up_done = 0; up_handle = 0; up_chunk = 0;
    return begin(cfg, 2, 0);
}
/* fetch a .wup from the share into WUP_HANDOFF_STAGE (its code, deflated, crc-checked) */
int ns_begin_fetch(const netfs_cfg_t *cfg, const char *remote) {
    if (!remote || remote[0] != '/') { result = EINVAL; return NS_FAIL; }
    snprintf(f_remote, sizeof f_remote, "%s", remote);
    f_handle = 0; f_done = f_total = f_next = f_crc = f_raw = 0; f_fsize = 0;
    return begin(cfg, 3, 0);
}
uint32_t ns_stage_size(void) { return f_total; }
uint32_t ns_stage_raw(void) { return f_raw; }
int ns_upload_progress(void) {
    if (op != 2 || st != NS_WORK) return -1;
    return up_size ? (int)(up_done * 100 / up_size) : 100;
}

void ns_step(void) {
    if (st != NS_WORK) return;
    uint32_t deadline = (op == 2 || op == 3) ? UP_DEADLINE_MS : DEADLINE_MS;
    if ((uint32_t)(wup_us() - t0) > deadline * 1000) { close_pcb(); fail(ETIMEDOUT); }
}
