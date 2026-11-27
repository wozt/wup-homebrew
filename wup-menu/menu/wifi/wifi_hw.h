/* wifi_hw: the GamePad's radio, through the kernel (see wifi_hw.c) */
#pragma once
/* The radio's country (the kernel sets it): it decides the 5 GHz channels scanned and joined. "US" has 36-165 ("DE",
   up to 1.0.4, stopped at 140: issues #4, #20, #1). Kept in the menu's settings record */
#define WIFI_COUNTRY "DE"
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef struct { char ssid[33]; u8 ssid_len, bssid[6]; int channel, rssi, secured; } wifi_net;
extern void (*wifi_wait)(int ms);                 /* how to wait (default: a plain sleep) */
int wifi_bringup(void);                           /* 0 = interface up */
int wifi_set_country(const char *cc);             /* the kernel's next bring-up (2 letters; applies from the next up) */
int wifi_scan(wifi_net *out, int max);            /* 5 GHz networks, strongest first; < 0 = error */
int wifi_join(const wifi_net *net, const u8 *pmk);/* pmk = NULL for an open network */
int wifi_associated(const wifi_net *net);
int wifi_leave(void);                             /* off the network it is on */
int wifi_rssi(void);
int wifi_mac(u8 *mac);
void wpa_pmk(const char *pass, const u8 *ssid, u32 ssid_len, u8 out[32]);   /* PBKDF2-HMAC-SHA1, 4096 */
