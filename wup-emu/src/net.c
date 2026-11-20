/* The far end of the GamePad's Wi-Fi link: a minimal "console" network endpoint.
 *
 * Ethernet frames the firmware sends through the (fake) BCM4319 arrive in net_from_pad().
 * We answer ARP and DHCP ourselves, as the console does (console 192.168.1.10, pad 192.168.1.11,
 * per a decrypted real session), and bridge every UDP flow to real UDP sockets on the host so the
 * console side of the DRC protocol can be any program:
 *
 *   pad 192.168.1.11:P  -> console 192.168.1.10:C    becomes   host socket bound to NET_HOST:P  -> NET_HOST:C
 *   host NET_HOST:C     -> NET_HOST:P                becomes   192.168.1.10:C -> 192.168.1.11:P (into the pad)
 *
 * So a console program simply binds its usual ports (e.g. 50010, 50020..50023) on NET_HOST
 * (default 127.0.0.1) and sends to the pad's ports on the same address. The emulator binds a pad port
 * when the pad first sends from it, and pre-binds NET_PAD_PORTS (comma list) for flows the console starts. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

void wifi_net_inject(const uint8_t *eth, int len);
const uint8_t *wifi_pad_mac(void);
extern uint8_t wifi_bssid[6];

static const uint8_t CON_IP[4] = { 192, 168, 1, 10 }, PAD_IP[4] = { 192, 168, 1, 11 };
static uint8_t pad_mac[6];
static int nlog = -1;
#define NLOG(...) do { if (nlog < 0) nlog = getenv("NET_DEBUG") != NULL; if (nlog) fprintf(stderr, __VA_ARGS__); } while (0)

typedef struct { uint16_t port; int fd; uint32_t rx, tx; } PadSock;
static PadSock socks[32]; static int nsocks;
static struct in_addr host_addr;
static bool inited;

#define TCP_MAX 8
typedef struct {
    uint16_t pad_port, dport;
    int fd;                 /* host socket, nonblocking */
    int state;              /* 0 connecting, 1 established, 2 closing */
    uint32_t iss;           /* our initial send seq */
    uint32_t next;          /* next seq we send */
    uint32_t exp;           /* next seq we expect from the pad */
    uint32_t last_ack;      /* last pure ACK we sent (for pad retransmits) */
    int idle;               /* net_poll rounds since last activity */
} TcpConn;
static TcpConn tconns[TCP_MAX];
static uint32_t tcp_iss_ctr = 0x5f3a0000;

static void net_init(void) {
    inited = true;
    for (int i = 0; i < TCP_MAX; i++) tconns[i].fd = -1;
    const char *h = getenv("NET_HOST");
    inet_aton(h ? h : "127.0.0.1", &host_addr);
}

static int pad_sock(uint16_t port) {
    for (int i = 0; i < nsocks; i++) if (socks[i].port == port) return socks[i].fd;
    if (nsocks >= 32) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = host_addr };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "[net] can't bind pad port %u: %s\n", port, strerror(errno));
        close(fd); return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    socks[nsocks].port = port; socks[nsocks].fd = fd; nsocks++;
    NLOG("[net] bridging pad port %u\n", port);
    return fd;
}

static uint16_t csum(const uint8_t *p, int n, uint32_t s) {
    for (int i = 0; i + 1 < n; i += 2) s += (p[i] << 8) | p[i + 1];
    if (n & 1) s += p[n - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return ~s;
}

/* build Ethernet + IPv4 + UDP from the console to the pad */
static void send_udp(uint16_t sport, uint16_t dport, const uint8_t *data, int dlen, const uint8_t *dst_ip, const uint8_t *dst_mac) {
    static uint8_t f[2048];
    if (dlen > 1472) return;
    static uint16_t ipid;
    memcpy(f, dst_mac, 6); memcpy(f + 6, wifi_bssid, 6); f[12] = 0x08; f[13] = 0x00;
    uint8_t *ip = f + 14;
    int tot = 20 + 8 + dlen;
    memset(ip, 0, 20);
    ip[0] = 0x45; ip[2] = tot >> 8; ip[3] = tot; ip[4] = ipid >> 8; ip[5] = ipid; ipid++;
    ip[8] = 64; ip[9] = 17; memcpy(ip + 12, CON_IP, 4); memcpy(ip + 16, dst_ip, 4);
    uint16_t c = csum(ip, 20, 0); ip[10] = c >> 8; ip[11] = c;
    uint8_t *u = ip + 20;
    u[0] = sport >> 8; u[1] = sport; u[2] = dport >> 8; u[3] = dport; u[4] = (8 + dlen) >> 8; u[5] = 8 + dlen; u[6] = u[7] = 0;
    memcpy(u + 8, data, dlen);
    {   /* UDP checksum over the IPv4 pseudo-header */
        uint32_t ps = (CON_IP[0] << 8 | CON_IP[1]) + (CON_IP[2] << 8 | CON_IP[3]) +
                      (dst_ip[0] << 8 | dst_ip[1]) + (dst_ip[2] << 8 | dst_ip[3]) + 17 + 8 + dlen;
        uint16_t uc = csum(u, 8 + dlen, ps); if (!uc) uc = 0xffff;
        u[6] = uc >> 8; u[7] = uc;
    }
    wifi_net_inject(f, 14 + tot);
}

/* ---- DHCP server (the console hands the pad 192.168.1.11) ---- */
static void dhcp(const uint8_t *b, int n) {
    if (n < 240 || b[0] != 1) return;
    int type = 0;
    for (int i = 240; i + 1 < n && b[i] != 255; ) {
        if (b[i] == 0) { i++; continue; }
        if (b[i] == 53) type = b[i + 2];
        i += 2 + b[i + 1];
    }
    if (type != 1 && type != 3) return;               /* DISCOVER / REQUEST */
    uint8_t r[300]; memset(r, 0, sizeof r);
    r[0] = 2; r[1] = 1; r[2] = 6; memcpy(r + 4, b + 4, 4);           /* reply, xid */
    memcpy(r + 10, b + 10, 2);                                        /* flags */
    memcpy(r + 16, PAD_IP, 4); memcpy(r + 20, CON_IP, 4); memcpy(r + 28, b + 28, 16);
    r[236] = 99; r[237] = 130; r[238] = 83; r[239] = 99;
    int o = 240;
    r[o++] = 53; r[o++] = 1; r[o++] = type == 1 ? 2 : 5;              /* OFFER / ACK */
    r[o++] = 54; r[o++] = 4; memcpy(r + o, CON_IP, 4); o += 4;        /* server id */
    r[o++] = 51; r[o++] = 4; r[o++] = 0; r[o++] = 1; r[o++] = 0x51; r[o++] = 0x80;  /* lease 86400 s */
    r[o++] = 1; r[o++] = 4; r[o++] = 255; r[o++] = 255; r[o++] = 255; r[o++] = 0;  /* netmask */
    r[o++] = 3; r[o++] = 4; memcpy(r + o, CON_IP, 4); o += 4;         /* router (as the real console does) */
    r[o++] = 255;
    static const uint8_t bcast_ip[4] = { 255, 255, 255, 255 }, bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    NLOG("[net] DHCP %s -> %s 192.168.1.11\n", type == 1 ? "DISCOVER" : "REQUEST", type == 1 ? "OFFER" : "ACK");
    send_udp(67, 68, r, o, bcast_ip, bcast);
}

/* ---- TCP proxy: the pad is the client, we are the "console" (192.168.1.10).
   Each pad connection is terminated here (SYN-ACK spoofed) and bridged to a real
   TCP socket on the host (NET_HOST:dport, default 127.0.0.1), so pad lwIP talks
   to e.g. the netfs server running on the PC. LAN test path only. */
static uint16_t tcp_checksum(const uint8_t *src, const uint8_t *dst, const uint8_t *tcp, int len) {
    uint32_t s = 0;
    for (int i = 0; i < 4; i += 2) s += (src[i] << 8) | src[i + 1];
    for (int i = 0; i < 4; i += 2) s += (dst[i] << 8) | dst[i + 1];
    s += 6 + len;                                    /* proto TCP + tcp length */
    for (int i = 0; i + 1 < len; i += 2) s += (tcp[i] << 8) | tcp[i + 1];
    if (len & 1) s += tcp[len - 1] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    uint16_t r = ~s;
    return r ? r : 0xffff;
}

/* send one TCP segment console -> pad. flags: TH_*. data may be NULL. */
static void send_tcp(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
                     uint8_t flags, const uint8_t *data, int dlen) {
    static uint8_t f[1600];
    int thl = 20, tot = 20 + thl + dlen;
    if (tot > 1500) return;
    static uint16_t ipid;
    memcpy(f, pad_mac, 6); memcpy(f + 6, wifi_bssid, 6); f[12] = 0x08; f[13] = 0x00;
    uint8_t *ip = f + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45; ip[2] = tot >> 8; ip[3] = tot; ip[4] = ipid >> 8; ip[5] = ipid; ipid++;
    ip[8] = 64; ip[9] = 6; memcpy(ip + 12, CON_IP, 4); memcpy(ip + 16, PAD_IP, 4);
    uint16_t c = csum(ip, 20, 0); ip[10] = c >> 8; ip[11] = c;
    uint8_t *t = ip + 20;
    memset(t, 0, thl);
    t[0] = sport >> 8; t[1] = sport; t[2] = dport >> 8; t[3] = dport;
    t[4] = seq >> 24; t[5] = seq >> 16; t[6] = seq >> 8; t[7] = seq;
    t[8] = ack >> 24; t[9] = ack >> 16; t[10] = ack >> 8; t[11] = ack;
    t[12] = 0x50; t[13] = flags; t[14] = 0xff; t[15] = 0xff;   /* data offset 5, win 65535 */
    if (dlen) memcpy(t + thl, data, dlen);
    uint16_t tc = tcp_checksum(CON_IP, PAD_IP, t, thl + dlen);
    t[16] = tc >> 8; t[17] = tc;
    wifi_net_inject(f, 14 + tot);
}

static TcpConn *tcp_find(uint16_t pad_port, uint16_t dport) {
    for (int i = 0; i < TCP_MAX; i++)
        if (tconns[i].fd >= 0 && tconns[i].pad_port == pad_port && tconns[i].dport == dport) return &tconns[i];
    return NULL;
}
static TcpConn *tcp_alloc(void) {
    for (int i = 0; i < TCP_MAX; i++) if (tconns[i].fd < 0) return &tconns[i];
    return NULL;
}
static void tcp_close(TcpConn *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}
static void tcp_rst(uint16_t pad_port, uint16_t dport, uint32_t seq, uint32_t ack) {
    send_tcp(dport, pad_port, seq, ack, 0x04, NULL, 0);       /* RST|ACK */
    NLOG("[net] tcp rst pad %u -> %u\n", pad_port, dport);
}

/* one TCP segment pad -> console */
static void tcp_from_pad(const uint8_t *t, int avail, uint16_t dport) {
    if (avail < 20) return;
    uint16_t sport = (t[0] << 8) | t[1];
    uint32_t seq = (t[4] << 24) | (t[5] << 16) | (t[6] << 8) | t[7];
    uint8_t thl = (t[12] >> 4) * 4, flags = t[13];
    if (thl < 20 || thl > avail) return;
    int dlen = avail - thl;
    const uint8_t *data = t + thl;

    if (flags & 0x04) {                                       /* RST */
        TcpConn *c = tcp_find(sport, dport);
        if (c) { NLOG("[net] tcp rst from pad %u\n", sport); tcp_close(c); }
        return;
    }
    if (flags & 0x02 && !(flags & 0x10)) {                    /* SYN */
        TcpConn *old = tcp_find(sport, dport);
        if (old) {                                            /* retransmitted SYN: resend SYN-ACK */
            send_tcp(dport, sport, old->next - 1, old->exp, 0x12, NULL, 0);
            return;
        }
        TcpConn *c = tcp_alloc();
        if (!c) { tcp_rst(sport, dport, seq + 1, 0); return; }
        memset(c, 0, sizeof *c); c->fd = -1;
        c->pad_port = sport; c->dport = dport;
        c->iss = tcp_iss_ctr = tcp_iss_ctr * 1103515245u + 12345u;
        c->next = c->iss + 1;
        c->exp = seq + 1;
        c->idle = 0;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { tcp_rst(sport, dport, seq + 1, 0); return; }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(dport), .sin_addr = host_addr };
        connect(fd, (struct sockaddr *)&to, sizeof to);       /* in progress; EWOULDBLOCK expected */
        c->fd = fd; c->state = 0;
        send_tcp(dport, sport, c->iss, c->exp, 0x12, NULL, 0); /* SYN-ACK */
        NLOG("[net] tcp syn pad %u -> console %u\n", sport, dport);
        return;
    }
    TcpConn *c = tcp_find(sport, dport);
    if (!c) return;                                           /* stray segment: drop */
    c->idle = 0;
    if (c->state == 0) return;                                /* host connect still in progress: pad will retransmit */
    if (!(flags & 0x10)) return;                              /* no ACK yet: ignore */

    /* data / FIN: only accept in-order bytes */
    if (dlen || (flags & 0x01)) {
        uint32_t end = seq + dlen + ((flags & 0x01) ? 1 : 0);
        if (seq == c->exp) {
            if (dlen) {
                ssize_t w = send(c->fd, data, dlen, MSG_NOSIGNAL);
                if (w <= 0) {
                    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;  /* don't ACK what we didn't take */
                    tcp_close(c); return;
                }
                c->exp += w;                                   /* pad retransmits any remainder */
            }
            if (flags & 0x01) { c->exp++; shutdown(c->fd, SHUT_WR); }
            send_tcp(dport, sport, c->next, c->exp, 0x10, NULL, 0);   /* ACK */
            c->last_ack = c->exp;
        } else if (end <= c->exp) {
            send_tcp(dport, sport, c->next, c->exp, 0x10, NULL, 0);   /* pure dup: re-ACK */
        } else if (seq < c->exp) {
            int skip = c->exp - seq;                            /* partial overlap */
            if (dlen > skip) {
                ssize_t w = send(c->fd, data + skip, dlen - skip, MSG_NOSIGNAL);
                if (w > 0) c->exp += w;
            }
            if (flags & 0x01) { c->exp++; shutdown(c->fd, SHUT_WR); }
            send_tcp(dport, sport, c->next, c->exp, 0x10, NULL, 0);
        } else {
            send_tcp(dport, sport, c->next, c->exp, 0x10, NULL, 0);   /* gap: dup ACK, wait retransmit */
        }
    }
}

void net_from_pad(const uint8_t *e, int n) {
    if (!inited) net_init();
    if (n < 14) return;
    memcpy(pad_mac, e + 6, 6);
    uint16_t type = (e[12] << 8) | e[13];
    if (type == 0x0806 && n >= 42) {                                  /* ARP */
        const uint8_t *a = e + 14;
        if (a[7] == 1 && !memcmp(a + 24, CON_IP, 4)) {                /* who-has 192.168.1.10 */
            uint8_t r[42];
            memcpy(r, e + 6, 6); memcpy(r + 6, wifi_bssid, 6); r[12] = 0x08; r[13] = 0x06;
            uint8_t *q = r + 14;
            memcpy(q, a, 6); q[6] = 0; q[7] = 2;
            memcpy(q + 8, wifi_bssid, 6); memcpy(q + 14, CON_IP, 4);
            memcpy(q + 18, a + 8, 6); memcpy(q + 24, a + 14, 4);
            NLOG("[net] ARP reply 192.168.1.10\n");
            wifi_net_inject(r, 42);
        }
        return;
    }
    if (type != 0x0800 || n < 14 + 28) { NLOG("[net] pad frame type %04x len %d\n", type, n); return; }
    const uint8_t *ip = e + 14;
    int ihl = (ip[0] & 15) * 4, tot = (ip[2] << 8) | ip[3];
    if (ip[9] == 6) {                                          /* TCP */
        const uint8_t *t = ip + ihl;
        if (tot - ihl >= 20) tcp_from_pad(t, tot - ihl, (t[2] << 8) | t[3]);
        return;
    }
    if (ip[9] != 17) { NLOG("[net] pad IP proto %d\n", ip[9]); return; }
    const uint8_t *u = ip + ihl;
    uint16_t sport = (u[0] << 8) | u[1], dport = (u[2] << 8) | u[3];
    int ulen = ((u[4] << 8) | u[5]) - 8;
    if (ulen < 0 || ihl + 8 + ulen > tot) return;
    if (dport == 67) { dhcp(u + 8, ulen); return; }
    int fd = pad_sock(sport);
    if (fd < 0) return;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(dport), .sin_addr = host_addr };
    sendto(fd, u + 8, ulen, 0, (struct sockaddr *)&to, sizeof to);
    for (int i = 0; i < nsocks; i++) if (socks[i].fd == fd && !socks[i].tx++) NLOG("[net] pad %u -> console %u (first, %d bytes)\n", sport, dport, ulen);
}

void net_poll(void) {
    if (!inited) {
        net_init();
        const char *pp = getenv("NET_PAD_PORTS");
        if (pp) for (const char *q = pp; *q; ) { pad_sock((uint16_t)strtol(q, (char **)&q, 10)); if (*q == ',') q++; }
    }
    static uint8_t buf[2048];
    for (int i = 0; i < nsocks; i++) {
        for (int k = 0; k < 64; k++) {
            struct sockaddr_in from; socklen_t fl = sizeof from;
            int r = recvfrom(socks[i].fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (r < 0) break;
            if (!socks[i].rx++) NLOG("[net] console %u -> pad %u (first, %d bytes)\n", ntohs(from.sin_port), socks[i].port, r);
            send_udp(ntohs(from.sin_port), socks[i].port, buf, r, PAD_IP, pad_mac[0] | pad_mac[1] | pad_mac[2] ? pad_mac : wifi_pad_mac());
        }
    }
    /* TCP: finish connects, pump host -> pad, reap idle connections */
    static uint8_t tbuf[1460];
    for (int i = 0; i < TCP_MAX; i++) {
        TcpConn *c = &tconns[i];
        if (c->fd < 0) continue;
        if (++c->idle > 4000) {                                /* ~10 s of net_poll rounds */
            NLOG("[net] tcp idle timeout pad %u -> %u\n", c->pad_port, c->dport);
            tcp_close(c); continue;
        }
        if (c->state == 0) {                                   /* nonblocking connect in progress */
            fd_set wf; FD_ZERO(&wf); FD_SET(c->fd, &wf);
            struct timeval tv = { 0, 0 };
            if (select(c->fd + 1, NULL, &wf, NULL, &tv) <= 0) continue;
            int err = 0; socklen_t el = sizeof err;
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) {
                NLOG("[net] tcp connect pad %u -> %u failed: %s\n", c->pad_port, c->dport, strerror(err));
                send_tcp(c->dport, c->pad_port, c->next, c->exp, 0x14, NULL, 0);   /* RST|ACK */
                tcp_close(c); continue;
            }
            c->state = 1;
            NLOG("[net] tcp established pad %u -> console %u\n", c->pad_port, c->dport);
            continue;
        }
        if (c->state == 2) continue;                           /* closing: FIN already sent to pad */
        for (int k = 0; k < 32; k++) {
            int r = recv(c->fd, tbuf, sizeof tbuf, 0);
            if (r == 0) {                                      /* host closed: FIN to pad */
                send_tcp(c->dport, c->pad_port, c->next, c->exp, 0x11, NULL, 0);   /* FIN|ACK */
                c->next++; c->state = 2;
                NLOG("[net] tcp fin console %u -> pad %u\n", c->dport, c->pad_port);
                break;
            }
            if (r < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) { tcp_close(c); }
                break;
            }
            send_tcp(c->dport, c->pad_port, c->next, c->exp, 0x18, tbuf, r);        /* PSH|ACK */
            c->next += r;
        }
    }
}
