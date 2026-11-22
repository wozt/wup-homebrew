# wup-emu: Wii U GamePad (DRC) firmware emulator

Low-level emulator for the Wii U GamePad, running the real boot ROM and SPI flash dump of your own
pad. SDL2 provides the display, input (keyboard, mouse as touchscreen, game controllers) and audio.

## Build & run

    brew install sdl2
    make
    ./wup-emu                      # opens an 854x480 window, runs in real time
    ./wup-emu --scale 2            # bigger window
    ./wup-emu --headless --cycles 1500000000 --screenshot out.bmp

It looks for `bootrom.bin`, `flash.bin` and `uic_eeprom.bin` in `$WUP_FIRMWARE`, `firmware/`, `../wup-firmware/`
(a folder next to this one), `../firmware/`, then the current folder. `--bootrom` and `--flash` pick other files; `UIC_EEPROM=file`
another EEPROM (it is written back when the firmware changes it). The dumps are not in git.
Timing: the emulated CPU runs at 108 MHz; the real pad's ARM926 runs at about 400 MHz, so frame rates
measured here are pessimistic.

Debug options:

* `DRC_DEBUG=1`: log unknown MMIO, I2C and UIC traffic, DMA and data aborts
* `MMIOTRACE=f0005800-f0006fff`: trace every access in a physical range
* `--break <hexaddr>`: stop at a PC and dump registers, recent PCs and blocked kernel calls
* `HLE_RET=addr:val,...`: make firmware functions return `val` immediately (experiments)
* `RAMDUMP=file` (headless): dump the 4 MiB of RAM on exit
* `WIFI_DEBUG=1`: log SDIO commands, backplane accesses and dongle ioctls
* `WATCH=start-end`: log each unique PC that reads a virtual address range
* `TRACEPC=addr`: print registers every time a PC is executed
* `HID_POKE=byte:mask@start_ms-end_ms`: force bits in the raw UIC input report
* `UIC_LOG=1`: log UIC commands (except input polls); `UIC_BATTERY=0..6`: the battery level (default 5); `UIC_BOOTPARAM=n`: UIC boot mode (cmd 0x05): 0 normal, 5 pairing (SYNC), 9 TV remote (TV button), 12 DK menu
* `DMA2D_LOG=1`: log 2D DMA (blitter) operations; `AUDIO_WAV=file`: dump the audio output
* `--touch <x> <y> <start_ms> <end_ms>` (headless): script a touchscreen tap at screen coordinates
* `--hold <button> <start_ms> <end_ms>` (headless): script button presses (a, b, x, y, up, down, left, right, l, r, zl, zr, plus, minus, home, sync, tv, power, l3, r3)

Firmware debug output (`[fw] ...`) comes from hooking the firmware's compiled-out printf.

## Keys

| GamePad | Key | GamePad | Key |
|---|---|---|---|
| A / B / X / Y | X / Z / S / A | D-pad | arrows |
| L / R | Q / W | ZL / ZR | 1 / 2 |
| + / − | Enter / Backspace | HOME | H |
| TV | T | POWER | P |
| SYNC (back) | Y | Left stick | I J K L |
| Right stick | keypad 8 4 2 6 | L3 / R3 (stick click) | U / O |
| Volume slider | [ / ] | Touchscreen | mouse |
| Screenshot | F12 | Quit | Esc |

Game controllers (via SDL) work too, mapped by button position: bottom face button = B,
right = A, left = Y, top = X; triggers = ZL/ZR; Start/Back/Guide = + / − / HOME; stick clicks = L3/R3.

SYNC and TV are handled by the UIC (the power-management MCU), not by the main firmware:
pressing either one restarts the SoC in pairing mode or standalone TV-remote mode, like the real hardware.

## What is emulated

| Block | Address | Status |
|---|---|---|
| ARM926EJ-S (ARMv5TE ARM+Thumb, CP15, MMU) | | complete interpreter, ~70 MIPS |
| Boot ROM / RAM remap / soft reset | 0xF0000000 | ok |
| Interrupt controller | 0xF0001200 | ok (per-IRQ mask/priority, ack at +0x1F0) |
| Timers + 1 MHz counter | 0xF0000400 | ok (108 MHz core clock) |
| SPI controller + DMA (flash, UIC) | 0xF0004400 / 0xF0004000 | ok, incl. 2D DMA channels |
| SPI NOR flash (32 MiB) | | read / program / erase |
| UIC (Renesas MCU: buttons, power, EEPROM) | SPI, CS = GPIO 0xF00050FC | boot state, version, HID report, config EEPROM (`uic_eeprom.bin`) |
| I2C ("Renesas" channels 2-4) | 0xF0005800 | ok; LCD panel IC (R69208), OV7740 camera, TLV320AIC3012 codec |
| LCD controller (854x480 8bpp + palette) | 0xF0009400 | rendered to the SDL window; vsync IRQ 22 at 60 Hz |
| Touchscreen + buttons + sticks (UIC HID report) | | big-endian button word, touch calibration from the firmware defaults |
| SD host (SDHCI) + Broadcom BCM4319 SDIO Wi-Fi | 0xE0010000, IRQ 2 | host + card + backplane modelled; firmware download accepted; fake dongle answers CDC ioctls (no networks) |
| Audio out (DMA ring → I2S) | 0xF0005400, IRQ 23 | 48 or 24 kHz stereo (control bit 22) → SDL audio at 48 kHz; codec DAC volume/mute (volume slider) applied |
| Audio capture block | 0xF0005480 | 180 Hz frame tick (IRQ 30) that paces input polling; mic samples not yet |
| Video decode / camera capture / NFC / IR | | not yet |

## Custom firmware

The menu and libwup ([architecture.md](architecture.md)) were developed in this emulator. Build a flash image with
it and run it:

    python3 ../wup-menu/tools/mkflash.py ../wup-firmware/flash.bin build/cfw.bin   # (wup-menu: the Homebrew Menu)
    ./wup-emu --flash build/cfw.bin      # hold X (= A on the keyboard) while it starts

`emulator/tools/mkpair.py` writes a pairing record into the emulated EEPROM; `emulator/tools/console.py`
talks to the emulated pad over the emulator's UDP bridge (`NET_HOST`).

Debug aids added for this: `FLASH_LOG=1` (every flash read, `FLASH_LOG_STACK=1` adds callers),
`UIC_LOG_ALL=1` (include input polls), `UIC_LOG_STACK=1`, `I2C_LOG=1`, `RAMFILL=0xa5` (canary-fill RAM
to find unused memory, with `RAMDUMP=file`).

## Boot progress

Boot ROM → stage 2 loader → main firmware `LVC_` (rev 7677, NR4_12_20140805_release) →
LCD panel init → camera / codec init → UIC sync, board info, power-button detect →
Wi-Fi driver up → application: **"This Wii U GamePad has not yet been paired with a Wii U console"**
(about 14 s of emulated time; the emulator idles while the firmware waits, so this is fast).

Other modes (SYNC / TV / `UIC_BOOTPARAM`):

* **Pairing (SYNC)**: "Tap the symbols in the order they are displayed on the TV screen": touch
  entry, backspace and Cancel work, with the firmware's own UI click sounds.
* **TV remote (TV button)**: shows "Set up TV Remote in System Settings." because no TV maker code is
  stored in the EEPROM, then turns the screen off, as a GamePad does when the TV remote isn't set up.
* **DK menu (12)**: dev-kit menu; D-pad/A navigate it; System Information shows the
  DRC / UMI / Wi-Fi firmware versions from the dump.

Not yet: video decode / streaming (needs a Wii U console on the other end of the Wi-Fi link),
camera capture, microphone samples, NFC, IR transmit, accelerometer/gyro beyond a fixed 1 g.

## Licence

GPL-3.0-or-later ([LICENSE](../LICENSE)), as the rest of [wup-homebrew](..).
