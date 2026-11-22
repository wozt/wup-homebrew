# wup-homebrew

Homebrew for the **Wii U GamePad** (model WUP-010): the **Homebrew Menu**, the SDK its apps are built with, an
installer that puts it on a GamePad from a Wii U, and an emulator of the pad. Everything was
worked out from the pad's own firmware, and it runs on a real GamePad.

<p align="center"><img src="wup-menu/docs/images/home.png" width="427" alt="The Homebrew Menu"> <img src="wup-menu/docs/images/settings.png" width="427" alt="Settings"></p>

## Get it

You need a Wii U running [Aroma](https://aroma.foryour.cafe/) and its GamePad.

1. Download `homebrew_menu_installer.wuhb` from [Releases](https://github.com/ravensclawx/wup-homebrew/releases) into
   `sd:/wiiu/apps/` on the Wii U's SD card, and open **Homebrew Menu Installer** from the Wii U Menu.
2. **Install**. Keep the GamePad charging and close to the console; it takes a couple of minutes.
3. Turn the GamePad off, then on while holding **A**: the Homebrew Menu, with **App Store** and **Software Update**.
   Join Wi-Fi in Settings, then get apps from the App Store.

Without A the GamePad starts as always. **Remove** in the same installer puts the original firmware back.
Use at your own risk: see the installer's [README](wup-menu-installer).

## What's here

| Folder | What |
|---|---|
| [`wup-menu`](wup-menu) | the Homebrew Menu: a small hook in the pad's firmware, the loader, the menu, App Store and Software Update, and the install tools (UART, Wi-Fi updates). Hold A at power-on |
| [`libwup`](libwup) | the SDK: the runtime every app is built on (C library, LVGL, threads, sockets, the pad's hardware), the `.wup` app format, and the tools to build, send and debug apps |
| [`wup-menu-installer`](wup-menu-installer) | the Homebrew Menu Installer: a Wii U app that installs the menu through the GamePad's own firmware update, no soldering (GPL, built on DRXUtil) |
| [`wup-emu`](wup-emu) | the GamePad emulator, running the real boot ROM and flash of your own pad |

## Apps

Apps have repositories of their own and come to the GamePad through the App Store
([its catalogue](https://github.com/ravensclawx/wup-homebrew-apps)):

| App | What |
|---|---|
| [WUP Stream](https://github.com/ravensclawx/wup-stream) | a computer's picture, sound and controls on the GamePad: libwupstream on the computer, the app on the pad |
| [WUP Boy](https://github.com/ravensclawx/wup-boy) | Game Boy and Game Boy Color games, with save states, rewind and fast forward |

Write your own with [libwup](libwup): `kit = yes` in an app's `app.cfg` gives it the menu's look.

## Network Storage

The menu can run `.wup` apps straight from a computer on your LAN — nothing is copied to
the GamePad's flash. Run the little server on the machine that holds the apps:

```sh
python3 tools/netfs-server/netfs_server.py --root /path/to/share --host 0.0.0.0 --port 8734 --read-write
```

Then in the Homebrew Menu: **Settings → Network → Network Storage** → server IP, port,
token, enable. The share's `.wup` files appear on the home screen under
**Network Storage**: **A** runs one, **-** shows its info (version, size) and offers
**Delete App**. `--read-write` also allows uploading apps to the share; without it the
server is read-only. Details and options: [tools/netfs-server](tools/netfs-server/README.md).
The protocol is LAN-only and not encrypted — keep it on a trusted network.

## Quick start

Needs LLVM (`clang`, `ld.lld`), Python 3 with Pillow, meson and ninja. macOS: `brew install llvm lld meson ninja`.

```
cd libwup && python3 tools/wupbuild.py examples/hello      # an app -> build/apps/hello_world.wup
python3 tools/wuplink.py build/apps/hello_world.wup         # send it to the pad (press Y in the Homebrew Menu first)
cd ../wup-menu && python3 ../libwup/tools/wupbuild.py menu  # the menu itself
```

Install the menu on a GamePad from a Wii U ([wup-menu-installer](wup-menu-installer)) or over UART
([wup-menu/docs/install.md](wup-menu/docs/install.md)). Each folder's README has the rest.

## Notes

- Only GamePad firmware LVC rev 7677 (`NR4_12_20140805_release`, Nintendo's last) is supported.
- No Nintendo code or firmware is included; the emulator and the install tools need dumps of your own pad, kept
  outside the repository (`wup-firmware/` next to these folders is ignored, or `WUP_FIRMWARE=path`).

## Licence

**GPL-3.0-or-later** ([LICENSE](LICENSE)): libwup, wup-menu, wup-stream and wup-emu. Apps built with libwup are
therefore GPL too. `wup-menu-installer` is GPL-2.0-or-later, like DRXUtil, which it is built on.

Third-party code is fetched by the builds, not kept here, each under its own licence (`libwup/tools/deps.py`):
BearSSL, jsmn and picohttpparser (MIT), LVGL (MIT), littlefs (BSD-3-Clause), lwIP (BSD-3-Clause), picolibc (BSD),
tinf (zlib). Kept here with their own licences: the Inter fonts (SIL OFL
1.1), Hello World's icon (Twemoji, CC-BY 4.0); App Store's and Software Update's icons are drawn with Font Awesome
Free symbols (CC-BY 4.0, fontawesome.com).
