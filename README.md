# wup-homebrew

Custom firmware for the **Wii U GamePad** (model WUP-010), side by side with Nintendo's. Hold **A** at power-on and
the GamePad starts **wup-homebrew**: our own kernel, the **Homebrew Menu** and its apps, with the whole pad to
themselves. Don't, and it starts Nintendo's firmware exactly as it always has.

<p align="center"><img src="wup-menu/docs/images/home.png" width="427" alt="The Homebrew Menu"> <img src="wup-menu/docs/images/settings.png" width="427" alt="Settings"></p>

## How it works

```
 power on ─▶ boot ROM ─▶ stage 2 (Nintendo's, untouched) ─▶ Nintendo's firmware, its first instruction
                                                                │  a 4-byte branch to
                                                                ▼
                                          the early hook (~1 KB): A held, or Autoboot on?
                                    no │                                              │ yes
                                       ▼                                              ▼
                     Nintendo's firmware, exactly as before        the wup-homebrew kernel (its drivers: screen,
                     (pairing, the Wii U, TV remote, standby)      sound, buttons, flash, Wi-Fi, mic, rumble)
                                                                    └─ the Homebrew Menu ─▶ apps (one flat 1.8 MB
                                                                       block each; nothing of Nintendo's runs)
```

- **Stock stays stock.** Nintendo's firmware is changed in three places only: the branch, and the early hook's code
  over error-message text it never uses. Without A (or Autoboot) the hook puts back everything it touched and stock
  runs from its normal start.
- **The whole pad.** Under wup-homebrew nothing of Nintendo's runs: apps get the CPU and about 1.8 MB of memory each,
  where they had a few hundred KB around Nintendo's firmware.
- **Safe.** If the kernel or the menu in flash is not whole, A starts stock. Hold the Power button a second to turn
  it off, as on stock.

## Get it

From a Wii U with [Aroma](https://aroma.foryour.cafe/): the **Homebrew Menu Installer** (`homebrew_menu_installer.wuhb`
from the [releases](https://github.com/ravensclawx/wup-homebrew/releases)), which installs through the GamePad's own
firmware update: no opening the pad. Steps: [wup-menu-installer](wup-menu-installer#use). **Had version 1?** Remove it
with the version 1 installer first, then install version 2.

After that, Settings > System > Software Update keeps it current. Also over UART or, on a pad running the Homebrew
Menu, over Wi-Fi ([wup-menu/docs/install.md](wup-menu/docs/install.md)).

## What's here

| Folder | What |
|---|---|
| [`kernel`](kernel) | the wup-homebrew kernel: scheduler, the pad's drivers, the services apps use |
| [`libwup`](libwup) | the SDK: the runtime every app is built on (C library, LVGL, threads, sockets, the pad's hardware), the `.wup` app format, and the tools to build, send and debug apps |
| [`wup-menu`](wup-menu) | the early hook, the Homebrew Menu (with its App Store and Software Update), and the install tools (UART, Wi-Fi) |
| [`wup-menu-installer`](wup-menu-installer) | the Homebrew Menu Installer: a Wii U app that installs through the GamePad's own firmware update |
| [`wup-emu`](wup-emu) | the GamePad emulator, running the real boot ROM and flash of your own pad |

## Apps

Apps come to the GamePad through the App Store, from any repositories you add, or over File Sharing (FTP):

- [WUP Stream](https://github.com/ravensclawx/wup-stream): the GamePad as a wireless screen and controller for your
  computer: picture, sound, every input, the microphone and the camera.
- [Doom](https://github.com/ravensclawx/wup-doom): the shareware episode, with the original OPL music.
- Hello World: libwup's sample.

[WUP Boy](https://github.com/ravensclawx/wup-boy) (Game Boy games) is archived for now: emulators are next.

Write your own with [libwup](libwup): `kit = yes` in an app's `app.cfg` gives it the menu's look.

## Quick start

Needs LLVM (`clang`, `ld.lld`), Python 3 with Pillow, meson and ninja. macOS: `brew install llvm lld meson ninja`.

```
make -C kernel                                              # the kernel -> kernel/build/kernel.bin
cd libwup && python3 tools/wupbuild.py examples/hello      # an app -> build/apps/hello_world.wup
python3 tools/wuplink.py build/apps/hello_world.wup         # send it to the pad (press Y in the Homebrew Menu first)
cd ../wup-menu && python3 ../libwup/tools/wupbuild.py menu  # the menu itself
python3 tools/mkflash.py DUMP.bin build/emu.bin             # wup-homebrew in a copy of your pad's flash: try it in wup-emu
```

## Notes

- Only GamePad firmware LVC rev 7677 (`NR4_12_20140805_release`, Nintendo's last) is supported.
- No Nintendo code or firmware is included. At start-up the kernel reads two things from the pad's own firmware
  bank: the Wi-Fi radio's firmware (Broadcom's, which runs inside the radio chip) and the settings of its board. The
  display and sound start-up replays the register values the stock firmware writes (`kernel/src/fw_init.c`,
  `fw_audio.c`, recorded in the emulator). The emulator and the install tools need dumps of your own pad, kept outside
  the repository (`wup-firmware/` next to these folders is ignored, or `WUP_FIRMWARE=path`).

## Licence

**GPL-3.0-or-later** ([LICENSE](LICENSE)): the kernel, libwup, wup-menu and wup-emu. Apps built with libwup are
therefore GPL too. `wup-menu-installer` is GPL-2.0-or-later, like DRXUtil, which it is built on.

Third-party code is fetched by the builds, not kept here, each under its own licence (`libwup/tools/deps.py`):
BearSSL, jsmn and picohttpparser (MIT), LVGL (MIT), littlefs (BSD-3-Clause), lwIP (BSD-3-Clause), picolibc (BSD),
tinf (zlib). Kept here with their own licences: the Inter fonts (SIL OFL 1.1), Hello World's icon (Twemoji,
CC-BY 4.0); App Store's and Software Update's icons are drawn with Font Awesome Free symbols (CC-BY 4.0,
fontawesome.com).
