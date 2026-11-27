/* wifi_hw: the GamePad's Broadcom BCM4319 radio, through the kernel's driver (libwup include/wupk.h). Its start
 * (wifi_up) loads the radio's firmware from Nintendo's bank already patched (the WPA2 OUI, the key rotation), sets the
 * country (US: 5 GHz channels 36-165), legacy rates and power saving off, and brings the interface up; it also wakes a
 * radio Nintendo's standby left asleep. The menu's scan is Broadcom's ioctls, through the kernel. */
#include "wifi_hw.h"
#include "wupk.h"
void (*wifi_wait)(int ms);
static void DLY(int ms) { if (wifi_wait) wifi_wait(ms); else WUPK->sleep(ms); }
static u8 iobuf[0x2000 + 64] __attribute__((aligned(32)));
static int ioctl_(u32 cmd, const void *data, u32 dlen, u32 n, int set) {
    if (n > 0x2000) return -100;
    __builtin_memset(iobuf, 0, n); if (data) __builtin_memcpy(iobuf, data, dlen < n ? dlen : n);
    return WUPK->wifi_ioctl(cmd, iobuf, n, set);
}
static int radio_ok, scan_err;
int wifi_scan_err(void) { return scan_err; }        /* the last wifi_scan's error (0: none, -2..: see wifi_hw.c) */
int wifi_bringup(void) {
    if (radio_ok) return 0;
    int r = WUPK->wifi_up();
    if (!r) radio_ok = 1;
    return r;
}
int wifi_scan(wifi_net *out, int max) {
    u8 p[68] __attribute__((aligned(4))); __builtin_memset(p, 0, sizeof p);
    for (int i = 0; i < 6; i++) p[36 + i] = 0xff;
    p[42] = 2;
    for (int i = 0; i < 4; i++) *(int *)(p + 44 + 4 * i) = -1;
    int r = ioctl_(50 /* WLC_SCAN */, p, sizeof p, sizeof p, 1);
    if (r) { scan_err = r < 0 ? r : -r; return scan_err; }
    for (int i = 0; i < 35; i++) DLY(200);         /* (the scan visits every channel: ~7 s) */
    /* WLC_SCAN_RESULTS can answer -14 (not ready) just after the scan ends: ask again, as the kernel's own scan does.
       The kernel's ioctl cap is 1536 bytes (wifi.c: m[16+1536]): asking for 8 KB is refused with -100. */
    enum { SCANBUF = 1536 };
    u32 hdr[3] = { SCANBUF, 0, 0 };
    int e = -1;
    for (int t = 0; t < 50; t++) {
        e = ioctl_(51 /* WLC_SCAN_RESULTS */, hdr, 12, SCANBUF, 0);
        if (e != -14) break;
        DLY(20);
    }
    if (e) { scan_err = e < 0 ? e : -e; return -1; }
    scan_err = 0;
    u32 buflen = *(u32 *)iobuf, count = *(u32 *)(iobuf + 8), off = 12;
    if (buflen > SCANBUF) buflen = SCANBUF;
    int n = 0;
    for (u32 i = 0; i < count && off + 80 <= buflen; i++) {
        u8 *b = iobuf + off; u32 blen = *(u32 *)(b + 4);
        u32 sl = b[18] > 32 ? 32 : b[18];
        int ch = *(u16 *)(b + 72) & 0xff, rssi = *(int16_t *)(b + 78), sec = (*(u16 *)(b + 16) & 0x10) != 0;
        if (sl && ch >= 36) {
            int j;
            for (j = 0; j < n; j++) if (out[j].ssid_len == sl && !__builtin_memcmp(out[j].ssid, b + 19, sl)) break;
            if (j == n && n < max) { n++; out[j].rssi = -200; }
            if (j < n && rssi > out[j].rssi) {
                __builtin_memcpy(out[j].ssid, b + 19, sl); out[j].ssid[sl] = 0; out[j].ssid_len = (u8)sl;
                __builtin_memcpy(out[j].bssid, b + 8, 6); out[j].channel = ch; out[j].rssi = rssi; out[j].secured = sec;
            }
        }
        if (!blen) break;
        off += blen;
    }
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && out[j].rssi > out[j - 1].rssi; j--) { wifi_net t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return n;
}
int wifi_join(const wifi_net *net, const u8 *pmk) { return WUPK->wifi_join(net->ssid, pmk); }
int wifi_associated(const wifi_net *net) { (void)net; return WUPK->wifi_state() == 2; }   /* (2: joined, the key in place) */
int wifi_leave(void) { WUPK->wifi_leave(); return 0; }
int wifi_rssi(void) { int v = 0; ioctl_(127 /* WLC_GET_RSSI */, 0, 0, 4, 0); v = *(int *)iobuf; return v; }
int wifi_mac(u8 *mac) { __builtin_memcpy(mac, WUPK->mac, 6); return 0; }
