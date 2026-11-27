# Migrating from the v1 fork (`feature/netfs`) to `feature/netfs-v2`

Our v1 fork ran on Nintendo's firmware through a loader; v2 is Raven's standalone
kernel (`kernel/`) with its own drivers and the WUPK ABI. **A `.wup` built for
v1 does not run on v2 and vice versa** — every app must be rebuilt from source.

## What changes on the pad

| | v1 (`feature/netfs`) | v2 (`feature/netfs-v2`) |
|---|---|---|
| Boot path | hook at `LVC_ + 0x63a0e` → loader at `0x10000` | early hook at `LVC_ + 0x40` + patches at `0x50408`/`0x50c9c` → kernel |
| Kernel/menu slots | menu header `0x30000`, image `0x31000-0x8FFFF` | kernel `0xB2000`/`0xB3000`, menu `0x10000`/`0x11000`, settings `0xB0000`/`0xB1000` |
| Memory model | firmware context, `WUP_BIGMEM` blob after the menu | flat ~1.8 MB app block; `WUP_STAGE` = screen-1 framebuffer |
| Runtime | firmware services | `kernel/` resident: scheduler, Wi-Fi, flash, camera, video decode |
| Apps | `.wup` v1 ABI | `.wup` v2 ABI (rebuild required) |

The v2 installer **restores Nintendo's instruction at `LVC_ + 0x63a0e`**
(`wup-menu/docs/architecture.md`), so the v1 hook is undone automatically.
The old v1 menu image at `0x30000-0x8FFFF` is simply left unused — harmless,
about 380 KiB of flash wasted; erase it only if you want the space back.

## What survives

- **littlefs is the same.** Both versions keep it in the unused Lang1/Lang2
  message-screen copy (`libwup/src/fs.c`). `/apps`, app data folders and
  `/config/*` — including `/config/netfs` (Network Storage settings),
  `/config/clock`, `/config/sharing` — are read by the same paths in v2.
  Your share configuration and installed-app data carry over.
- **Stock boot is preserved.** With A released (or Autoboot off) and a whole
  kernel + menu in flash, the early hook starts stock untouched.

## What does NOT survive

- **v1 `.wup` binaries.** Rebuild every app against v2 libwup. From this
  ecosystem: `netfs_browse`, `flash_backup` (in `libwup/examples/`), and
  `wup-stream` (builds unchanged — verified). WUP Boy is archived upstream;
  its v2 port is a separate project.
- **Wi-Fi may need re-joining.** v2's kernel owns the radio and keeps its own
  join state; re-enter the network in Settings if it does not come back.
- **v1's loader/bigmem machinery** (`big.c`, `WUPB` blob, `menu_slot.c` slot
  writes) does not exist in v2 — do not copy those files back.

## Procedure

1. **Back up first** (strongly recommended before any flash change):
   ```sh
   cd wup-menu
   python3 tools/flash/gamepad_flash.py backup        # full dump, 4-byte addressing
   ```
   Copy the backup folder somewhere safe.

2. **Uninstall v1** (Raven's recommendation): power off, hold **A** and power
   on to boot stock, then either leave it stock or go straight to step 3 —
   the v2 install undoes the v1 hook itself. If you want a clean pad first,
   restore the backup with `gamepad_flash.py restore <backup>`.

3. **Install the fork** — pick one:
   - **Wi-Fi** (pad already running a v2 menu with Developer Mode on):
     ```sh
     make -C ../kernel
     python3 ../libwup/tools/wupbuild.py menu
     WUP_PORT=tcp:<pad-ip> python3 tools/install.py
     ```
   - **UART** (from stock or v1): `python3 tools/flash/gamepad_flash.py install-cfw`
   - **Wii U installer**: `cd ../wup-menu && python3 tools/mkinstaller.py ../wup-menu-installer/payload/payload.bin`
     then `cd ../wup-menu-installer && make DEVKITPRO=$PWD/../devkitpro` → `.wuhb`.

4. **First boot**: power off, hold **A**, power on → Homebrew Menu. Check
   Settings > Network > Network Storage: the share settings should still be
   there (`/config/netfs` survived). Re-join Wi-Fi if needed, then run the
   share's "Test" row.

5. **Reinstall apps**: `/apps` survived, but any v1-built `.wup` inside will
   fail to launch — replace them with v2 builds (App Store, Netloader, or
   `wupbuild.py`).

## Updating the fork later

Software Update on the pad checks **this fork's** GitHub Releases
(`wozt/wup-homebrew`, `wup-menu/common/release.h`), never Raven's — an
upstream release would flash a menu without Network Storage. Fork releases
are tagged `v2.0.1-netfs.N` and ship `wup-homebrew-update.zip` built by
`python3 wup-menu/tools/mkrelease.py`.
