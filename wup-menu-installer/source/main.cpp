/*
 *   Homebrew Menu Installer: puts the Homebrew Menu on the Wii U GamePad, or takes it off, through the console's own
 *   GamePad firmware update. Part of wup-homebrew; built on DRXUtil (github.com/GaryOderNichts/DRXUtil): its drawing
 *   (Gfx, SDL_FontCache), ProcUI and flashing code (drc.cpp), and its input handling below.
 *
 *   This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 *   Public License as published by the Free Software Foundation, either version 2 of the License, or (at your
 *   option) any later version. See LICENSE.
 *
 *   Install: the console's copy of Nintendo's GamePad firmware (drc_fw.bin), checked to be the one the boot hook was
 *   made for, plus the hook and the bundle (fwpatch.c), written to the MLC's tmp folder and sent to the GamePad. The
 *   GamePad writes it to its other firmware bank and switches to it only once it is complete: an update that fails
 *   leaves the GamePad as it was. Remove: the console's copy as it is, the same way.
 *
 *   First a disclaimer, the way the Wii's HackMii Installer did it: white capitals on black, faded in once the console's
 *   GamePad firmware is checked, and "Press A to continue" only after a wait (DISCLAIMER_MS).
 */
#include "Gfx.hpp"
#include "ProcUI.hpp"
#include "drc.hpp"
#include "fwpatch.h"
#include <payload_bin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <sndcore2/core.h>
#include <vpad/input.h>

namespace {

constexpr SDL_Color INK = {0x20, 0x21, 0x24, 0xff}, SUBTLE = {0x6b, 0x6e, 0x75, 0xff}, PAPER = {0xff, 0xff, 0xff, 0xff},
                    BAR = {0xf5, 0xf5, 0xf6, 0xff}, LINE = {0xe2, 0xe3, 0xe6, 0xff}, PICK = {0xe9, 0xf1, 0xfe, 0xff},
                    BLUE = {0x1a, 0x73, 0xe8, 0xff}, TRACK = {0xe2, 0xe3, 0xe6, 0xff};
constexpr int W = Gfx::SCREEN_WIDTH, H = Gfx::SCREEN_HEIGHT, TOP = 144, BOTTOM = 100;

const char* TMP_DEVOPTAB = "storage_mlc01:/usr/tmp/drc_fw.bin";
const char* TMP_PATH = "/vol/storage_mlc01/usr/tmp/drc_fw.bin";

enum State { LOADING, BLOCKED, MENU, CONFIRM, PREPARE, FLASHING, DONE, FAILED };
State state = LOADING;                     // (LOADING: the disclaimer, while Load runs)
State after = MENU;                        // where the disclaimer leads
bool loaded = false;
uint32_t shownAt = 0;
constexpr uint32_t DISCLAIMER_MS = 2000;   // (was 15000: the fork keeps the disclaimer but drops the long wait)
constexpr float PROMPT_FADE_MS = 600.0f, PROMPT_PULSE_MS = 2000.0f;
constexpr uint32_t FADE_MS = 400;          // the disclaimer fades out to black, then the installer fades in
uint32_t fadeOutAt = 0, fadeInAt = 0;
const char* const DISCLAIMER[] = {
    "THIS SOFTWARE CHANGES THE FIRMWARE OF YOUR WII U GAMEPAD.",
    "",
    "IT IS PROVIDED AS IS, WITHOUT WARRANTY OF ANY KIND.",
    "YOU USE IT ENTIRELY AT YOUR OWN RISK.",
    "THE AUTHORS ARE NOT RESPONSIBLE FOR ANY DAMAGE",
    "TO YOUR GAMEPAD, YOUR WII U OR YOUR DATA.",
    "",
    "THIS SOFTWARE IS FREE. IF YOU PAID FOR IT,",
    "YOU HAVE BEEN SCAMMED: ASK FOR YOUR MONEY BACK.",
    "",
    "NOT AFFILIATED WITH OR ENDORSED BY NINTENDO.",
};
int row = 0;                       // 0 Install, 1 Remove
bool installing = true;            // what CONFIRM .. FAILED are about
std::string blocked, cantInstall, failure, original;
int progress = 0;

// ---- input: GamePad, and Wii Remotes / Pro Controllers while the GamePad is updating (DRXUtil's main.cpp)
uint32_t Remap(uint32_t b, bool classic)
{
    struct { uint32_t w, c, v; } map[] = {
        {WPAD_BUTTON_UP, WPAD_CLASSIC_BUTTON_UP, VPAD_BUTTON_UP}, {WPAD_BUTTON_DOWN, WPAD_CLASSIC_BUTTON_DOWN, VPAD_BUTTON_DOWN},
        {WPAD_BUTTON_A, WPAD_CLASSIC_BUTTON_A, VPAD_BUTTON_A}, {WPAD_BUTTON_B, WPAD_CLASSIC_BUTTON_B, VPAD_BUTTON_B},
        {WPAD_BUTTON_HOME, WPAD_CLASSIC_BUTTON_HOME, VPAD_BUTTON_HOME},
    };
    uint32_t v = 0;
    for (auto& m : map) if (b & (classic ? m.c : m.w)) v |= m.v;
    return v;
}
uint32_t Pressed()
{
    VPADStatus vpad{};
    VPADReadError verr;
    uint32_t t = VPADRead(VPAD_CHAN_0, &vpad, 1, &verr) > 0 && verr == VPAD_READ_SUCCESS ? vpad.trigger : 0;
    for (int i = 0; i < 4; i++) {
        KPADStatus k{};
        KPADError kerr;
        if (KPADReadEx((KPADChan) i, &k, 1, &kerr) > 0 && kerr == KPAD_ERROR_OK && k.extensionType != 0xFF) {
            bool classic = !(k.extensionType == WPAD_EXT_CORE || k.extensionType == WPAD_EXT_NUNCHUK);
            t |= Remap(classic ? k.classic.trigger : k.trigger, classic);
        }
    }
    return t;
}

// ---- files
bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    bool ok = n > 0 && fread(out.data(), 1, n, f) == (size_t) n;
    fclose(f);
    return ok;
}
bool WriteFile(const char* path, const uint8_t* p, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
std::string Devoptab(const std::string& path)        // /vol/storage_mlc01/... -> storage_mlc01:/...
{
    const std::string vol = "/vol/storage_mlc01";
    return path.rfind(vol, 0) == 0 ? "storage_mlc01:" + path.substr(vol.size()) : std::string();
}

// ---- drawing, in the Homebrew Menu's look (854x480 there, scaled 2.25x to 1920x1080 here)
void Rounded(int x, int y, int w, int h, int r, SDL_Color c)    // (SDL2_gfx's roundedBox leaves seams at the ends)
{
    if (r > h / 2) r = h / 2;
    if (r > w / 2) r = w / 2;
    Gfx::DrawRectFilled(x + r, y, w - 2 * r, h, c);
    Gfx::DrawRectFilled(x, y + r, w, h - 2 * r, c);
    for (int cx : {x + r, x + w - r - 1}) for (int cy : {y + r, y + h - r - 1}) Gfx::DrawCircleFilled(cx, cy, r, c);
}
void Hint(int& x, const char* key, const char* label)          // laid out from the right
{
    int tw = Gfx::GetTextWidth(40, label);
    x -= tw;
    Gfx::Print(x, H - BOTTOM / 2, 40, INK, label, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    x -= 16 + 22;
    Gfx::DrawCircleFilled(x, H - BOTTOM / 2, 22, INK);
    Gfx::Print(x, H - BOTTOM / 2, 30, PAPER, key, Gfx::ALIGN_CENTER);
    x -= 22 + 48;
}
void Frame(const char* b, const char* a)                        // bars; the hints: B immediately left of A
{
    Gfx::Clear(PAPER);
    Gfx::DrawRectFilled(0, 0, W, TOP, BAR);
    Gfx::Print(72, TOP / 2, 72, INK, "Homebrew Menu", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    Gfx::DrawRectFilled(0, H - BOTTOM, W, BOTTOM, BAR);
    Gfx::Print(72, H - BOTTOM / 2, 36, SUBTLE, "v" APP_VERSION, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);   // (as the menu's home)
    int x = W - 72;
    if (a) Hint(x, "A", a);
    if (b) Hint(x, "B", b);
}
void Statement(const std::string& title, const std::string& line)
{
    int mid = TOP + (H - TOP - BOTTOM) / 2;
    Gfx::Print(W / 2, mid - 40, 72, INK, title, Gfx::ALIGN_CENTER);
    if (!line.empty()) Gfx::Print(W / 2, mid + 50, 44, SUBTLE, line, Gfx::ALIGN_CENTER);
}
void Row(int i, int y, const char* label, const std::string& detail, bool enabled)
{
    const int x = 360, w = W - 720, h = 150;
    if (i == row) Rounded(x, y, w, h, 24, PICK);
    Gfx::Print(x + 48, y + 52, 52, enabled ? INK : SUBTLE, label, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    Gfx::Print(x + 48, y + 108, 36, SUBTLE, detail, Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
}

void Disclaimer()
{
    const SDL_Color white = {0xff, 0xff, 0xff, 0xff}, black = {0x00, 0x00, 0x00, 0xff};
    Gfx::Clear(black);
    const int n = sizeof DISCLAIMER / sizeof DISCLAIMER[0], line = 64;
    int y = (H - n * line) / 2 - 40;
    for (int i = 0; i < n; i++, y += line)
        if (DISCLAIMER[i][0]) Gfx::Print(W / 2, y, 46, white, DISCLAIMER[i], Gfx::ALIGN_CENTER);
    if (shownAt && SDL_GetTicks() - shownAt >= DISCLAIMER_MS) {          // "Press (A) to continue", centred
        // fades in (0.6 s), then breathes slowly between 55 % and full (every 2 s). On black, grey is the same as
        // white made transparent: the prompt is drawn in grey.
        float t = (float)(SDL_GetTicks() - shownAt - DISCLAIMER_MS), k;
        if (t < PROMPT_FADE_MS) k = t / PROMPT_FADE_MS;
        else k = 0.775f + 0.225f * cosf(6.2831853f * (t - PROMPT_FADE_MS) / PROMPT_PULSE_MS);
        const uint8_t v = (uint8_t)(255.0f * k);
        const SDL_Color white = {v, v, v, 0xff};
        const int size = 44, r = 26, gap = 18;
        int w1 = Gfx::GetTextWidth(size, "Press"), w2 = Gfx::GetTextWidth(size, "to continue");
        int x = (W - (w1 + gap + 2 * r + gap + w2)) / 2, cy = H - 110;
        Gfx::Print(x, cy, size, white, "Press", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        x += w1 + gap + r;
        Gfx::DrawCircleFilled(x, cy, r, white);
        Gfx::Print(x, cy, 34, black, "A", Gfx::ALIGN_CENTER);
        Gfx::Print(x + r + gap, cy, size, white, "to continue", Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }
}

void Draw()
{
    switch (state) {
    case LOADING:
        Disclaimer();
        break;
    case PREPARE:
        Frame(nullptr, nullptr);
        break;
    case BLOCKED:
        Frame("Exit", nullptr);
        Statement("Can't Continue", blocked);
        break;
    case MENU: {
        Frame("Exit", "Select");
        int y = TOP + (H - TOP - BOTTOM) / 2 - 165;
        Row(0, y, "Install", cantInstall.empty() ? "The Homebrew Menu on this GamePad" : cantInstall, cantInstall.empty());
        Gfx::DrawRectFilled(408, y + 165, W - 816, 2, LINE);
        Row(1, y + 180, "Remove", "The GamePad's original firmware", true);
        break;
    }
    case CONFIRM:
        Frame("Cancel", installing ? "Install" : "Remove");
        Statement("Are You Sure?", installing ? "This changes your GamePad's firmware. Keep it charging and close by"
                                              : "This puts your GamePad's original firmware back");
        break;
    case FLASHING: {
        Frame(nullptr, nullptr);
        int mid = TOP + (H - TOP - BOTTOM) / 2;
        Gfx::Print(W / 2, mid - 80, 72, INK, installing ? "Installing\xE2\x80\xA6" : "Removing\xE2\x80\xA6", Gfx::ALIGN_CENTER);
        const int bw = 900, bx = (W - bw) / 2;
        Rounded(bx, mid + 10, bw, 16, 8, TRACK);
        if (progress > 0) Rounded(bx, mid + 10, bw * (progress > 100 ? 100 : progress) / 100, 16, 8, BLUE);
        Gfx::Print(W / 2, mid + 110, 44, SUBTLE, "Don't turn off the console or the GamePad", Gfx::ALIGN_CENTER);
        break;
    }
    case DONE:
        Frame(nullptr, "Done");
        if (installing) Statement("Installed", "Hold A while turning on the GamePad to open the Homebrew Menu. The first time takes a few seconds longer");
        else Statement("Removed", "The GamePad runs its original firmware");
        break;
    case FAILED:
        Frame("Back", nullptr);
        Statement(installing ? "Couldn't Install" : "Couldn't Remove", failure);
        break;
    }
    uint32_t now = SDL_GetTicks();
    if (state == LOADING && !shownAt) Gfx::Fade(255);                                  // black until it is shown,
    else if (state == LOADING && !fadeOutAt && now - shownAt < FADE_MS)                // then the disclaimer fades in
        Gfx::Fade((uint8_t)(255 - 255 * (now - shownAt) / FADE_MS));
    else if (state == LOADING && fadeOutAt) Gfx::Fade((uint8_t)(255 * std::min<uint32_t>(now - fadeOutAt, FADE_MS) / FADE_MS));
    else if (fadeInAt && now - fadeInAt < FADE_MS) Gfx::Fade((uint8_t)(255 - 255 * (now - fadeInAt) / FADE_MS));
    Gfx::Render();
}

// ---- the work
void Load()
{
    std::string err;
    if (!Drc::Init(err)) { blocked = err; after = BLOCKED; return; }
    if (!Drc::FirmwarePath(original)) { blocked = "The GamePad firmware was not found"; after = BLOCKED; return; }
    std::vector<uint8_t> fw;
    if (!ReadFile(Devoptab(original), fw)) { blocked = "The GamePad firmware could not be read"; after = BLOCKED; return; }
    const char* why = nullptr;
    if (fwpatch_check(fw.data(), fw.size(), payload_bin, payload_bin_size, &why)) cantInstall = why;
    after = MENU;
}

bool Begin(std::string& err)
{
    std::string path = original;
    if (installing) {
        std::vector<uint8_t> fw;
        if (!ReadFile(Devoptab(original), fw)) { err = "The GamePad firmware could not be read"; return false; }
        uint8_t* out = nullptr;
        size_t n = 0;
        const char* why = nullptr;
        if (fwpatch(fw.data(), fw.size(), payload_bin, payload_bin_size, &out, &n, &why)) { err = why; return false; }
        bool ok = WriteFile(TMP_DEVOPTAB, out, n);
        free(out);
        if (!ok) { remove(TMP_DEVOPTAB); err = "The system memory is full"; return false; }
        path = TMP_PATH;
    }
    return Drc::Start(path, err);
}

void Update(uint32_t in)
{
    switch (state) {
    case LOADING:                                     // the disclaimer: the checks run behind it; A after the wait
        if (!loaded) { Load(); loaded = true; shownAt = SDL_GetTicks(); }   // (it fades in once the checks are done)
        if (fadeOutAt) {
            if (SDL_GetTicks() - fadeOutAt >= FADE_MS) { state = after; fadeInAt = SDL_GetTicks(); }
        } else if (SDL_GetTicks() - shownAt >= DISCLAIMER_MS && (in & VPAD_BUTTON_A)) fadeOutAt = SDL_GetTicks();
        break;
    case BLOCKED:
        if (in & VPAD_BUTTON_B) ProcUI::StopRunning();
        break;
    case MENU:
        if (in & VPAD_BUTTON_UP) row = 0;
        if (in & VPAD_BUTTON_DOWN) row = 1;
        if (in & VPAD_BUTTON_B) ProcUI::StopRunning();
        if ((in & VPAD_BUTTON_A) && (row == 1 || cantInstall.empty())) { installing = row == 0; state = CONFIRM; }
        break;
    case CONFIRM:
        if (in & VPAD_BUTTON_B) state = MENU;
        else if (in & VPAD_BUTTON_A) state = PREPARE;
        break;
    case PREPARE:                                     // (drawn once, empty, before the work)
        ProcUI::SetHomeButtonMenuEnabled(false);
        progress = 0;
        if (Begin(failure)) state = FLASHING;
        else { state = FAILED; remove(TMP_DEVOPTAB); ProcUI::SetHomeButtonMenuEnabled(true); }
        break;
    case FLASHING:
        if (!Drc::Poll(progress)) {
            state = Drc::Finish(failure) ? DONE : FAILED;
            remove(TMP_DEVOPTAB);
            ProcUI::SetHomeButtonMenuEnabled(true);
        } else {
            OSSleepTicks(OSMillisecondsToTicks(200));
        }
        break;
    case DONE:
        if (in & (VPAD_BUTTON_A | VPAD_BUTTON_B)) ProcUI::StopRunning();
        break;
    case FAILED:
        if (in & VPAD_BUTTON_B) state = MENU;
        break;
    }
}

}

int main(int, char**)
{
    ProcUI::Init();
    AXInit();                      // (stops sounds still playing)
    KPADInit();
    WPADEnableURCC(TRUE);
    Gfx::Init();
    while (ProcUI::IsRunning()) {
        uint32_t in = Pressed();
        Draw();
        Update(in);
    }
    Drc::Shutdown();
    Gfx::Shutdown();
    AXQuit();
    ProcUI::Shutdown();
    return 0;
}
