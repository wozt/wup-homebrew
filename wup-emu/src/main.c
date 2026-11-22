#include "dev.h"
#include "lcd.h"
#include "uic.h"
#include "audio.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>

CPU cpu;
uint8_t *flash;
uint32_t flash_size;

#define CPU_HZ 108000000
#define FPS 60

static uint8_t *load_file(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { perror("read"); exit(1); }
    fclose(f);
    *size = (uint32_t)n;
    return b;
}

static void sdl_audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    audio_fill((int16_t *)stream, len / 4);
}

static void run_cycles(long long n) {
    const int slice = 1000;
    while (n > 0) {
        int k = cpu_run(&cpu, n < slice ? (int)n : slice);
        devices_tick(k);
        n -= k;
    }
}

static void write_bmp(const char *path, const uint32_t *px) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    uint32_t rowsz = LCD_W * 3 + ((4 - (LCD_W * 3) % 4) % 4), size = 54 + rowsz * LCD_H;
    uint8_t h[54] = { 'B', 'M' };
    *(uint32_t *)(h + 2) = size; *(uint32_t *)(h + 10) = 54; *(uint32_t *)(h + 14) = 40;
    *(int32_t *)(h + 18) = LCD_W; *(int32_t *)(h + 22) = LCD_H;
    *(uint16_t *)(h + 26) = 1; *(uint16_t *)(h + 28) = 24; *(uint32_t *)(h + 34) = rowsz * LCD_H;
    fwrite(h, 1, 54, f);
    uint8_t row[LCD_W * 3 + 4] = { 0 };
    for (int y = LCD_H - 1; y >= 0; y--) {
        for (int x = 0; x < LCD_W; x++) {
            uint32_t c = px[y * LCD_W + x];
            row[x * 3] = c; row[x * 3 + 1] = c >> 8; row[x * 3 + 2] = c >> 16;
        }
        fwrite(row, 1, rowsz, f);
    }
    fclose(f);
}

/* keyboard -> GamePad buttons */
static void handle_key(SDL_Keycode k, bool down) {
    UicInput *in = &uic_input;
    uint16_t b = 0; uint8_t e = 0;
    switch (k) {
    case SDLK_x: b = 0x8000; break;          /* A */
    case SDLK_z: b = 0x4000; break;          /* B */
    case SDLK_s: b = 0x2000; break;          /* X */
    case SDLK_a: b = 0x1000; break;          /* Y */
    case SDLK_LEFT: b = 0x0800; break;
    case SDLK_RIGHT: b = 0x0400; break;
    case SDLK_UP: b = 0x0200; break;
    case SDLK_DOWN: b = 0x0100; break;
    case SDLK_1: b = 0x0080; break;          /* ZL */
    case SDLK_2: b = 0x0040; break;          /* ZR */
    case SDLK_q: b = 0x0020; break;          /* L */
    case SDLK_w: b = 0x0010; break;          /* R */
    case SDLK_RETURN: b = 0x0008; break;     /* + */
    case SDLK_BACKSPACE: b = 0x0004; break;  /* - */
    case SDLK_h: b = 0x0002; break;          /* HOME */
    case SDLK_y: if (down) uic_press_special(UIC_MODE_PAIRING); return;   /* SYNC (back) */
    case SDLK_t: if (down) uic_press_special(UIC_MODE_TVREMOTE); return;  /* TV (front) */
    case SDLK_p: if (down) in->power_status |= 0x02; else in->power_status &= ~0x02; return; /* POWER */
    case SDLK_j: in->lx = down ? 0x100 : 0x800; return;
    case SDLK_l: in->lx = down ? 0xf00 : 0x800; return;
    case SDLK_i: in->ly = down ? 0xf00 : 0x800; return;
    case SDLK_k: in->ly = down ? 0x100 : 0x800; return;
    case SDLK_KP_4: in->rx = down ? 0x100 : 0x800; return;  /* right stick: keypad 8/4/2/6 */
    case SDLK_KP_6: in->rx = down ? 0xf00 : 0x800; return;
    case SDLK_KP_8: in->ry = down ? 0xf00 : 0x800; return;
    case SDLK_KP_2: case SDLK_KP_5: in->ry = down ? 0x100 : 0x800; return;
    case SDLK_u: e = 0x80; break;            /* L3 (left stick click) */
    case SDLK_o: e = 0x40; break;            /* R3 (right stick click) */
    case SDLK_LEFTBRACKET:  if (down) in->volume = in->volume > 0x10 ? in->volume - 0x10 : 0; return;   /* volume slider */
    case SDLK_RIGHTBRACKET: if (down) in->volume = in->volume < 0xef ? in->volume + 0x10 : 0xff; return;
    default: return;
    }
    if (down) { in->buttons |= b; in->extra_buttons |= e; }
    else { in->buttons &= ~b; in->extra_buttons &= ~e; }
}

/* SDL game controller -> GamePad (by position: SDL's bottom face button is the GamePad's B) */
static void handle_cbutton(int b, bool down) {
    UicInput *in = &uic_input;
    uint16_t m = 0; uint8_t e = 0;
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: m = 0x4000; break;              /* bottom -> B */
    case SDL_CONTROLLER_BUTTON_B: m = 0x8000; break;              /* right  -> A */
    case SDL_CONTROLLER_BUTTON_X: m = 0x1000; break;              /* left   -> Y */
    case SDL_CONTROLLER_BUTTON_Y: m = 0x2000; break;              /* top    -> X */
    case SDL_CONTROLLER_BUTTON_DPAD_UP: m = 0x0200; break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: m = 0x0100; break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: m = 0x0800; break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: m = 0x0400; break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: m = 0x0020; break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: m = 0x0010; break;
    case SDL_CONTROLLER_BUTTON_START: m = 0x0008; break;
    case SDL_CONTROLLER_BUTTON_BACK: m = 0x0004; break;
    case SDL_CONTROLLER_BUTTON_GUIDE: m = 0x0002; break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: e = 0x80; break;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: e = 0x40; break;
    case SDL_CONTROLLER_BUTTON_MISC1: if (down) uic_press_special(UIC_MODE_TVREMOTE); return;
    default: return;
    }
    if (down) { in->buttons |= m; in->extra_buttons |= e; }
    else { in->buttons &= ~m; in->extra_buttons &= ~e; }
}
static void handle_caxis(int a, int v) {
    UicInput *in = &uic_input;
    int s = 0x800 + v / 16;                  /* -32768..32767 -> 12 bit */
    if (s < 0) s = 0; if (s > 0xfff) s = 0xfff;
    int inv = 0xfff - s;                     /* SDL: up is negative; GamePad: up is high */
    switch (a) {
    case SDL_CONTROLLER_AXIS_LEFTX: in->lx = s; break;
    case SDL_CONTROLLER_AXIS_LEFTY: in->ly = inv; break;
    case SDL_CONTROLLER_AXIS_RIGHTX: in->rx = s; break;
    case SDL_CONTROLLER_AXIS_RIGHTY: in->ry = inv; break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  if (v > 16000) in->buttons |= 0x0080; else in->buttons &= ~0x0080; break;  /* ZL */
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: if (v > 16000) in->buttons |= 0x0040; else in->buttons &= ~0x0040; break;  /* ZR */
    }
}

typedef struct { const char *name; uint16_t b; uint8_t e; uint8_t p; } BtnName;
static const BtnName btn_names[] = {
    {"a",0x8000,0,0}, {"b",0x4000,0,0}, {"x",0x2000,0,0}, {"y",0x1000,0,0}, {"left",0x0800,0,0}, {"right",0x0400,0,0}, {"up",0x0200,0,0}, {"down",0x0100,0,0},
    {"zl",0x0080,0,0}, {"zr",0x0040,0,0}, {"l",0x0020,0,0}, {"r",0x0010,0,0}, {"plus",0x0008,0,0}, {"minus",0x0004,0,0}, {"home",0x0002,0,0}, {"sync",0x0001,0,0},
    {"tv",0,0x20,0}, {"l3",0,0x80,0}, {"r3",0,0x40,0}, {"power",0,0,0x02}, {"p01",0,0,0x01}, {"p80",0,0,0x80}, {"p04",0,0,0x04}, {"p40",0,0,0x40}, {NULL,0,0,0}
};
typedef struct { const BtnName *btn; long long start, end; } Hold;
static Hold holds[16]; static int nholds;
typedef struct { int x, y; long long start, end; } Tap;
static Tap taps[16]; static int ntaps;
static void set_touch_screen(float lx, float ly, bool down) {
    uic_input.touch_down = down;
    /* inverse of the firmware's default touch calibration:
       x = (rx - 195) * 814 / 3682 + 20,  y = (ry - 278) * -440 / 3445 + 460 */
    float rx = (lx - 20) * 3682.0f / 814.0f + 195, ry = (460 - ly) * 3445.0f / 440.0f + 278;
    if (rx < 0) rx = 0; if (rx > 4095) rx = 4095; if (ry < 0) ry = 0; if (ry > 4095) ry = 4095;
    uic_input.tx = (uint16_t)rx;
    uic_input.ty = (uint16_t)ry;
}
static void apply_holds(long long cyc) {
    bool any = false;
    for (int i = 0; i < ntaps; i++)
        if (cyc >= taps[i].start && cyc < taps[i].end) { set_touch_screen(taps[i].x, taps[i].y, true); any = true; }
    if (ntaps && !any) uic_input.touch_down = false;
    for (int i = 0; i < nholds; i++) {
        Hold *h = &holds[i];
        bool on = cyc >= h->start && cyc < h->end;
        if (on) { uic_input.buttons |= h->btn->b; uic_input.extra_buttons |= h->btn->e; uic_input.power_status |= h->btn->p; }
        else if (cyc >= h->end && cyc < h->end + 2000000) { uic_input.buttons &= ~h->btn->b; uic_input.extra_buttons &= ~h->btn->e; uic_input.power_status &= ~h->btn->p; }
    }
}

/* firmware files (dumps of your own pad, never in git): $WUP_FIRMWARE, ./firmware/, ../wup-firmware/ (next to this
   folder), ../firmware/ or the current folder */
const char *fw_path(const char *name) {
    static char buf[4][512], env[512]; static int n;
    const char *e = getenv("WUP_FIRMWARE");
    if (e && *e) snprintf(env, sizeof env, "%s/", e);
    const char *dirs[] = { env, "firmware/", "../wup-firmware/", "../firmware/", "" };
    for (int i = 0; i < 5; i++) {
        if (!*dirs[i] && i == 0) continue;
        char *p = buf[n & 3]; snprintf(p, 512, "%s%s", dirs[i], name);
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); n++; return p; }
    }
    return name;
}

int main(int argc, char **argv) {
    extern const char *fw_path(const char *name);
    const char *rom = fw_path("bootrom.bin"), *fl = fw_path("flash.bin"), *shot = NULL, *frames = NULL;
    long long max_cycles = -1;
    bool headless = false, realtime = false;
    int scale = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cycles") && i + 1 < argc) max_cycles = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--bootrom") && i + 1 < argc) rom = argv[++i];
        else if (!strcmp(argv[i], "--flash") && i + 1 < argc) fl = argv[++i];
        else if (!strcmp(argv[i], "--uart-boot")) { extern uint32_t boot_straps; boot_straps = 0; }   /* "UART boot" pad grounded */
        else if (!strcmp(argv[i], "--uart-pty")) setenv("UART_PTY", "1", 1);
        else if (!strcmp(argv[i], "--flash-writeback")) setenv("FLASH_WRITEBACK", "1", 1);
        else if (!strcmp(argv[i], "--mmio")) log_mmio = 1;
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--realtime")) realtime = true;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = argv[++i];   /* headless: the screen, 30 a second */
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--touch") && i + 4 < argc && ntaps < 16) {
            taps[ntaps].x = atoi(argv[++i]); taps[ntaps].y = atoi(argv[++i]);
            taps[ntaps].start = atoll(argv[++i]) * (CPU_HZ / 1000); taps[ntaps].end = atoll(argv[++i]) * (CPU_HZ / 1000);
            ntaps++;
        }
        else if (!strcmp(argv[i], "--hold") && i + 3 < argc && nholds < 16) {
            const char *n = argv[++i]; const BtnName *b = btn_names;
            while (b->name && strcmp(b->name, n)) b++;
            if (!b->name) { fprintf(stderr, "unknown button %s\n", n); return 1; }
            holds[nholds].btn = b;
            holds[nholds].start = atoll(argv[++i]) * (CPU_HZ / 1000);
            holds[nholds].end = atoll(argv[++i]) * (CPU_HZ / 1000);
            nholds++;
        }
        else if (!strcmp(argv[i], "--break") && i + 1 < argc) { extern uint32_t cpu_break_addr; cpu_break_addr = strtoul(argv[++i], NULL, 16); }
        else { fprintf(stderr, "usage: %s [--bootrom f] [--flash f] [--headless --cycles N --screenshot out.bmp --frames out.raw] [--scale N]\n", argv[0]); return 1; }
    }
    bus_init();
    uint32_t rs;
    uint8_t *r = load_file(rom, &rs);
    memcpy(bootrom, r, rs > 0x1000 ? 0x1000 : rs);
    flash = load_file(fl, &flash_size);
    if (getenv("FLASH_WRITEBACK")) {   /* map the image file shared: flash writes/erases land in it */
        int fd = open(fl, O_RDWR);
        void *m = fd >= 0 ? mmap(NULL, flash_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (m == MAP_FAILED) { perror("[flash] writeback mmap"); }
        else { free(flash); flash = m; fprintf(stderr, "[flash] writes go back to %s\n", fl); }
    }
    bus_map_rom(true);
    devices_init();
    cpu_reset(&cpu);

    static uint32_t fb[LCD_W * LCD_H];

    if (headless) {
        if (max_cycles < 0) max_cycles = 1000000000LL;
        struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
        /* --frames FILE: raw 854x480 frames (ffmpeg: -f rawvideo -pix_fmt bgr0 -s 854x480 -r 30), with AUDIO_WAV a video */
        FILE *ff = frames ? fopen(frames, "wb") : NULL; long long next_frame = 0;
        for (long long c = 0; c < max_cycles && !uic_powered_off; c += 1000000) {
            apply_holds(c); run_cycles(1000000); audio_discard();
            if (ff && c >= next_frame) { lcd_render(fb); fwrite(fb, 4, LCD_W * LCD_H, ff); next_frame += CPU_HZ / 30; }
            if (uic_reboot_request >= 0) {   /* UIC restarts the SoC in another mode (same as the GUI) */
                char v[16]; snprintf(v, sizeof v, "%d", uic_reboot_request);
                setenv("UIC_BOOTPARAM", v, 1);
                fprintf(stderr, "[uic] rebooting SoC into mode %s\n", v);
                execv(argv[0], argv); perror("execv"); return 1;
            }
            if (realtime) {   /* pace to wall-clock time (for talking to a live console over the network bridge) */
                struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
                double ahead = (double)c / CPU_HZ - ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9);
                if (ahead > 0.001) usleep((useconds_t)(ahead * 1e6));
            }
        }
        if (ff) fclose(ff);
        cpu_dump(&cpu);
        cpu_trace_dump();
        { extern void cpu_pcsample_dump(void); cpu_pcsample_dump(); }
        if (shot) { if (uic_powered_off) memset(fb, 0, sizeof fb); else lcd_render(fb); write_bmp(shot, fb); fprintf(stderr, "screenshot -> %s\n", shot); }
        if (getenv("RAMDUMP")) { FILE *f = fopen(getenv("RAMDUMP"), "wb"); fwrite(ram, 1, RAM_SIZE, f); fclose(f); }
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("Wii U GamePad (DRC) emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       LCD_W * scale, LCD_H * scale, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, LCD_W, LCD_H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, LCD_W, LCD_H);

    SDL_AudioSpec want = { 0 }, have;
    want.freq = 48000; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024; want.callback = sdl_audio_cb;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!adev) fprintf(stderr, "SDL audio: %s (continuing without sound)\n", SDL_GetError());
    else SDL_PauseAudioDevice(adev, 0);

    bool quit = false;
    uint64_t freq = SDL_GetPerformanceFrequency(), start = SDL_GetPerformanceCounter();
    long long emu_cycles = 0;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: quit = true; break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE) quit = true;
                else if (ev.key.keysym.sym == SDLK_F12) { lcd_render(fb); write_bmp("screenshot.bmp", fb); fprintf(stderr, "saved screenshot.bmp\n"); }
                else if (!ev.key.repeat) handle_key(ev.key.keysym.sym, true);
                break;
            case SDL_KEYUP: handle_key(ev.key.keysym.sym, false); break;
            case SDL_CONTROLLERDEVICEADDED: {
                SDL_GameController *gc = SDL_GameControllerOpen(ev.cdevice.which);
                if (gc) fprintf(stderr, "controller: %s\n", SDL_GameControllerName(gc));
                break;
            }
            case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
                handle_cbutton(ev.cbutton.button, ev.type == SDL_CONTROLLERBUTTONDOWN); break;
            case SDL_CONTROLLERAXISMOTION: handle_caxis(ev.caxis.axis, ev.caxis.value); break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONUP: {
                int mx, my; uint32_t st = SDL_GetMouseState(&mx, &my);
                float lx, ly; SDL_RenderWindowToLogical(ren, mx, my, &lx, &ly);
                if (lx < 0) lx = 0; if (ly < 0) ly = 0; if (lx > LCD_W - 1) lx = LCD_W - 1; if (ly > LCD_H - 1) ly = LCD_H - 1;
                set_touch_screen(lx, ly, (st & SDL_BUTTON_LMASK) && ev.type != SDL_MOUSEBUTTONUP);
                break;
            }
            }
        }
        /* keep emulated time in step with wall-clock time */
        double t = (double)(SDL_GetPerformanceCounter() - start) / freq;
        long long target = (long long)(t * CPU_HZ);
        long long todo = target - emu_cycles;
        if (todo > CPU_HZ / 10) todo = CPU_HZ / 10;   /* don't spiral if we fall behind */
        if (uic_powered_off) {           /* off: nothing runs until POWER is pressed */
            emu_cycles = target;
            static bool released;        /* needs a fresh press, not the one that turned it off */
            if (!(uic_input.power_status & 0x02)) released = true;
            else if (released) uic_reboot_request = 0;
        } else if (todo > 0) { apply_holds(emu_cycles); run_cycles(todo); emu_cycles += todo; }
        if (target - emu_cycles > CPU_HZ / 2) emu_cycles = target - CPU_HZ / 10;
        if (audio_backlog() > 48000 / 5) audio_discard();   /* keep audio latency bounded */
        if (uic_reboot_request >= 0) {   /* UIC restarts the SoC in another mode */
            char v[16]; snprintf(v, sizeof v, "%d", uic_reboot_request);
            setenv("UIC_BOOTPARAM", v, 1);
            fprintf(stderr, "[uic] rebooting SoC into mode %s\n", v);
            if (adev) SDL_CloseAudioDevice(adev);
            SDL_Quit();
            execv(argv[0], argv);
            perror("execv"); return 1;
        }

        if (uic_powered_off) memset(fb, 0, sizeof fb); else lcd_render(fb);
        SDL_UpdateTexture(tex, NULL, fb, LCD_W * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
    }
    if (adev) SDL_CloseAudioDevice(adev);
    SDL_Quit();
    return 0;
}
