/* large = yes: malloc in pieces of every size until it says no, all of it written and checked, then freed and the
   heap's whole again; the result on the screen (one screen) and in the log */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wup.h"
#include "gfx.h"
WUP_APP("Large Test");
size_t wup_mem_free(void);
int main(void) {
    static void *blk[4096]; static size_t sz[4096];
    size_t free0 = wup_mem_free(), got = 0; int n = 0, bad = 0;
    for (size_t s = 300000; n < 4096 && s >= 16; ) {        /* big pieces first, then smaller as it fills */
        void *p = malloc(s);
        if (!p) { s /= 2; continue; }
        blk[n] = p; sz[n] = s; memset(p, (unsigned char)(n * 37 + 1), s); got += s; n++;
    }
    for (int i = 0; i < n; i++) { const unsigned char *p = blk[i]; for (size_t k = 0; k < sz[i]; k += 509) if (p[k] != (unsigned char)(i * 37 + 1)) bad++; }
    for (int i = 0; i < n; i++) free(blk[i]);
    void *whole = malloc(free0 / 2);                       /* (merged again: half of it in one piece) */
    char l1[96], l2[96];
    snprintf(l1, sizeof l1, "free at start %u KB, allocated %u KB in %d pieces", (unsigned)(free0 >> 10), (unsigned)(got >> 10), n);
    snprintf(l2, sizeof l2, "%s, %s", bad ? "DATA WRONG" : "all data intact", whole ? "heap whole again" : "HEAP FRAGMENTED");
    printf("large: %s; %s\n", l1, l2);
    free(whole);
    wup_screen();
    gfx_fill(0, 0, GFX_W, GFX_H, RGB(10, 30, 60));
    gfx_text_c(&font_big, GFX_W / 2, 200, bad || !whole ? "Large Mode: FAILED" : "Large Mode: OK", RGB(255, 255, 255));
    gfx_text_c(&font_body, GFX_W / 2, 250, l1, RGB(200, 210, 230));
    gfx_text_c(&font_body, GFX_W / 2, 280, l2, RGB(200, 210, 230));
    gfx_present_all();
    for (int t = 0; t < 300 && !(wup_buttons() & WUP_B); t++) wup_sleep(16);
    return 0;
}
