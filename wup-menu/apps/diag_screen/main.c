/* Diagnostic: proves the hook -> loader -> staged image chain works.
   Solid magenta screen + text, loops forever (never returns to stock). */
#include "wup.h"

WUP_APP("Diag Screen");

int main(void) {
    wup_screen();
    gfx_fill(0, 0, GFX_W, GFX_H, RGB(255, 0, 255));
    gfx_text_c(&font_big, GFX_W / 2, 220, "BOOT OK", RGB(255, 255, 255));
    gfx_text_c(&font_body, GFX_W / 2, 280, "hook+loader+image chain works", RGB(255, 255, 255));
    gfx_present_all();
    for (;;) wup_sleep(100);
    return 0;
}
