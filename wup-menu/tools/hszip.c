/* heatshrink compressor for the host: stdin -> stdout (tools/mkflash.py stores the menu's
 * WUP_BIGMEM blob with it; libwup/src/big.c inflates it on the pad). Window/lookahead must
 * match big.c's decoder (12/8). Build: cc -O2 -I<heatshrink> hszip.c heatshrink_encoder.c */
#include <stdio.h>
#include <stdlib.h>
#include "heatshrink_encoder.h"

static int pump(heatshrink_encoder *e, uint8_t *out, size_t outsz) {
    for (;;) {                                              /* poll until the encoder is empty */
        size_t on = 0;
        HSE_poll_res pr = heatshrink_encoder_poll(e, out, outsz, &on);
        if (pr < 0) return -1;
        if (on && fwrite(out, 1, on, stdout) != on) return -2;
        if (pr == HSER_POLL_EMPTY) return 0;
    }
}

int main(void) {
    size_t cap = 1 << 20, n = 0;
    uint8_t *in = malloc(cap);
    for (;;) {
        if (n == cap) { cap *= 2; in = realloc(in, cap); }
        size_t r = fread(in + n, 1, cap - n, stdin);
        n += r;
        if (!r) break;
    }
    heatshrink_encoder *e = heatshrink_encoder_alloc(12, 8);
    if (!e) return 1;
    uint8_t out[8192];
    size_t ip = 0;
    while (ip < n) {
        size_t sunk = 0;
        HSE_sink_res sr = heatshrink_encoder_sink(e, in + ip, n - ip, &sunk);
        if (sr < 0 || !sunk) {                              /* buffer not empty: drain first */
            if (pump(e, out, sizeof out)) return 3;
            sr = heatshrink_encoder_sink(e, in + ip, n - ip, &sunk);
            if (sr < 0 || !sunk) return 4;
        }
        ip += sunk;
        if (pump(e, out, sizeof out)) return 3;
    }
    for (;;) {                                              /* finish: drain whatever remains */
        if (pump(e, out, sizeof out)) return 5;
        if (heatshrink_encoder_finish(e) == HSER_FINISH_DONE) break;
    }
    heatshrink_encoder_free(e);
    free(in);
    return ferror(stdout) ? 6 : 0;
}
