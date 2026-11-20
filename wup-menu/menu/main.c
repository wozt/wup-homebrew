/* The Homebrew Menu: installed apps (/apps/<name>/<name>.wup), Settings (Wi-Fi, file sharing, brightness, boot),
 * the Netloader (wuplink) and Developer Mode. An ordinary libwup app with LVGL; the loader starts it from the
 * menu slot (boot/loader.c) and again whenever an app ends. Starting an app is wup_exec() and returning. */
#include <stdio.h>
#include <string.h>
#include "menu.h"
#include "dbgsrv.h"
#include "ftpd.h"
#include "netshare.h"
#include "wup_hw.h"
#include "wupfmt.h"

WUP_APP("Homebrew Menu");
menu_t M;
static int radio_ready, net_started, leaving;
static int dbg_open, ftp_open;
void wdbg(const char *s, int v) { printf("[wifi] %s %d\n", s, v); }

/* ---- settings */
void menu_save(void) { int r = config_save(&M.cfg); printf("[menu] settings saved %d\n", r); }
/* the file sharing login: /config/sharing {"SHR1", login on, MD5("pad:GamePad:<password>")}. The password itself is
   never stored; FTP refuses all access to /config. */
typedef struct { uint32_t magic; uint8_t auth, pad[3]; uint8_t hash[16]; } share_cfg_t;
static share_cfg_t share_cfg;
#define SHR1 0x31524853u
static void share_load(void) {
    if (fs_read("/config/sharing", 0, &share_cfg, sizeof share_cfg) != (int)sizeof share_cfg || share_cfg.magic != SHR1) memset(&share_cfg, 0, sizeof share_cfg);
    M.share_pass_set = share_cfg.magic == SHR1;
    M.share_auth = M.share_pass_set && share_cfg.auth;
}
void menu_share_save(void) {
    share_cfg.auth = (uint8_t)M.share_auth;
    if (M.share_pass_set) { share_cfg.magic = SHR1; lfs_mkdir(&fs_lfs, "/config"); fs_write("/config/sharing", &share_cfg, sizeof share_cfg); }
    ftpd_set_auth(M.share_auth && M.share_pass_set, share_cfg.hash);
}
void menu_share_password(const char *pw) { ftpd_hash(pw, share_cfg.hash); M.share_pass_set = 1; M.share_auth = 1; menu_share_save(); }
/* network storage (/net): /config/netfs, the record every app reads (include/netfs.h) */
static netfs_cfg_t nfs_cfg;
static void nfs_load(void) {
    memset(&nfs_cfg, 0, sizeof nfs_cfg);
    if (fs_read(NETFS_PATH, 0, &nfs_cfg, sizeof nfs_cfg) != (int)sizeof nfs_cfg || nfs_cfg.magic != NETFS_MAGIC) memset(&nfs_cfg, 0, sizeof nfs_cfg);
}
netfs_cfg_t *menu_netfs(void) { return &nfs_cfg; }
void menu_netfs_save(void) {
    nfs_cfg.magic = NETFS_MAGIC;
    lfs_mkdir(&fs_lfs, "/config");
    fs_write(NETFS_PATH, &nfs_cfg, sizeof nfs_cfg);
}
void menu_refresh_info(void) {
    static char fw[32], st[48];
    uint32_t v = 0;
    if (!fs_flash_read(0x100070, &v, 4)) snprintf(fw, sizeof fw, "%08x (rev 7677)", (unsigned)v);     /* Product1's VER_ entry */
    else snprintf(fw, sizeof fw, "Unknown");
    if (M.fs_ok) {
        unsigned size = fs_size_kb(), used = fs_used_kb(), fr = size > used ? size - used : 0;
        if (size >= 1024) snprintf(st, sizeof st, "%u.%u MB free of %u MB", fr / 1024, fr % 1024 * 10 / 1024, (size + 512) / 1024);
        else snprintf(st, sizeof st, "%u KB free of %u KB", fr, size);
    } else snprintf(st, sizeof st, "Not available");
    M.firmware = fw; M.storage = st;
}

/* ---- the network: the radio, the saved network in the background, the services */
static void ip_text(void) {
    uint32_t ip = net_started ? wup_net_ip() : 0;
    if (ip) snprintf(M.ip, sizeof M.ip, "%u.%u.%u.%u", (unsigned)(ip & 255), (unsigned)(ip >> 8 & 255), (unsigned)(ip >> 16 & 255), (unsigned)(ip >> 24));
    else M.ip[0] = 0;
}
void menu_poll(void) {
    if (!net_started) return;
    ip_text();
    int on = M.ip[0] != 0;
    if (on && !M.cfg.debug_off && !dbg_open) { dbg_open = !dbgsrv_open(); printf("[menu] debug server on tcp 28802\n"); }
    if (dbg_open && M.cfg.debug_off) { dbgsrv_close(); dbg_open = 0; }
    if (on && !M.cfg.share_off && M.fs_ok && !ftp_open) { ftp_open = !ftpd_open(); menu_share_save(); printf("[menu] file sharing (FTP) on tcp 21\n"); }
    if (ftp_open && M.cfg.share_off) { ftpd_close(); ftp_open = 0; }
    clock_network(on);
    if (dbg_open) dbgsrv_step();
    if (ftp_open) ftpd_step();
    ns_step();
}
void menu_wait(int ms) {
    for (int t = 0; t < ms && !leaving; t += 10) { wup_lvgl_run(10); menu_poll(); }
}
int menu_radio(void) {
    if (radio_ready) return 0;
    int p = wifi_patch_radio(), b = p >= 0 ? wifi_bringup("DE") : -1;
    wdbg("patch", p); wdbg("bringup", b);
    if (p < 0 || b) return -1;
    radio_ready = 1;
    return 0;
}
static void net_start(void) {                      /* our stack takes the radio's frames: before joining, so none reach Nintendo's idle one */
    if (net_started) return;
    uint8_t mac[6] __attribute__((aligned(4)));
    wifi_mac(mac);
    net_started = !wup_net_start(mac);
}
const char *menu_connect(const wifi_net *n, const char *pass) {
    static uint8_t pmk[32] __attribute__((aligned(4)));
    char l[64];
    const char *err = 0;
    (void)l;
    if (n->secured) { menu_wait(50); wpa_pmk(pass, (const uint8_t *)n->ssid, n->ssid_len, pmk); }
    menu_wait(50);
    net_start();
    if (wifi_join(n, n->secured ? pmk : 0)) err = "Couldn't Join";
    int ok = 0;
    for (int i = 0; !err && i < 24 && !ok; i++) { menu_wait(1000); ok = wifi_associated(n); }
    if (!err && !ok) err = "Couldn't Join";
    if (!err) { wup_net_dhcp(); for (int i = 0; i < 20 && !M.ip[0]; i++) menu_wait(1000); }
    if (!err && !M.ip[0]) err = n->secured ? "Incorrect Password" : "No Address";
    if (!err) {                                        /* remember the network: its name and the derived key, never the password */
        M.cfg.wifi_saved = 1; M.cfg.secured = (uint8_t)n->secured; M.cfg.ssid_len = n->ssid_len;
        memcpy(M.cfg.ssid, n->ssid, 33);
        memcpy(M.cfg.pmk, pmk, 32); if (!n->secured) memset(M.cfg.pmk, 0, 32);
        memcpy(M.cfg.country, "DE", 3);
        menu_save();
    }
    memset(pmk, 0, sizeof pmk);
    return err;
}
/* the saved network forgotten: off it, its address given back, nothing kept of it (its name, its key) */
static int aj_state;
void menu_forget(void) {
    aj_state = 0;
    if (radio_ready) wifi_leave();
    if (net_started) wup_net_leave();
    M.cfg.wifi_saved = 0; M.cfg.secured = 0; M.cfg.ssid_len = 0; memset(M.cfg.ssid, 0, sizeof M.cfg.ssid); memset(M.cfg.pmk, 0, sizeof M.cfg.pmk);
    menu_save();
    M.ip[0] = 0;
}
/* the saved network, in steps from the main loop (the menu stays usable meanwhile) */
static uint32_t aj_ticks; static wifi_net aj_net;
static void autojoin_step(void) {
    switch (aj_state) {
    case 1:
        if (menu_radio()) { aj_state = 0; return; }
        memcpy(aj_net.ssid, M.cfg.ssid, 33); aj_net.ssid_len = M.cfg.ssid_len; aj_net.secured = M.cfg.secured;
        net_start();
        wdbg("autojoin", wifi_join(&aj_net, M.cfg.secured ? M.cfg.pmk : 0));
        aj_state = 2; aj_ticks = 0;
        return;
    case 2:                                            /* associated? (checked once a second) */
        if (++aj_ticks % 100) return;
        if (wifi_associated(&aj_net)) { wup_net_dhcp(); aj_state = 3; aj_ticks = 0; wdbg("autojoin associated", 0); }
        else if (aj_ticks > 3000) {
            aj_state = 0; wdbg("autojoin gave up", 0);
            static char t[56]; snprintf(t, sizeof t, "Couldn't Join \"%s\"", M.cfg.ssid); wup_notice(t, 0);
        }
        return;
    case 3:
        if (M.ip[0]) { aj_state = 0; wdbg("autojoin has an address", 1); }
        else if (++aj_ticks > 3000) { aj_state = 0; wdbg("autojoin no address", 0); }
        return;
    }
}

/* ---- starting and installing apps */
const char *menu_start(const char *path) {
    volatile wup_handoff_t *pb = WUP_HANDOFF;
    ns_abort();                                       /* a /net test or browse in progress ends with the menu */
    if (wup_exec(path, 0)) return "It may be damaged or made for another version";
    pb->flags = (radio_ready ? WUP_HO_RADIO_READY : 0) | (M.ip[0] ? WUP_HO_WIFI_JOINED : 0);
    pb->menu_sel = (uint32_t)ui_selected();
    pb->light = (uint8_t)(M.cfg.brightness * 10 + 1);
    pb->ui = (uint8_t)(WUP_UI_SET | (M.cfg.light ? 0 : WUP_UI_DARK) | (M.cfg.sound_off ? WUP_UI_QUIET : 0) | (clock_24h() ? WUP_UI_24H : 0));
    for (int i = 0; i < (int)sizeof pb->menu_version; i++) pb->menu_version[i] = i < (int)sizeof MENU_VERSION ? MENU_VERSION[i] : 0;
    pb->net_ip = pb->net_mask = pb->net_gw = pb->net_dns = 0;
    if (M.ip[0]) {                                     /* the live connection for the app (libwup's wup_net_init) */
        uint32_t ip, mask, gw, dns; wup_net_info(&ip, &mask, &gw, &dns);
        uint8_t mac[6] __attribute__((aligned(4))); wifi_mac(mac);
        pb->net_ip = ip; pb->net_mask = mask; pb->net_gw = gw; pb->net_dns = dns;
        for (int i = 0; i < 6; i++) pb->net_mac[i] = mac[i];
        pb->flags |= WUP_HO_NET_INFO;
    }
    leaving = 1;
    return 0;
}
/* an app's folder name from its title, as the .wup name the tools give it: "Game Boy" -> "game_boy" */
static void slug(char *o, const char *name) {
    int k = 0;
    for (int i = 0; name[i] && i < 31; i++) { char c = name[i]; o[k++] = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : c >= 'A' && c <= 'Z' ? (char)(c + 32) : '_'; }
    if (!k) o[k++] = 'a';
    o[k] = 0;
}
const char *menu_install_received(int run) {
    wup_hdr_t w; char s[32], path[96];
    if (fs_read(WUPLINK_STREAM_PATH, 0, &w, WUP_HDR_V2) != WUP_HDR_V2 || w.magic != WUP_MAGIC) { fs_remove(WUPLINK_STREAM_PATH); return "This is not a GamePad app"; }
    w.name[31] = 0; slug(s, w.name);
    snprintf(path, sizeof path, "/apps/%s", s); lfs_mkdir(&fs_lfs, path);
    snprintf(path, sizeof path, "/apps/%s/%s.wup", s, s);
    fs_remove(path);
    if (lfs_rename(&fs_lfs, WUPLINK_STREAM_PATH, path)) { fs_remove(WUPLINK_STREAM_PATH); return "The app could not be stored on the GamePad"; }
    apps_scan();
    return run ? menu_start(path) : 0;
}

/* Reset: the apps and their data, the clock and file sharing settings (the files) and the menu's settings (Wi-Fi too)
   go. The menu, the firmware and the system's own apps stay: App Store and Software Update, with their records, so
   that a reset pad can still get apps and updates. Files are removed, not the flash under them erased block by block
   (that took minutes with a few MB stored). The GamePad is then turned off by hand: the next start is a new pad's.
   < 0 if something could not be removed. */
static const char *const kept[] = { "/apps/app_store", "/apps/software_update", "/.get/packages/app_store", "/.get/packages/software_update", "/.get/bundled" };
static int is_kept(const char *p) {
    for (unsigned i = 0; i < sizeof kept / sizeof kept[0]; i++) { size_t n = strlen(kept[i]); if (!strncmp(p, kept[i], n) && (!p[n] || p[n] == '/')) return 1; }
    return 0;
}
static int wipe(const char *dir, int depth, void (*tick)(void)) {    /* everything in dir but what is kept; < 0: could not */
    static char path[8][192];
    if (depth >= 8) return -1;
    for (int skip = 0;;) {                             /* (one entry at a time: a folder is not walked while it changes) */
        lfs_dir_t d; struct lfs_info inf; int found = 0, n = 0;
        if (lfs_dir_open(&fs_lfs, &d, dir)) return -1;
        while (lfs_dir_read(&fs_lfs, &d, &inf) > 0) {
            if (!strcmp(inf.name, ".") || !strcmp(inf.name, "..")) continue;
            if (n++ < skip) continue;
            snprintf(path[depth], sizeof path[0], "%s%s%s", dir, dir[1] ? "/" : "", inf.name); found = 1; break;
        }
        lfs_dir_close(&fs_lfs, &d);
        if (!found) return 0;
        if (is_kept(path[depth])) { skip++; continue; }
        if (inf.type == LFS_TYPE_DIR) wipe(path[depth], depth + 1, tick);
        if (lfs_remove(&fs_lfs, path[depth])) skip++;      /* (a folder with something kept in it stays) */
        if (tick) tick();
    }
}
int menu_reset(void (*tick)(void)) {
    wuplink_close();
    if (ftp_open) { ftpd_close(); ftp_open = 0; }
    if (dbg_open) { dbgsrv_close(); dbg_open = 0; }
    sound_off();
    int r = M.fs_ok ? wipe("/", 0, tick) : -1;
    if (r) r = fs_erase(tick) ? -1 : 0;                /* (no filesystem to walk: a new, empty one) */
    r |= config_erase() ? -2 : 0;
    printf("[menu] reset %d\n", r);
    return r;
}

int main(void) {
    wup_screen_hold();                                 /* (it always presents its first screen: fade in with that, not before) */
    const wup_handoff_t *pb = wup_handoff();
    int resumed = pb && pb->runs > 0;
    if (resumed && (pb->flags & WUP_HO_RADIO_READY)) radio_ready = 1;      /* patched and up already: never twice */
    M.fs_ok = wup_fs_ok();
    memset(&M.cfg, 0, sizeof M.cfg);
    M.cfg.light = 1;                                   /* new pads: light, Developer Mode off, file sharing on */
    M.cfg.debug_off = 1;
    int have = config_load(&M.cfg);
    if (!M.cfg.bright10) {                             /* an older menu's level (1..5), or none yet: the pad's own */
        int p = M.cfg.brightness >= 1 && M.cfg.brightness <= 5 ? hw_backlight_percent(M.cfg.brightness) : hw_backlight_default();
        M.cfg.brightness = (uint8_t)(p >= 10 ? p / 10 : p >= 0 ? 1 : 7); M.cfg.bright10 = 1;
    }
    if (M.cfg.brightness < 1 || M.cfg.brightness > 10) M.cfg.brightness = M.cfg.brightness ? 7 : 1;   /* (10..100 %: never dark) */
    hw_backlight(M.cfg.brightness * 10);
    if (have && M.cfg.wifi_saved) aj_state = 1;
    share_load();
    nfs_load();
    clock_load();
    M.setup = !resumed && bundle_pending();            /* the Wii U install's apps are to be put on the pad: after the
                                                          menu has faded in, behind "Setting Up..." (ui.c, bundle.c) */
    apps_scan();
    sys_power(&M.power);
    wup_lvgl_init();
    wifi_wait = menu_wait;                             /* (bringing the radio up and scanning take seconds) */
    ui_start(resumed ? (int)pb->menu_sel : 0);
    if (!resumed) sound_chime();                       /* the pad has just started: the chime (not after an app) */
    while (!leaving) {
        wup_lvgl_run(10);
        autojoin_step();
        menu_poll();
        ui_step();
    }
    sound_off();                                       /* (the chime's thread, the audio) */
    wuplink_close();
    if (ftp_open) ftpd_close();
    if (dbg_open) dbgsrv_close();
    return 0;
}
