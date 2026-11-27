/* Wi-Fi: the Broadcom BCM4319 on SDIO (sdio.c), brought up as the stock firmware brings it up (the sequence traced
 * from it in the emulator: wifi_spec; LVC's dhd driver), then driven through Broadcom's own protocol: SDPCM frames on SDIO
 * function 2, carrying CDC commands ("ioctls") on channel 0, events on 1, Ethernet frames on 2.
 *
 * The radio runs Broadcom's firmware, which the host downloads at every start: read from Nintendo's firmware bank and
 * patched as it goes (radio.c: the WIFI section + the NVRAM text, the WPA2 OUI and the key rotation fixed).
 *
 * Backplane: SDIO function 1 reaches the chip's internal bus through a 32 KB window (F1 0x1000A-C = address bits 15-31);
 * F1 address (a & 0x7FFF) | 0x8000 is a 32-bit access. Cores (Sonics backplane, 4 KB each from 0x18000000; id in
 * sbidhigh +0xFFC bits 4-15; +0xF98 sbtmstatelow: bit 0 reset, 1 reject, 16 clock, 17 force gated clock). */
#include "os.h"

#define CLKCSR    0x1000e
#define PULLUP    0x1000f
#define WATERMARK 0x10008
#define CHIPBASE  0x18000000u
#define HC8(o)    (*(volatile u8 *)(0xe0010000u + (o)))   /* (the SD host, sdio.c) */

static u32 window = 0xffffffffu, arm, socram, sdcore, ramsize;
static int up;
u8 wifi_mac[6];
void (*wifi_rx_hook)(const u8 *eth, u32 len);              /* frames for the app's network stack (kapi.c) */
void (*wifi_link_hook)(int up);
static u8 mac_kept[6];                                      /* the chip's own MAC, once seen (its OTP can be missed after a partial reset) */
static void net_link(int up) { if (wifi_link_hook) wifi_link_hook(up); }
u32 wifi_seq(void);

static int set_window(u32 a) {
    u32 w = a & ~0x7fffu;
    if (w == window) return 0;
    int e = sdio_write8(1, 0x1000a, (u8)(w >> 8 & 0x80)) | sdio_write8(1, 0x1000b, (u8)(w >> 16)) | sdio_write8(1, 0x1000c, (u8)(w >> 24));
    window = e ? 0xffffffffu : w;
    return e;
}
static u32 rd(u32 a) {
    u32 v = 0xffffffffu;
    int e = set_window(a);
    if (!e) e = sdio_rw(0, 1, (a & 0x7fff) | 0x8000, 1, &v, 4, 64);
    if (e) klog("wifi: backplane read %08x failed (%x, %s)\n", a, e, sdio_state());
    return v;
}
static void wr(u32 a, u32 v) {
    if (set_window(a) || sdio_rw(1, 1, (a & 0x7fff) | 0x8000, 1, &v, 4, 64)) klog("wifi: backplane write %08x failed\n", a);
}
/* bytes into chip RAM (2 KB pieces in 64-byte blocks, the rest in byte mode: the stock driver's chunking) */
static int ram_write(u32 a, const u8 *p, u32 n) {
    static u32 buf[512];
    while (n) {
        u32 k = 2048 - (a & 2047);
        if (k > n) k = n;
        u32 blk = k & ~63u, tail = k - blk;
        memcpy(buf, p, k);
        if (set_window(a)) return -1;
        if (blk && sdio_rw(1, 1, (a & 0x7fff) | 0x8000, 1, buf, blk, 64)) return -2;
        if (tail && sdio_rw(1, 1, ((a + blk) & 0x7fff) | 0x8000, 1, (u8 *)buf + blk, tail, 64)) return -3;
        a += k; p += k; n -= k;
    }
    return 0;
}

static int clk_wait(u8 want, u32 us) {
    u8 v = 0;
    for (u32 t = time_us(); time_us() - t < us; ) if (!sdio_read8(1, CLKCSR, &v) && (v & want)) return v;
    if (us > 50000) klog("wifi: clock bit %02x never came (CSR %02x)\n", want, v);
    return -1;
}

/* Cores. Two kinds of backplane: Sonics (bus type 0, what the emulator models: per-core state in the core's own last
   bytes, +0xF98 sbtmstatelow...) and AXI (bus type 1: the pad's BCM4319, chip id 0x16914319, found 2026-11-19).
   On AXI the cores are listed in an enumeration ROM (chipcommon +0xFC points at it), and each has a wrapper with
   +0x408 ioctrl (bit 0 clock, 1 force gated clock) and +0x800 resetctrl (bit 0 in reset): Broadcom's documented
   scheme (as in Linux's brcmfmac chip.c). */
static int axi;
static u32 arm_w, socram_w, socram_rev;

static void core_disable(u32 c, u32 w) {
    if (axi) {                                             /* (the stock driver's ai_core_disable, LVC 0x3d1ba: traced) */
        if (rd(w + 0x800) & 1) return;
        wr(w + 0x408, 0); rd(w + 0x408); delay_ms(1);
        wr(w + 0x800, 1); delay_ms(1);
        return;
    }
    u32 s = rd(c + 0xf98);
    if (s & 1) return;                                     /* (already in reset) */
    wr(c + 0xf98, 0x10002);                                /* clock, reject */
    for (u32 t = time_us(); (rd(c + 0xf9c) & 4) && time_us() - t < 10000; ) ;   /* (busy) */
    wr(c + 0xf98, 0x30003);
    rd(c + 0xf98);
    delay_us(10);
    wr(c + 0xf98, 0x00003);
}
static void core_reset(u32 c, u32 w) {
    core_disable(c, w);
    if (axi) {                                             /* (ai_core_reset, LVC 0x3d224: clock forced on while the
                                                              reset lifts, 1 ms, then the clock alone, 1 ms: traced) */
        wr(w + 0x408, 3); rd(w + 0x408);
        wr(w + 0x800, 0); delay_ms(1);
        wr(w + 0x408, 1); rd(w + 0x408); delay_ms(1);
        return;
    }
    wr(c + 0xf98, 0x30001); rd(c + 0xf98);
    if (rd(c + 0xf9c) & 1) wr(c + 0xf9c, 0);              /* (SError) */
    u32 im = rd(c + 0xf90);
    if (im & 0x60000) wr(c + 0xf90, im & ~0x60000u);       /* (inband error, timeout) */
    wr(c + 0xf98, 0x30000); rd(c + 0xf98);
    wr(c + 0xf98, 0x10000); rd(c + 0xf98);
}

/* the cores we need, from the enumeration ROM: component descriptors (type 1: part number bits 8-19; the second
   word: revision bits 24-31), then address descriptors (type 5; bit 3: a high word
   follows; bits 4-5 = 3: a size descriptor follows; bits 6-7: 0 slave, 2 slave wrapper, 3 master wrapper; bits 8-11
   the port), the table ending with 0xF */
static void erom_scan(void) {
    u32 p = rd(CHIPBASE + 0xfc);
    for (int guard = 0; guard < 512; guard++) {
        u32 a = rd(p); p += 4;
        if ((a & 15) == 15) break;
        if ((a & 15) != 1) continue;
        u32 b = rd(p); p += 4;
        if ((b & 15) != 1) continue;
        u32 id = a >> 8 & 0xfff, rev = b >> 24, base = 0, wrap = 0;   /* (the revision is in the second word) */
        for (;;) {
            u32 d = rd(p);
            if ((d & 15) == 1 || (d & 15) == 15) break;   /* (the next core, or the end) */
            p += 4;
            if ((d & 7) != 5) continue;                    /* (master ports and the rest) */
            if (d & 8) p += 4;
            if ((d >> 4 & 3) == 3) { u32 sz = rd(p); p += 4; if (sz & 8) p += 4; }
            u32 type = d >> 6 & 3, port = d >> 8 & 15, addr = d & 0xfffff000u;
            if (type == 0 && port == 0 && !base) base = addr;
            if ((type == 2 || type == 3) && !wrap) wrap = addr;
        }
        if (id == 0x82a) { arm = base; arm_w = wrap; }
        else if (id == 0x80e) { socram = base; socram_w = wrap; socram_rev = rev; }
        else if (id == 0x829) sdcore = base;
    }
}

/* chip RAM: the SOCRAM core's coreinfo (older revisions: banks of 2^(bsz+14), plus a last smaller one; newer: per
   bank, +0x10 index, +0x40 info) */
static u32 socram_size(void) {
    u32 ci = rd(socram), nb = ci >> 4 & 15;
    if (!axi || socram_rev <= 7 || socram_rev == 12 || socram_rev > 40) {   /* (newer: only for a sane revision;
                                                             a wrong register read wedges the backplane) */
        u32 lss = ci >> 20 & 15, sz = (lss ? nb - 1 : nb) * (1u << ((ci & 15) + 14));
        if (lss) sz += 1u << (lss - 1 + 14);
        return sz;
    }
    u32 sz = 0;
    for (u32 i = 0; i < nb; i++) { wr(socram + 0x10, i); sz += ((rd(socram + 0x40) & 0x7f) + 1) * 8192; }
    return sz;
}

/* ---- SDPCM: frames on function 2 */
static u8 txf[8448] __attribute__((aligned(4)));       /* an ioctl's frame is 16 + its buffer: scan results ask 8 KB */
static u8 rxf[16384 + 64] __attribute__((aligned(4)));
static u8 seq;
static u16 reqid;

static u8 tx_max = 4;                                  /* the radio's credit: the last sequence number it takes */
static u8 tx_fc;                                       /* the radio's flow control (header byte 4): bit 0 = hold data */
static int credit(void) { u8 d = (u8)(tx_max - seq); return d && !(d & 0x80); }   /* (control frames need it too) */
static int data_ok(void) { return credit() && !(tx_fc & 1); }

static int frame_send(int chan, const void *p, u32 n) {
    u32 off = chan == 2 ? 14 : 12;                         /* (data: 2 pad bytes, as the stock driver sends them) */
    u32 len = off + n, sz = (len + 3) & ~3u;
    if (sz > 64) sz = (sz + 63) & ~63u;
    if (sz > sizeof txf) return -1;
    memset(txf, 0, sz);
    txf[0] = (u8)len; txf[1] = (u8)(len >> 8); txf[2] = (u8)~len; txf[3] = (u8)(~len >> 8);
    txf[4] = seq; txf[5] = (u8)chan; txf[7] = (u8)off;
    memcpy(txf + off, p, n);
    int e = sdio_rw(1, 2, 0x8000, 0, txf, sz, 64);
    if (e) return e;
    seq++;                                                 /* (only for a frame the radio got: a number it never saw
                                                              leaves it waiting for that frame for ever, brcmfmac too
                                                              counts on success only; 2026-11-24, at 1000 sends/s) */
    return 0;
}

typedef void (*event_fn)(u32 type, u32 status, const u8 *data, u32 len);
static event_fn on_event;
static void link_event(u32 type, u32 status, u32 flags);
static u8 *ctl_out; static u32 ctl_max; static int ctl_got, ctl_status, ctl_waiting;

u32 wifi_chan_frames[4];
u32 wifi_fc_seen, wifi_tx_drops;                        /* (frames that said "hold data"; sends dropped: a full queue) */
static void frame_in(const u8 *f, u32 len) {
    u32 chan = f[5] & 15, off = f[7];
    wifi_chan_frames[chan & 3]++;
    tx_max = f[9]; tx_fc = f[8]; if (tx_fc) wifi_fc_seen++;   /* (no brcmfmac clamp: here a window "ahead by over
                                                              64" means we are past it, after a wake-up command; the
                                                              clamp sent into a full radio: 2026-11-24) */
    if (off < 12 || off > len) return;
    const u8 *p = f + off; u32 n = len - off;
    if (chan == 0 && n >= 16) {                            /* CDC reply: cmd, len, flags (id << 16), status */
        u32 flags = p[8] | p[9] << 8 | p[10] << 16 | (u32)p[11] << 24;
        if (!ctl_waiting || (flags >> 16) != reqid) return;   /* (a late reply to a command that already timed out:
                                                              dropped. Copied into its long-gone buffer, it once wrote
                                                              0x14E46C77 over the stack: a crash, 2026-11-20) */
        ctl_status = (int)(p[12] | p[13] << 8 | p[14] << 16 | (u32)p[15] << 24);
        if (ctl_out) memcpy(ctl_out, p + 16, n - 16 < ctl_max ? n - 16 : ctl_max);
        ctl_got = 1;
    } else if (chan == 1 || chan == 2) {                   /* BDC header (4 + 4 * its offset), then Ethernet. On
                                                              channel 1 a Broadcom event is Ethertype 0x886C; anything
                                                              else there is an ordinary packet (Broadcom's dhd treats
                                                              both channels as data: dropping them lost TCP and ARP) */
        u32 h = 4 + p[3] * 4u;
        if (n < h + 14) return;
        const u8 *e = p + h;
        if (e[12] == 0x88 && e[13] == 0x6c) {
            if (n < h + 14 + 10 + 48) return;
            const u8 *m = e + 14 + 10;
            u32 type = (u32)m[4] << 24 | m[5] << 16 | m[6] << 8 | m[7], status = (u32)m[8] << 24 | m[9] << 16 | m[10] << 8 | m[11];
            u32 dl = (u32)m[20] << 24 | m[21] << 16 | m[22] << 8 | m[23];
            if (m + 48 + dl > f + len) dl = 0;
            if (on_event) on_event(type, status, m + 48, dl);
            link_event(type, status, (u32)m[2] << 8 | m[3]);
        } else if (wifi_rx_hook) wifi_rx_hook(e, n - h);  /* (the app's network stack queues it) */
    }
}

static void tx_flush(void);

/* every frame the radio has waiting (a 32-byte read gives the length, the rest follows) */
u32 wifi_rx_frames, wifi_tx_frames, wifi_tx_stalls;
static int more;                                           /* (frames left when the last poll stopped) */
static int poll_frames(void) {
    u32 st = rd(sdcore + 0x20);
    if (st == 0xffffffffu) return -1;
    if (st) wr(sdcore + 0x20, st);
    if (st & 0x80) { u32 mb = rd(sdcore + 0x4c); wr(sdcore + 0x40, 2); (void)mb; }   /* mailbox: acknowledge */
    if (!(st & 0x40) && !more) return 0;                   /* (the flag comes once for a whole burst: what a poll
                                                              leaves is read the next time, flag or not) */
    more = 1;
    for (int i = 0; i < 32; i++) {
        if (sdio_rw(0, 2, 0x8000, 0, rxf, 32, 64)) return -2;
        u32 len = rxf[0] | rxf[1] << 8, chk = rxf[2] | rxf[3] << 8;
        if (!len || (len ^ chk) != 0xffff) { more = 0; break; }
        wifi_rx_frames++;
        if (len > sizeof rxf - 64) { klog("wifi: frame of %u bytes dropped\n", len); break; }
        if (len > 32) {
            u32 rest = len - 32;
            rest = rest > 64 ? (rest + 63) & ~63u : (rest + 3) & ~3u;
            if (sdio_rw(0, 2, 0x8000, 0, rxf + 32, rest, 64)) return -3;
        }
        frame_in(rxf, len);
    }
    return 0;
}

/* (the main loop, every frame) frames and events the radio has for us */
void wifi_poll(void) { if (up) { poll_frames(); tx_flush(); } }

/* Frames out (BDC header: version 2, no offset), by the radio's rules (Broadcom's brcmfmac, sdio.c): nothing goes out
   without credit (tx_max, in the header of every frame the radio sends us), control frames neither; data waits while
   the radio's flow-control bit is set; and the sender never waits. A frame without credit waits in a small queue and
   goes out from wifi_poll as credit comes; a full queue drops the newest (UDP is lossy; TCP sends again). Waiting here
   stopped receiving, and probing with a command without credit stalled the radio for half a second at a time
   (2026-11-24: the microphone at a third of its rate in a stream). */
#define TXQ 16
static struct { u16 len; u8 d[4 + 1600]; } txq[TXQ];
static u32 txq_r, txq_w, blocked_us;
static int send_now(u8 *b, u32 len) { b[0] = 0x20; b[1] = b[2] = b[3] = 0; int e = frame_send(2, b, 4 + len); if (!e) wifi_tx_frames++; return e; }
static void tx_flush(void) {
    while (txq_r != txq_w && data_ok()) { if (send_now(txq[txq_r % TXQ].d, txq[txq_r % TXQ].len)) break; txq_r++; }
    if (txq_r == txq_w) { blocked_us = 0; return; }
    if (!blocked_us) blocked_us = time_us();
    more = 1;                                              /* (the credit comes in frames: read them, flag or not) */
    /* Stuck: this radio firmware (Nintendo's build) can sit at no credit for good, frames or not; a command wakes it,
       and its answer carries the credit (2026-11-19, and again 2026-11-24 in a stream). Sent without waiting for the
       answer (it is read like any frame and dropped), after 3 ms stuck, at most every 3 ms (20 ms left the radio stuck
       much of the time in a stream: the microphone at a fifth of its rate). */
    static u32 probe_us;
    if (!credit() && time_us() - blocked_us > 3000 && time_us() - probe_us > 3000) {
        static u8 m[16 + 4];
        u32 cmd = 0, len = 4, flags = (u32)++reqid << 16;  /* WLC_GET_MAGIC, a get */
        memcpy(m, &cmd, 4); memcpy(m + 4, &len, 4); memcpy(m + 8, &flags, 4); memset(m + 12, 0, 8);
        if (!frame_send(0, m, sizeof m)) wifi_tx_stalls++;
        probe_us = time_us();
    }
}
int wifi_tx_room(void) { return (int)(TXQ - (txq_w - txq_r)); }   /* frames the send queue takes now */
int wifi_tx(const void *eth, u32 len) {
    if (!up || len > 1600) return -1;
    if (txq_r == txq_w && data_ok()) { static u8 b[4 + 1600]; memcpy(b + 4, eth, len); if (!send_now(b, len)) return 0; }
    if (txq_w - txq_r >= TXQ) { wifi_tx_drops++; return -3; }
    memcpy(txq[txq_w % TXQ].d + 4, eth, len); txq[txq_w % TXQ].len = (u16)len; txq_w++;
    if (!blocked_us) blocked_us = time_us();
    more = 1;
    return 0;
}

/* one CDC command: cmd, the buffer (len bytes; a get's answer comes back into it), set or get. Returns its status. */
int wifi_ioctl(u32 cmd, void *buf, u32 len, int set) {
    static u8 m[16 + 8192];                              /* scan results need kilobytes: 1536 truncated the list
                                                           (a wanted network past the cut was never seen) */
    if (len > sizeof m - 16) return -100;
    reqid++;
    u32 flags = (u32)reqid << 16 | (set ? 2 : 0);
    memcpy(m, &cmd, 4); memcpy(m + 4, &len, 4); memcpy(m + 8, &flags, 4); memset(m + 12, 0, 4);
    memcpy(m + 16, buf, len);
    ctl_out = set ? NULL : buf; ctl_max = len; ctl_got = 0; ctl_waiting = 1;
    int r;
    for (u32 t = time_us(); !credit(); ) {                 /* (a command needs credit too) */
        more = 1;
        if (poll_frames() < 0) { ctl_waiting = 0; return -102; }
        if (time_us() - t > 1000000) { klog("wifi: no credit for ioctl %u\n", cmd); ctl_waiting = 0; ctl_out = NULL; return -105; }
    }
    if (frame_send(0, m, 16 + len)) r = -101;
    else for (u32 t = time_us(); ; ) {
        if (ctl_got) { r = ctl_status; break; }
        if (poll_frames() < 0) { r = -102; break; }
        if (time_us() - t > 1000000) { klog("wifi: ioctl %u timed out\n", cmd); r = -103; break; }
    }
    ctl_waiting = 0; ctl_out = NULL;                       /* (whatever comes later is not for anyone) */
    return r;
}
/* "name" iovars: the name, a zero, the value */
int wifi_var(const char *name, void *v, u32 n, int set) {
    static u8 b[256];
    u32 l = strlen(name) + 1;
    if (l + n > sizeof b) return -104;
    memcpy(b, name, l); memcpy(b + l, v, n);
    int e = wifi_ioctl(set ? 263 : 262, b, set ? l + n : (l + n < 128 ? 128 : l + n), set);
    if (!e && !set) memcpy(v, b, n);
    return e;
}
static int var32(const char *name, u32 v) { return wifi_var(name, &v, 4, 1); }

/* what the card reports before the bring-up (the log: a radio in an unexpected state shows here first) */
static void card_state(void) {
    u8 c[8] = { 0 }, v = 0;
    for (int i = 0; i < 8; i++) sdio_read8(0, (u32)i, &c[i]);
    sdio_read8(1, CLKCSR, &v);
    klog("wifi: CCCR %02x %02x %02x %02x %02x %02x %02x %02x, clock CSR %02x\n", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], v);
}

/* ---- bring-up: the stock driver's sequence (dhd, LVC 0x3d000-0x48000; traced in the emulator on a model of the pad's
   own chip). Facts from the pad, 2026-11-19: the chip is AXI (id 0x16914319: cores from the enumeration ROM); touching
   a core that is not there, or one that is disabled, wedges the backplane until a power cycle; the card keeps its
   state across our restarts (sdio_init aborts and resets it); and the radio's boot ROM starts the image only if the
   CRC-32 it carries matches (tools/mkwifi.py fixes it after the WPA2 patch). */
int wifi_up(void) {
    if (up) return 0;
    u32 hdr[3] = { 0 }, fwlen, nvlen;                      /* "WFW1", firmware length, NVRAM length: streamed from
                                                              flash in pieces (no 250 KB in memory) */
    int size = radio_size();
    if (size < 12 || radio_read(0, hdr, 12) || memcmp(hdr, "WFW1", 4)) {
        klog("wifi: no radio firmware in flash (tools/mkwifi.py, then install it)\n"); return -1;
    }
    fwlen = hdr[1]; nvlen = hdr[2];
    if (12 + fwlen + nvlen != (u32)size || nvlen % 4 || nvlen > 4096) { klog("wifi: the radio firmware in flash is damaged\n"); return -4; }
    u8 *pkg = NULL;
    int e = sdio_init();
    if (e) { klog("wifi: SDIO failed (%d)\n", e); return -5; }
    {   /* Nintendo's standby (wake on WLAN) leaves the radio in deep sleep with its pads isolated (DEVICE_CTL 0x08):
           its backplane unreachable until the bit is cleared. The stock driver's first step (bcmsdh_attach: "resetting
           iso bit"), before anything else touches the radio */
        u8 dc = 0;
        if (!sdio_read8(1, 0x10009, &dc) && (dc & 0x08)) { sdio_write8(1, 0x10009, 0); klog("wifi: radio woken from standby (pads isolated)\n"); }
    }
    window = 0xffffffffu;
    int restarted = 0;
again:;
    card_state();
    u32 id = rd(CHIPBASE);
    klog("wifi: chip %04x rev %u, %s backplane\n", id & 0xffff, id >> 16 & 15, id >> 28 == 1 ? "AXI" : "Sonics");
    for (u32 t = time_us(), n = 0; (e = sdio_write8(1, CLKCSR, 0x28)); n++) {   /* ALP requested, the hardware clock
                                                              request forced off. (After a restart with the firmware
                                                              running, the chip resets itself and is deaf for a moment) */
        if (time_us() - t > 300000) {                     /* (a firmware left running can put the SDIO side to
                                                              sleep: single-byte commands time out while block reads
                                                              still work, 2026-11-19. The chip reset by its watchdog,
                                                              through a block write, as below) */
            if (restarted) { klog("wifi: the radio does not answer (%x)\n", e); goto fail; }
            wr(CHIPBASE + 0x80, 1);
            delay_ms(100);
            restarted = 1;
            klog("wifi: chip reset (it did not answer)\n");
            if ((e = sdio_init())) { klog("wifi: SDIO failed after the reset (%d)\n", e); goto fail; }
            window = 0xffffffffu;
            goto again;
        }
        delay_ms(10);
        (void)n;
    }
    if (clk_wait(0x40, 50000) < 0) {                       /* after a watchdog reset the power unit keeps the
                                                              firmware's resource masks (it is always on): ALP is no
                                                              longer on by itself, so it is requested with the hardware
                                                              clock request left on, then the stock values again */
        klog("wifi: no ALP: PMU state %08x, min %08x, max %08x: the cold-start masks back\n", rd(CHIPBASE + 0x60c),
             rd(CHIPBASE + 0x618), rd(CHIPBASE + 0x61c));
        wr(CHIPBASE + 0x61c, 0x2663e); wr(CHIPBASE + 0x618, 0x2663e);   /* (the masks a cold radio has: measured) */
        sdio_write8(1, CLKCSR, 0x08);
        if (clk_wait(0x40, 1000000) < 0) goto fail;
        sdio_write8(1, CLKCSR, 0x28);
        if (clk_wait(0x40, 50000) < 0) goto fail;
    }
    sdio_write8(1, CLKCSR, 0x21);                          /* ALP forced */
    sdio_write8(1, PULLUP, 0);
    arm = socram = sdcore = 0;
    axi = (id >> 28) == 1;
    if (axi) erom_scan();
    else for (u32 i = 0, n = id >> 24 & 15; i < (n ? n : 8); i++) {   /* (Sonics: the emulator's model) */
        u32 c = CHIPBASE + i * 0x1000, cid = rd(c + 0xffc) >> 4 & 0xfff;
        if (cid == 0x82a) arm = c; else if (cid == 0x80e) socram = c; else if (cid == 0x829) sdcore = c;
    }
    if (!arm || !socram || !sdcore) { klog("wifi: cores missing\n"); goto fail; }
    if (axi && !(rd(arm_w + 0x800) & 1) && !restarted) {   /* the firmware still running from an earlier start: the
                                                              whole chip reset by its watchdog (chipcommon +0x80), then
                                                              set up from the start. (A partial reset left its OTP
                                                              unread: the NVRAM's placeholder MAC, 2026-11-19) */
        wr(CHIPBASE + 0x80, 1);
        delay_ms(100);
        restarted = 1;
        klog("wifi: chip reset\n");
        e = sdio_init();
        if (e) { klog("wifi: SDIO failed after the reset (%d)\n", e); goto fail; }
        window = 0xffffffffu;
        goto again;
    }
    core_disable(arm, arm_w);
    core_reset(socram, socram_w);
    ramsize = socram_size();
    core_disable(socram, socram_w);                        /* (as the stock driver leaves it: and no register of a
                                                              disabled core is touched after this) */
    if (axi) {                                             /* (chipcommon rev >= 20 / 21: GPIO pull-ups and pull-downs
                                                              off, corecontrol bit 2) */
        wr(CHIPBASE + 0x58, 0); wr(CHIPBASE + 0x5c, 0);
        wr(CHIPBASE + 0x08, rd(CHIPBASE + 0x08) | 4);
    }
    if (!ramsize || ramsize > 0x200000) goto fail;
    wr(sdcore + 0x00, rd(sdcore + 0x00) | 2);              /* (an SDIO reset resets the backplane) */
    sdio_write8(0, 0x02, 0x02);
    sdio_write8(1, CLKCSR, 0);

    sdio_write8(1, CLKCSR, 0x08);                          /* the download, on ALP */
    if (clk_wait(0x40, 50000) < 0) goto fail;
    core_disable(arm, arm_w);
    core_reset(socram, socram_w);
    wr(ramsize - 4, 0);
    u32 t0 = time_us();
    static u8 piece[4096];
    for (u32 o = 0; o < fwlen; o += sizeof piece) {
        u32 k = fwlen - o < sizeof piece ? fwlen - o : sizeof piece;
        if (radio_read(12 + o, piece, k) || ram_write(o, piece, k)) { klog("wifi: download failed at %u\n", o); goto fail; }
    }
    if (radio_read(12 + fwlen, piece, nvlen) || ram_write(ramsize - 4 - nvlen, piece, nvlen)) {
        klog("wifi: NVRAM download failed\n"); goto fail;
    }
    u32 words = nvlen / 4;
    wr(ramsize - 4, (~words << 16) | (words & 0xffff));    /* the NVRAM's length, as the firmware looks for it */
    klog("wifi: firmware in (%u KB RAM, %u ms)\n", ramsize >> 10, (time_us() - t0) / 1000);
    wr(sdcore + 0x20, 0xffffffffu);
    core_reset(arm, arm_w);                                /* the radio's CPU starts the firmware */

    sdio_write8(1, CLKCSR, 0);
    sdio_write8(1, CLKCSR, 0x10);                          /* HT: once the firmware runs (~ms) */
    int v = clk_wait(0x80, 3000000);
    if (v < 0) { klog("wifi: the firmware did not start (last RAM word %08x)\n", rd(ramsize - 4)); goto fail; }
    sdio_write8(1, CLKCSR, (u8)(v | 2));
    wr(sdcore + 0x48, 4u << 16);                           /* SDPCM protocol version 4 */
    sdio_write8(0, 0x02, 0x06);                            /* F1 and F2 */
    u8 rdy = 0;
    for (u32 t = time_us(); !(rdy & 4); ) {
        if (sdio_read8(0, 0x03, &rdy)) rdy = 0;
        if (time_us() - t > 3000000) { klog("wifi: F2 never ready (%02x)\n", rdy); goto fail; }
    }
    wr(sdcore + 0x24, 0x200000f0);
    sdio_write8(1, WATERMARK, 8);
    sdio_write8(1, CLKCSR, 0xd0);
    u32 st = 0;
    for (u32 t = time_us(); !(st & 0x80); ) {              /* the firmware's "ready" */
        st = rd(sdcore + 0x20);
        if (time_us() - t > 3000000) { klog("wifi: no ready from the radio (status %08x)\n", st); goto fail; }
    }
    rd(sdcore + 0x4c);
    wr(sdcore + 0x20, st); wr(sdcore + 0x40, 2);

    static u8 b[128];
    memset(b, 0, sizeof b);
    if (wifi_var("cur_etheraddr", b, 6, 0)) goto fail;
    static const u8 placeholder[6] = { 0x00, 0x90, 0x4c, 0x14, 0x4a, 0xab };   /* (the NVRAM's macaddr) */
    static const u8 none[6];
    if (memcmp(b, placeholder, 6)) {                       /* the chip's own (OTP): kept, for starts that miss it */
        memcpy(mac_kept, b, 6);
    } else if (memcmp(mac_kept, none, 6) && !wifi_var("cur_etheraddr", mac_kept, 6, 1)) {
        memcpy(b, mac_kept, 6);                            /* (after a partial reset the firmware misses its OTP) */
    }
    memcpy(wifi_mac, b, 6);
    u8 ev[16];
    memset(ev, 0, sizeof ev);
    wifi_var("event_msgs", ev, 16, 0);
    ev[0] |= 0xeb; ev[1] |= 0x1f; ev[2] |= 0xbb; ev[3] |= 0x04; ev[4] |= 0x50; ev[5] |= 0x40;   /* (the stock mask: incl.
                                                             SCAN_COMPLETE 26, LINK 16, PSK_SUP 46) */
    wifi_var("event_msgs", ev, 16, 1);
    var32("roam_off", 1);
    /* before the first UP (the CFW's bring-up, proven on the pad: an earlier homebrew Wi-Fi driver): the country, legacy
       rates (this 20 MHz radio drops 40/80 MHz networks as join candidates with 802.11n on, and its ROM faults on HT
       rates), infrastructure, no power saving */
    static u8 cc[12];
    memset(cc, 0, sizeof cc); memcpy(cc, "DE", 2); cc[4] = cc[5] = cc[6] = cc[7] = 0xff; memcpy(cc + 8, "DE", 2);
    wifi_var("country", cc, 12, 1);
    var32("nmode", 0);
    u32 one = 1;
    wifi_ioctl(20, &one, 4, 1);                            /* SET_INFRA */
    var32("mpc", 0);
    if (wifi_ioctl(2, &one, 4, 1)) { klog("wifi: WLC_UP failed\n"); return -7; }    /* WLC_UP */
    var32("a_mrate", 12);                                  /* 5 GHz rates pinned: 6 Mbit/s multicast, 54 unicast */
    var32("a_rate", 108);
    u32 pm = 0;
    wifi_ioctl(86, &pm, 4, 1);                             /* WLC_SET_PM: power saving off (on by default: the radio
                                                              dozes between beacons, so frames waited up to ~1 s and the
                                                              send credit ran out, 2026-11-19) */
    up = 1;
    klog("wifi: up, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", b[0], b[1], b[2], b[3], b[4], b[5]);
    return 0;
fail:
    if (pkg) kfree(pkg);
    return -6;
}

/* ---- scanning */
static int scan_done;
static void scan_event(u32 type, u32 status, const u8 *d, u32 n) { (void)d; (void)n; if (type == 26) scan_done = (int)status + 1; }

/* the networks in range, strongest first: ssid, channel, signal (dBm), secured */
int wifi_scan(wifi_net_t *out, int max) {
    if (!up && wifi_up()) return -1;
    static u8 p[64];
    memset(p, 0, sizeof p);
    memset(p + 36, 0xff, 6);                               /* any BSSID */
    p[42] = 2;                                             /* any BSS type */
    for (int i = 44; i < 60; i += 4) { p[i] = p[i + 1] = p[i + 2] = p[i + 3] = 0xff; }   /* firmware defaults */
    static u8 r[1536];
    u32 bl = sizeof r;
    int e = 0;
    for (int attempt = 0; attempt < 3; attempt++) {        /* (just after start-up the results can stay "not ready"
                                                              (-14): the scan is run again) */
        scan_done = 0;
        on_event = scan_event;
        e = wifi_ioctl(50, p, 64, 1);                      /* WLC_SCAN */
        if (e) {                                           /* (-25: a scan still running: it ends by itself) */
            on_event = NULL; klog("wifi: scan refused (%d)\n", e);
            for (u32 t = time_us(); time_us() - t < (e == -25 ? 2000000u : 200000u); ) poll_frames();
            continue;
        }
        for (u32 t = time_us(); !scan_done && time_us() - t < 8000000; ) poll_frames();
        on_event = NULL;
        if (!scan_done) { klog("wifi: the scan never finished\n"); e = -3; continue; }
        for (u32 t = time_us(); ; ) {                      /* WLC_SCAN_RESULTS */
            memset(r, 0, sizeof r); memcpy(r, &bl, 4);
            e = wifi_ioctl(51, r, sizeof r, 0);
            if (e != -14 || time_us() - t > 1000000) break;
            delay_ms(20);
        }
        if (!e) break;
    }
    if (e) { klog("wifi: no scan results (%d)\n", e); return -4; }
    u32 count, total; memcpy(&total, r, 4); memcpy(&count, r + 8, 4);
    int n = 0;
    for (u32 o = 12, i = 0; i < count && o + 80 <= sizeof r && n < max; i++) {
        const u8 *b = r + o;
        u32 len; memcpy(&len, b + 4, 4);
        if (len < 80 || o + len > sizeof r) break;
        wifi_net_t *w = &out[n];
        u32 sl = b[18] > 32 ? 32 : b[18];
        memcpy(w->ssid, b + 19, sl); w->ssid[sl] = 0;
        memcpy(w->bssid, b + 8, 6);
        w->channel = b[72];
        w->rssi = (s16)(b[78] | b[79] << 8);
        w->secure = (b[16] & 0x10) != 0;
        if (w->ssid[0]) n++;
        o += len;
    }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && out[j].rssi > out[j - 1].rssi; j--) {
        wifi_net_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
    }
    klog("wifi: %u networks (results %u bytes)\n", n, total);
    return n;
}

/* ---- joining: the stock firmware's connect commands (LVC 0x1d1ea, wifi_spec): infrastructure, open system, the
   cipher, WPA2-PSK, the radio's own supplicant ("sup_wpa": it does the 4-way handshake), the passphrase
   (WLC_SET_WSEC_PMK, flag 1: the radio derives the key), the SSID. Then events: LINK (16, flag bit 0 up) and, with a
   password, PSK_SUP (46) status 6 (keyed). */
int wifi_state;                                            /* WIFI_OFF, _JOINING, _CONNECTED, _FAILED */
char wifi_ssid[33];
static int linked, keyed, secured, ptt_due;
static u32 join_t0;

u32 wifi_events[128];
static void link_event(u32 type, u32 status, u32 flags) {
    if (type < 128) wifi_events[type]++;
    if (wifi_state == WIFI_OFF) return;
    if (wifi_state == WIFI_JOINING && type != 26 && type != 82) klog("wifi: event %u status %u flags %x\n", type, status, flags);
    if (type == 16) {                                      /* LINK */
        linked = flags & 1;
        if (!linked && wifi_state == WIFI_CONNECTED) { wifi_state = WIFI_JOINING; join_t0 = ticks_ms; net_link(0); klog("wifi: link down\n"); }
    } else if (type == 46 && status == 6) keyed = 1;       /* PSK_SUP: keyed */
    else if (type == 46 && status != 6 && status != 0 && status != 2 && status != 3 && status != 4 && status != 5)
        klog("wifi: supplicant status %u\n", status);
    else if ((type == 6 || type == 12) && wifi_state == WIFI_CONNECTED) {   /* DEAUTH_IND, DISASSOC_IND */
        wifi_state = WIFI_JOINING; join_t0 = ticks_ms; linked = keyed = 0; net_link(0); klog("wifi: disconnected\n");
    }
    if (wifi_state == WIFI_JOINING && linked && (keyed || !secured)) {
        wifi_state = WIFI_CONNECTED;
        klog("wifi: connected to %s\n", wifi_ssid);
        net_link(1);
        ptt_due = 1;                                       /* (the radio command: from wifi_tick, not from in here) */
    }
}

/* join ssid: pmk = the WPA2 key (wpa_pmk: the menu keeps it), NULL for an open network */
int wifi_join(const char *ssid, const u8 *pmk) {
    if (!up && wifi_up()) return -1;
    u32 n = strlen(ssid);
    if (!n || n > 32) return -2;
    static u8 last_pmk[32];
    if (wifi_state == WIFI_CONNECTED && !strcmp(wifi_ssid, ssid) && secured == (pmk != NULL) &&
        (!pmk || !memcmp(last_pmk, pmk, 32))) {           /* already on it, with this key: nothing to do. (The radio
                                                             stays connected from app to app; joining again would wait
                                                             for a key exchange the network does not redo, and then
                                                             take the connection for a wrong password) */
        klog("wifi: already connected to %s\n", ssid);
        return 0;
    }
    if (pmk) memcpy(last_pmk, pmk, 32);
    strcpy(wifi_ssid, ssid);
    secured = pmk != NULL; linked = keyed = 0;
    u32 v = 0;
    wifi_ioctl(22, &v, 4, 1);                              /* SET_AUTH: open system */
    v = secured ? 4 : 0; wifi_ioctl(134, &v, 4, 1);        /* SET_WSEC: AES */
    v = secured ? 0x80 : 0; wifi_ioctl(165, &v, 4, 1);     /* SET_WPA_AUTH: WPA2-PSK */
    var32("sup_wpa", secured ? 1 : 0);
    if (secured) {
        static u8 k[68];
        memset(k, 0, sizeof k);
        memcpy(k + 4, pmk, 32);
        k[0] = 32;                                         /* length 32, flags 0: a key, not a passphrase */
        if (wifi_ioctl(268, k, sizeof k, 1)) { klog("wifi: the key was refused\n"); return -3; }
    }
    static u8 ss[36];
    memset(ss, 0, sizeof ss);
    memcpy(ss, &n, 4); memcpy(ss + 4, ssid, n);
    wifi_state = WIFI_JOINING; join_t0 = ticks_ms;
    int e = wifi_ioctl(26, ss, sizeof ss, 1);              /* SET_SSID: join */
    if (e) { wifi_state = WIFI_FAILED; klog("wifi: join refused (%d)\n", e); return -4; }
    klog("wifi: joining %s\n", ssid);
    return 0;
}

/* (the main loop) a join that never finishes fails; a lost one is tried again */
static int associated(void) {                              /* (the CFW's test: a BSSID, and our SSID) */
    static u8 b[36];
    memset(b, 0, 8);
    if (wifi_ioctl(23, b, 8, 0)) return 0;                 /* GET_BSSID */
    if (!(b[0] | b[1] | b[2] | b[3] | b[4] | b[5])) return 0;
    memset(b, 0, sizeof b);
    if (wifi_ioctl(25, b, 36, 0)) return 0;                /* GET_SSID */
    u32 n; memcpy(&n, b, 4);
    return n == strlen(wifi_ssid) && !memcmp(b + 4, wifi_ssid, n);
}

void wifi_tick(void) {
    static u32 last, assoc_t;
    if (ptt_due && wifi_state == WIFI_CONNECTED) {         /* the stock driver's "pendingtx_timer", on at connect (LVC
                                                              0x1b128, 0x1b1d8): Nintendo's radio firmware sends what it
                                                              holds on a timer; without it the send credit could stay 0 */
        static u32 ptt[4] = { 1, 5, 2, 0 };
        ptt_due = 0;
        klog("wifi: pendingtx_timer %d\n", wifi_var("pendingtx_timer", ptt, 16, 1));
    }
    if (wifi_state == WIFI_JOINING && ticks_ms - last > 500) {   /* (events or not: the association itself) */
        last = ticks_ms;
        if (associated()) {
            if (!assoc_t) { assoc_t = ticks_ms; klog("wifi: associated (link %d, keyed %d)\n", linked, keyed); }
            if (secured && !keyed && ticks_ms - assoc_t > 8000) {   /* associated, never keyed: the password */
                wifi_state = WIFI_BADPASS; assoc_t = 0;
                klog("wifi: the password for %s is wrong\n", wifi_ssid);
                u32 z = 0; wifi_ioctl(52, &z, 4, 1);           /* (WLC_DISASSOC) */
                return;
            }
            if (!secured || keyed) {
                wifi_state = WIFI_CONNECTED; assoc_t = 0;
                klog("wifi: connected to %s\n", wifi_ssid);
                ptt_due = 1;
                net_link(1);
            }
        }
    }
    if (wifi_state == WIFI_JOINING && ticks_ms - join_t0 > 20000) {
        klog("wifi: joining %s timed out (link %d, keyed %d)\n", wifi_ssid, linked, keyed);
        wifi_state = WIFI_FAILED;
    }
}

u32 wifi_seq(void) { return (u32)seq << 8 | tx_max; }

/* the link dropped (WLC_DISASSOC) */
void wifi_leave(void) {
    if (!up) return;
    u32 z = 0;
    wifi_ioctl(52, &z, 4, 1);
    wifi_state = WIFI_OFF; wifi_ssid[0] = 0;
    net_link(0);
}
