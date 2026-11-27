/* Diagnostic: the boot-stage markers — [runs, mark] at flash 0xB2000 + runs*0x1000 (libwup fs.c/big.c,
   menu/main.c) and the loader's own byte at 0xB8000. The last letter is the stage reached before a crash. */
#include <stdio.h>
#include <string.h>
#include "wup.h"
#include "fs.h"
#include "gfx.h"

WUP_APP("Diag Flash");

static int y;
static int ypos(void) { return y += 22; }
static void line(int yy, const char *s) { gfx_text(&font_body, 10, yy, s, RGB(255, 255, 255)); }

int main(void) {
    wup_screen();
    gfx_fill(0, 0, GFX_W, GFX_H, RGB(8, 8, 24));
    y = 10;
    char b[96];
    for (uint32_t s = 0; s < 4; s++) {                /* app markers: sector 0xB2000 + runs*0x1000 */
        uint8_t em[2] = {0};
        int er = fs_flash_read(0xB2000u + s * 0x1000u, em, 2);
        snprintf(b, sizeof b, "emark @%05x read=%d runs=%d mark=%02x '%c'", 0xB2000u + s * 0x1000u, er, em[0], em[1], (em[1] >= 32 && em[1] < 127) ? em[1] : '.');
        line(ypos(), b);
    }
    uint8_t em = 0;
    int er = fs_flash_read(0xB8000u, &em, 1);
    snprintf(b, sizeof b, "lmark @B8000 read=%d byte=%02x '%c'", er, em, (em >= 32 && em < 127) ? em : '.');
    line(ypos(), b);
    gfx_present_all();
    for (;;) wup_sleep(200);
    return 0;
}
