/* The Homebrew Menu: what its two halves share. main.c: settings, the network, the services and starting apps;
 * ui.c: the screens (LVGL). */
#pragma once
#include <stdint.h>
#include "wup_lvgl.h"
#include "config.h"
#include "sys.h"
#include "apps.h"
#include "wuplink.h"
#include "netfs.h"
#include "wifi/wifi_hw.h"

typedef struct {
    config_t cfg;                          /* saved settings (config.h) */
    int share_auth, share_pass_set;        /* file sharing: a password is required / one is set */
    char ip[20];                           /* the pad's address, "" while not connected */
    int rssi, fs_ok;
    power_t power;
    const char *firmware, *storage;        /* Settings > System */
} menu_t;
extern menu_t M;

/* main.c */
void menu_save(void);                      /* the settings to flash */
void menu_share_save(void);
void menu_share_password(const char *pw);
netfs_cfg_t *menu_netfs(void);             /* network storage settings (/config/netfs); edit then menu_netfs_save() */
void menu_netfs_save(void);
void menu_poll(void);                      /* the network and the services: called from every waiting loop */
void menu_wait(int ms);                    /* draw, handle input and menu_poll for ms (not from inside an LVGL event) */
int menu_radio(void);                      /* the radio patched and up: 0 = ready */
const char *menu_connect(const wifi_net *n, const char *pass);   /* NULL = connected (the list shows it: a checkmark) */
const char *menu_start(const char *wup_path);     /* start an app when the menu's loop ends; NULL = it will */
const char *menu_start_netapp(const char *remote, uint32_t stage_size, uint32_t stage_raw);   /* an app already staged from /net (its saves go to /save/net/<slug>/) */
/* clock.c */
typedef struct { const char *id, *name, *tz; } clock_zone_t;     /* tz: a POSIX TZ rule */
extern const clock_zone_t clock_zones[]; extern const int clock_nzones;
void clock_load(void), clock_network(int online), clock_set_zone(int i), clock_set_24h(int on);
int menu_reset(void (*tick)(void));    /* erase all content and settings (0 = done); tick: between sectors */
int clock_zone(void), clock_24h(void), clock_text(char *out, int n, int long_form);
void sound_init(void), sound_click(void), sound_off(void), sound_note(int notice);   /* sound.c */
void menu_forget(void);                /* the saved Wi-Fi network: left and forgotten */
const char *menu_install_received(int run);       /* a .wup from the Netloader into /apps; run: and start it */
void menu_refresh_info(void);              /* firmware and storage texts */

/* ui.c */
void ui_start(int selected);
void ui_step(void);                        /* once per loop: the buttons LVGL does not know (X, Y), deferred work, status */
int ui_selected(void);
