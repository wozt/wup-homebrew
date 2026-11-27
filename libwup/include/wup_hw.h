/* wup_hw: the GamePad's screen brightness, rumble motor and microphone, through the kernel. Shared by the menu and
 * libwup. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus                                   /* (C++ apps: these are C) */
extern "C" {
#endif


/* the backlight, 0..100 % of its PWM range (0: dark). Nintendo's firmware uses only the levels of the pad's own table
   (its power chip's EEPROM: 21 45 77 122 183 255); the panel takes any. */
void hw_backlight(int percent);          /* 0..100 */
int hw_backlight_percent(int level);     /* one of the table's levels (1..6) as a percentage, to the nearest 10 */
int hw_backlight_default(void);          /* the level Nintendo's mode uses (the EEPROM's) as a percentage, or -1 if unknown */
void hw_screen_power(int on);            /* the LCD's power off (0) and on (1) */

/* the rumble motor */
void hw_rumble(int on);

/* the microphone: 16-bit mono, 48 kHz; read at least every 20 ms */
void hw_mic(int on);
int hw_mic_read(int16_t *out, int max);  /* samples recorded since the last read (up to max) */

#ifdef __cplusplus
}
#endif
