#pragma once
/* The menu's saved settings. The Wi-Fi key is the derived WPA2 PMK, never the password. */
typedef struct {
    unsigned char light : 1, sound_off : 1, bright10 : 1, icons : 3, : 2;   /* (one byte: the record's size is fixed, the loader checks it)
                                                         icons: the home icons' quality + 1 (0: never set: 32x32 256 colours) */
    unsigned char autoboot, wifi_saved, ssid_len;
    char ssid[33]; char country[3];
    unsigned char pmk[32];
    unsigned char secured, debug_off, share_off;      /* share_off: file sharing off */
    unsigned char brightness;                         /* bright10: the screen in tens of percent, 1..10; else an older menu's
                                                         level 1..5 (0: never set: the pad's own level) */
} config_t;
int config_load(config_t *c);        /* 1 = a valid record was found */
int config_save(const config_t *c);  /* 0 = written and verified */
int config_erase(void);              /* both records gone: the next start has the defaults (0 = done) */
