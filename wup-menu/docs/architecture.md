# Architecture

A small hook in the GamePad's stock firmware loads the **Homebrew Menu** from flash; the stock firmware keeps running
underneath (its drivers are what libwup calls) and is otherwise untouched. Only LVC rev 7677
(`NR4_12_20140805_release`) is supported: every firmware address used was derived from that build.

| Folder | What |
|---|---|
| `boot/` | `hook.S` (patched into the firmware's boot-app selection), `loader.c` (menu or stock; runs app images), `firmware.h` (the loader's firmware calls) |
| `menu/` | The Homebrew Menu, an app like the others: `main.c` (state, network, apps), `ui.c` (LVGL screens), `wifi/`, `ftpd.c`, `wuplink.c` (the Netloader), `dbgsrv.c` (Developer Mode) |
| `tools/` | `mkflash.py` (the install, and emulator images), `menu_install.py` (updates over Wi-Fi), `mkinstaller.py` (the Wii U installer's payload), `flash/` (the UART install) |
| `apps/` | App Store and Software Update (the Wii U install's bundle brings them; `menu/bundle.c` puts them on the pad once) |
| `common/` | what the menu and its apps share: `catalog.c` (the catalogue: libget's format), `tone.h`, `version.h` |

Built with [libwup](../../libwup) (next to this folder): the runtime, its headers
(`handoff.h`, `wupfmt.h`: shared with the loader), the app tools, and the docs on writing apps and the firmware's
kernel. Installing: [install.md](install.md). The emulator: [wup-emu](../../wup-emu).

## Boot chain (all derived from the firmware, verified on the real pad)

Nothing in the boot chain checks signatures or checksums:

- **Boot ROM** (`bootrom.bin` in your dumps, 4 KiB) loads a stage 2 from flash 0 to RAM 0x3F0000.
- **Stage 2** asks the UIC (the power-management MCU) for the boot mode, picks Product1 (0x100000)
  unless the bank selector byte at flash 0xF000 is 1 (Product2, 0x500000; boot mode 7: Service at
  0x1C00000), loads `LVC_` (INDX entry 2) to RAM 0 and jumps to it.
- **The hook.** LVC's app manager starts the boot app by calling `get_boot_mode(&app_id)` (0x30734)
  from 0x63a0e. That one `bl` (4 bytes) is redirected to `boot/hook.S` (156 bytes), which sits in 160
  bytes of unreferenced WPS-CLI debug strings at LVC 0xb951c. On a normal boot it loads the loader to RAM
  0x340000, checks its `CFW1` header and calls it: from the bundle in the running bank (bank + 0x2c9000:
  the Wii U install) or else from flash 0x10000 (the UART install). These two patches (and the bundle) are
  all that changes in Nintendo's firmware.
- **Firmware updates** (the pad's side, LVC 0x2b122): the console's update is written to the bank the pad
  is *not* running (Product2 when it runs Product1), sector by sector after a 0x5000-byte header and CRC
  table it keeps in RAM; the console then activates it (the bank selector byte). The Wii U install
  ([wup-menu-installer](../../wup-menu-installer)) is such an update, with the hook and the bundle built in.
- **The loader** reads the buttons and the menu's settings, then takes the menu (checking magic, size and
  CRC) from the menu slot (0x30000) or the bundle, whichever has the newer stamp, inflates it (tinf) and runs it. On any problem it returns, and
  the pad boots the stock firmware.
- **Apps.** The menu is an app image like any other. To start an app it stages the image, fills in the hand-off
  block (libwup: `include/handoff.h`) and returns `WUP_HANDOFF_RUN` (`wup_exec`). The loader runs the staged image
  and then the menu again, so an app gets all the memory the menu used.

### Flash layout (32 MiB N25Q256)

Nintendo's partitions (kuribo64.net/wup FLASH layout): SPL 0x0, ErrorLog 0xE000, Bank 0xF000, Product1 0x100000,
Product2 0x500000, Lang1 0x900000, Lang2 0x1100000, TVcode 0x1900000, Screen1/2 0x1B00000 / 0x1B80000, Service
0x1C00000, diagnostics from 0x1FDE000. Ours: the gap between Bank and Product1 (blank on a stock pad), the hook's two
patches in a firmware bank, and the spare language copy for the filesystem.

**The filesystem's place.** The pad keeps its own message screens (pairing, errors, updating, low battery, the TV
remote: pictures, in every language) twice, like its firmware: Lang1 and Lang2. A selector in its settings memory (the
UIC's EEPROM; the firmware's cache of it + 0x256: selector, version, CRC-16) says which copy it uses: 1 = Lang2, anything
else = Lang1 (the reader at boot, LVC 0x3a93a). A language update (type 0 of the update receiver, LVC 0x2b0c0) is written
to the other copy and the selector flipped, so which copy is in use differs from pad to pad. The filesystem lives in the
copy **not** in use, asked of the firmware every time it is mounted (`libwup/src/fs.c`); if the firmware cannot say, there
is no filesystem. Checked in wup-emu with every flash read logged: selector 0 reads Lang1 only, selector 1 reads Lang2
only; the menu on a pad using Lang2 formats Lang1 and leaves Lang2 alone. No language update comes to these pads
(firmware 7677 is Nintendo's last; the installers send firmware only). Were one sent, it would replace the files with a
complete language copy and flip the selector: lost files, a working pad.

Homebrew Menu 1.0.0 always took Lang2. On a pad using Lang2 that formatted over the screens in use: the pad still works
with its console, but shows a blank screen for its own messages. 1.0.1 recognises such a pad (the copy in use holds our
filesystem, the other one does not) and points the selector at the other copy, whole and only older, with the firmware's
own setter (LVC 0x3baa4); the files stay.

| Flash | Contents |
|---|---|
| Product1 LVC_ + 0x63a0e (0x163a82) | `bl` to the hook (was `bl get_boot_mode`) |
| Product1 LVC_ + 0xb951c (0x1b5590) | the hook |
| 0x10000-0x11FFF | the loader (`CFW1`) |
| 0x30000 / 0x31000 | menu slot (to 0xAFFFF): header `{"WMNU", stored size, crc32, inflated size, stamp}` / menu image / then the bigmem code as a `{"WUPB", stored, raw, crc}` blob (uncompressed: stored == raw) |
| bank + 0x2C9000 | the Wii U install's bundle, after the firmware's last section: loader, menu header sector, menu image, then App Store and Software Update (a "WAPS" table and their .wup files) |
| 0xB0000, 0xB1000 | menu settings (two `WCFG` records with sequence number and CRC) |
| 0x900000-0x10FFFFF or 0x1100000-0x18FFFFF | littlefs (8 MB: `/apps`, app data) in whichever of **Lang1 / Lang2** the pad does not use for its message screens (see below) |
| 0x12000-0x2FFFF, 0xB2000-0xFFFFF | free (the filesystem until 2026-11-18) |

A stock GamePad firmware update (or the installer's Remove) writes the other bank without the hook; everything in
the gap stays (unused until the hook is installed again).

### RAM while an app runs (4 MiB total; the stock firmware keeps the rest)

| RAM | Use |
|---|---|
| 0x0DA000-0x136000 | `WUP_SPARE` (368 KB, cached); large apps: their code, constants and network buffers |
| 0x190000-0x2FA400 | `WUP_BIGMEM` (1.4 MB): a large app's `.bss`, then the heap (malloc), up to the RGB565 framebuffer at 0x221400 |
| 0x340000 | loader; 0x342000 audio ring, 0x34E000 flash bounce buffer, 0x34F000 hand-off block |
| 0x350000-0x380000 | the app (code, data, stack) |
| 0x388400-0x3EE000 | the firmware's 8-bit framebuffer |

### Firmware services used

| Service | Address | Notes |
|---|---|---|
| Flash read / write / erase | 0x9141 / 0x8fb3 / 0x92b9, handle `*(u32 *)0xbb5f4` | the driver DMAs: buffers must be at 0x178000 or above and uncached |
| Task sleep | 0x89ae4 (ARM) | call it in every loop: input polling and the radio are firmware tasks |
| Input | 0xbdab0 (buttons), 0x178d38 (full 128-byte report) | sticks are 12-bit, centred on 0x800 |
| Radio | `wl_ioctl` 0x1b165, `dhd_ioctl` 0x41517 | see `menu/wifi/wifi_hw.c` |
| Kernel (µITRON) | libwup: `docs/kernel.md` | threads, event flags: libwup `src/kernel.c` |
| LCD | 0xF0009400 | +0xB0 format (3 = RGB565), +0x70 stride, +0x74 address, +0xC8 line counter |
| Audio | 0xF0005400 | loop mode over a ring; +0x20 bit 0 mutes; bit 2 (stop) wedges the engine: never used |

## Before touching real hardware

- **Keep a full backup** taken with 4-byte addressing (`tools/flash/gamepad_flash.py backup`), and a way
  to reflash the chip. A mistake in the hook or loader stops the stock firmware from booting too.
- **Never write a whole old dump back.** A dump read with 3-byte addressing has its upper 16 MiB
  mirrored from the lower half; writing it would destroy Service and everything above 16 MiB.
- **Firmware updates** (DRXUtil, or the console's) rewrite a bank and drop the hook: run `install-cfw`
  again. Our data in the gap stays.
- **Developer Mode has no password.** Anything on your network can read and write the pad's memory
  and flash while it is on (that is how `tools/menu_install.py` updates the menu). It is off on new
  installs; Settings > System > Developer Mode. File sharing without a password is open to your
  network too: set one in Settings > Network > File Sharing.
