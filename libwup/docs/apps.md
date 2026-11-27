# The Homebrew Menu and apps

## The menu

- **Hold A** while turning the pad on, or set Settings > System > Boot to Autoboot. Without that the pad boots the
  stock firmware. Press POWER to turn it off.
- **A** opens the selected app, **X** Settings, **Y** the Netloader, **B** goes back.
- Settings: **Network** (Wi-Fi, File Sharing), **Appearance**, **Brightness** (10 to 100 %; the console's own setting is never touched), **System** (Boot, Date & Time, Developer Mode, firmware, storage).
- **File Sharing** is FTP on port 21 (passive): `ftp://<pad IP>` in any FTP app. With a password the user name is
  `pad`. Apps copied to `/apps` appear by themselves.
- **App Store** and **Software Update** come with the Wii U install: apps from the project's catalogue, and newer
  Homebrew Menus. The pad's storage is 8 MB.

Apps live where nx-hbmenu keeps them on a Switch:

```
/apps/<name>.wup                    a single-file app
/apps/<dir>/<dir>.wup               an app with its own folder; its files live next to it
```

## Writing an app

An app is a folder with an `app.cfg` and C sources (`examples/hello`):

```
name = Hello World
author = you
version = 1.0
description = One line for the menu
icon = icon.png          # 64x64, optional
lvgl = yes               # the GUI library
net = yes                # sockets
```

```
python3 tools/wupbuild.py examples/hello                       # -> build/apps/hello_world.wup
python3 tools/wuplink.py build/apps/hello_world.wup            # keep it in /apps and start it (Y on the pad first)
python3 tools/wuplog.py                                        # its printf lines
```

It defines `WUP_APP("Title")` and `main()`; returning from `main` goes back to the menu. Other `app.cfg` keys
(sources, include, defines, large, thumb...) are listed at the top of `tools/wupbuild.py`.

| | |
|---|---|
| **C library** | picolibc: `printf` goes to the log; files are on the pad's filesystem, relative paths in the app's own folder (`fopen("save.dat", "wb")`); `malloc` has what is left of about 1.4 MB |
| **The pad** (`wup.h`) | `wup_screen()` (854x480 RGB565, with `gfx.h` for plain drawing), `wup_buttons()`, `wup_stick()`, `wup_touch()`, `wup_audio_*` (48 kHz stereo), `wup_rumble()`, `wup_backlight()`, `wup_mic_*`, `wup_battery()`, `wup_us()`, `wup_sleep()` |
| **GUI** (`lvgl = yes`, `wup_lvgl.h`) | `wup_lvgl_init()`, then everything in `<lvgl.h>`; `wup_lvgl_run(ms)` in the loop. Touch is a pointer, the buttons a keypad |
| **The menu's look** (`kit = yes`, `wup_kit.h`) | pages with the menu's bars, status and button hints, rows, notes, a spinner, outcomes and the empty state, on a stack of screens: `kit_start(home); while (kit_step()) {}`. It brings LVGL. App Store, Software Update and WUP Boy are made with it |
| **Threads** | `<pthread.h>` (threads, mutexes, condition variables, once, keys), `<semaphore.h>`, `usleep`, `clock_gettime`. Up to 12 threads; threads of one priority do not take turns by themselves: one runs until it waits or sleeps |
| **Network** (`net = yes`) | `wup_net_init()` takes over the menu's Wi-Fi connection (no Wi-Fi code in the app), then BSD sockets from any thread: `socket`, `connect`, `bind`, `listen`, `accept`, `send`, `recv`, `select`, `poll`, `getaddrinfo`... `127.0.0.1` works without Wi-Fi |
| **Time** | `time()`, `localtime()`, `gettimeofday()`: the date and time once the menu has had them from the network (Wi-Fi), in the time zone set in Settings › System › Date & Time; `wup_time_known()` says whether it has. `CLOCK_MONOTONIC` counts from the app's start. The pad keeps no time while it is off |
| **HTTPS** (`https = yes` with `net = yes`; `text_in_big = bearssl` saves room) | `wup_https_get(url, opts, body, arg)` (`wup_https.h`): a GET with BearSSL (TLS 1.2, modern ciphers), redirects followed, the body handed over as it arrives. Certificates are checked against 22 of Mozilla's roots (`wup_roots.h`) or the app's own, and the clock (`wup_time_known()`). `tests/https` tries real sites |
| **Large apps** (`large = yes`) | screen 0's 820 KB joins the heap (one screen, shown as it is drawn), mapped write-back while the app runs; on the 8-bit screen, `wup_large_take_screen_rest()` adds the rest of screen 1 (404 KB). TLSF. Not with LVGL. For ports of large programs (RetroArch) |
| **C++** (`.cpp` sources) | C++17 without exceptions or RTTI, libc++'s headers, static constructors; `printf = float` or `double` for `%f` |
| **Shared libraries** (`dl = yes`, `<dlfcn.h>`) | `dlopen`, `dlsym`, `dlclose`, `dlerror`: ARM ELF shared objects loaded into the heap and bound to the app's own C library and runtimes. A library is a folder with `shared = yes` (it builds `<name>.so`); the app lists them in `libraries =` and what they import in `dl_exports = @.../imports.txt`. RetroArch's cores are loaded this way |
| **Another app** | `wup_exec(path, arg)` |
| **Unzip** | `wup_inflate()`: raw deflate (a zip entry) |

Call `wup_sleep()` (or `wup_lvgl_run()`) in every loop: it gives the kernel's other tasks a turn (and runs
`net = raw` networking).

**Size.** The pad has 4 MB of RAM. Every app gets one flat block of about 1.8 MB (0x80000-0x253000,
`include/wup_ram.h`): its code, constants and data, its `.bss`, and the rest is its heap. The screens and the sound
rings are the kernel's, outside it. (Version 1's `large`, `screen8` and `text_in_big` settings mean nothing now: one
layout.)

## Tests

`tests/` holds apps that check the runtime, one line per check on the screen and in the log: `sdk` (C library,
files), `lvgl`, `exec` (one app starting another), `kernel` (threads, semaphores), `pthread`, `sockets`, `input` (every input live: buttons, sticks, touch, motion,
volume, power; A rumbles). Run one
like any app, on the pad or in the emulator:

```
python3 tools/wupbuild.py tests/sockets
python3 ../wup-menu/tools/mkflash.py ../wup-firmware/flash.bin build/wup.bin /apps/sockets_test/sockets_test.wup=build/apps/sockets_test.wup
../wup-emu/wup-emu --flash build/wup.bin --hold a 0 3000   # (wup-menu's mkflash puts wup-homebrew and the app in a copy of your dump)
```
