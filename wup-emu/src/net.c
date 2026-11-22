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

static void net_init(void) {
    inited = true;
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
}
