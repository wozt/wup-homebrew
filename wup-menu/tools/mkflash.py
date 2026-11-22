#!/usr/bin/env python3
"""The custom firmware: what changes in the GamePad's flash, and a copy of a stock flash image with it (the emulator).

    python3 tools/mkflash.py STOCK.bin OUT.bin [/apps/x/x.wup=local.wup ...]

  Product1 LVC_ + 0x63a0e   bl get_boot_mode -> bl hook        (the firmware's boot-app selection)
  Product1 LVC_ + 0xb951c   the hook (boot/hook.S, over 160 bytes of unused WPS-CLI debug strings)
  0x10000                   the loader (boot/loader.c, runs at RAM 0x340000, "CFW1" header), up to 8 KB
  0x30000 / 0x31000         the menu slot: header {"WMNU", stored size, crc32, inflated size, stamp} / menu image
  littlefs                  optional files (tools/fsimage), e.g. apps
Everything else of ours is in the free gap 0x10000-0xFFFFF too (libwup/src/fs.c); Nintendo's partitions are never
touched beyond those two patches. Only LVC rev 7677 (NR4_12_20140805_release). A real pad gets the same through
tools/flash/gamepad_flash.py install-cfw (UART); a pad that runs the menu updates it with tools/menu_install.py.

The Wii U install (installer/) cannot reach the gap: a firmware update writes only the bank the pad is not running,
then switches the bank selector to it. So that firmware carries the same two patches plus the bundle (build_bundle),
after its last section at BUNDLE_OFF: the loader (0x0000), the menu's header sector (0x2000) and image (0x3000).
The hook and the loader look there first, in the bank that runs."""
import os, struct, subprocess, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths

HOOK_SITE, GET_BOOT_MODE = 0x63a0e, 0x30734
STUB_ADDR, STUB_MAX = 0xb951c, 0xb95bc - 0xb951c
LVC_SIZE = 0xbc29c
LOADER_RAM, LOADER_FLASH, LOADER_MAX = 0x340000, 0x10000, 0x2000   # the loader (in the free gap, libwup/src/fs.c)
MENU_HDR, MENU_IMG = 0x30000, 0x31000                        # the menu slot (boot/loader.c)
MENU_SLOT_END = 0xB0000                                       # the menu slot: header sector + image + bigmem blob, up to the settings sectors (0x90000-0xAFFFF is free: docs/architecture.md)
BANK1, SELECTOR = 0x100000, 0xf000
FW_DATA_SIZE = 0x2c8ea5                                       # firmware 7677: its sections end here (INDX .. IMG_)
BUNDLE_OFF = 0x2c9000                                         # the Wii U install's bundle, in the bank (boot/hook.S)
CFLAGS = ['-target', 'armv5te-none-eabi', '-mthumb', '-Os', '-ffreestanding', '-fno-builtin', '-nostdlib',
          '-fno-zero-initialized-in-bss', '-fno-common', '-Wall', '-Wno-unused-function', '-I', paths.INCLUDE, '-I', paths.BOOT, '-I', paths.GEN]


def thumb_bl(at, target):
    off = target - (at + 4)
    assert -(1 << 22) <= off < (1 << 22) and off % 2 == 0
    return struct.pack('<HH', 0xf000 | ((off >> 12) & 0x7ff), 0xf800 | ((off >> 1) & 0x7ff))


def build_loader():
    """boot/loader.c -> image for the loader slot ("CFW1" header first), linked at 0x340000"""
    out = paths.local_out('boot')
    o, elf, binf, ld = (os.path.join(out, 'loader' + x) for x in ('.o', '.elf', '.bin', '.ld'))
    import deps, sdk; deps.fetch('tinf'); sdk.ensure()
    tinf = os.path.join(deps.DEPS['tinf'][0], 'src')
    inc = ['-isystem', os.path.join(sdk.sysroot('thumb'), 'include'), '-I', tinf, '-DNDEBUG', '-ffunction-sections']   # (the C library's headers only)
    o2 = os.path.join(out, 'tinflate.o')
    subprocess.run(['clang'] + CFLAGS + inc + ['-DBUNDLE_OFF=0x%x' % BUNDLE_OFF, '-c', os.path.join(paths.BOOT, 'loader.c'), '-o', o], check=True)
    subprocess.run(['clang'] + CFLAGS + inc + ['-w', '-c', os.path.join(tinf, 'tinflate.c'), '-o', o2], check=True)
    open(ld, 'w').write('SECTIONS { . = 0x%x; .header : { KEEP(*(.header)) } .text : { *(.text*) } .rodata : { *(.rodata*) }'
                        ' .data : { *(.data*) *(.bss*) *(COMMON) } /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) } }\n' % LOADER_RAM)
    subprocess.run([paths.LD, '--gc-sections', '--entry=0', '-u', 'header_', '-T', ld, '-o', elf, o, o2], check=True)
    subprocess.run([paths.LD, '--gc-sections', '--entry=0', '-u', 'header_', '-T', ld, '--oformat', 'binary', '-o', binf, o, o2], check=True)
    img = open(binf, 'rb').read()
    assert img[:4] == b'CFW1', 'the loader does not start with its header'
    assert len(img) <= LOADER_MAX, 'the loader is %d bytes (%d fit)' % (len(img), LOADER_MAX)
    return img, struct.unpack_from('<I', img, 4)[0]


def build_stub():
    """boot/hook.S at STUB_ADDR"""
    out = paths.local_out('boot')
    o, elf, binf, ld = (os.path.join(out, 'hook' + x) for x in ('.o', '.elf', '.bin', '.ld'))
    defs = ['-DLAUNCHER_RAM=0x%x' % LOADER_RAM, '-DLOADER_FLASH=0x%x' % LOADER_FLASH, '-DLOADER_MAX=0x%x' % LOADER_MAX, '-DBUNDLE_OFF=0x%x' % BUNDLE_OFF]
    subprocess.run(['clang', '-target', 'armv5te-none-eabi', '-mthumb', '-c', os.path.join(paths.BOOT, 'hook.S'), '-o', o] + defs, check=True)
    open(ld, 'w').write('ENTRY(_start) SECTIONS { . = 0x%x; .text : { *(.text*) } }\n' % STUB_ADDR)
    subprocess.run([paths.LD, '-T', ld, '--oformat', 'binary', '-o', binf, o], check=True)
    stub = open(binf, 'rb').read()
    assert len(stub) <= STUB_MAX, 'hook stub too big (%d bytes, %d fit)' % (len(stub), STUB_MAX)
    return stub


MENU_DIR = os.environ.get('MENU_APP') or paths.MENU     # (MENU_APP: another app folder in the menu's place, for tests)


def build_menu():
    """the menu: an app like any other (tools/wupbuild.py), stored deflated in its slot -> (image, stored bytes,
    bigmem blob). The blob ({"WUPB", stored, raw, crc} + the code for WUP_BIGMEM, stored == raw: uncompressed —
    flash is cheap and the menu's heap cannot hold the deflated blob) goes right after the stored
    image on a 4 KB boundary: the menu is no .wup, so libwup's big.c finds it in the flash itself (src/big.c)."""
    import wupbuild, wupack
    (code, big), name, author, version, icon, description = wupbuild.build_app(MENU_DIR)
    menu = code                                             # the image (first segment + second, as one blob)
    stored = wupack.deflate(menu)
    blob = b''
    if big:
        blob = struct.pack('<4sIII', b'WUPB', len(big), len(big), zlib.crc32(big) & 0xffffffff) + big
    blob_at = MENU_IMG + ((len(stored) + 0xfff) & ~0xfff)
    if blob_at + len(blob) > MENU_SLOT_END: sys.exit('the menu (%d KB stored, %d KB bigmem) does not fit its slot (%d KB)' % (len(stored) >> 10, len(blob) >> 10, (MENU_SLOT_END - MENU_IMG) >> 10))
    return menu, stored, blob


def menu_header(menu, stored, stamp=None):
    """the menu's header sector: {"WMNU", stored size, crc32, inflated size, stamp}; the loader runs the newer stamp
    of the slot's and the bundle's (seconds; SOURCE_DATE_EPOCH or now)"""
    import time
    if stamp is None: stamp = int(os.environ.get('SOURCE_DATE_EPOCH') or time.time())
    return struct.pack('<4sIIII', b'WMNU', len(stored), zlib.crc32(stored) & 0xffffffff, len(menu), stamp)


BUNDLED_APPS = [os.path.join(paths.WUPMENU, 'apps', 'app_store'), os.path.join(paths.WUPMENU, 'apps', 'software_update')]


def build_apps():
    """the apps a Wii U install puts on the pad: [(id, version, .wup bytes)] (menu/bundle.c copies them on first start)"""
    import wupbuild, wupack
    out = []
    for d in BUNDLED_APPS:
        (code, name, author, version, icon, description) = wupbuild.build_app(d)
        out.append((name.replace(' ', '_').lower(), version, wupack.pack(code[0], name, author, version, icon, description, code[1])))
    return out


def build_bundle():
    """the Wii U install's bundle (BUNDLE_OFF in the bank): the loader (0x0000), the menu's header sector (0x2000) and
    image (0x3000), then the apps (menu/bundle.c): at the next 4 KB a table {"WAPS", count, then per app: id[32],
    version[16], offset from the bundle's start, size, crc32}, and the .wup files"""
    loader, _ = build_loader()
    menu, stored, blob = build_menu()
    b = bytearray(loader.ljust(0x2000, b'\xff') + menu_header(menu, stored).ljust(0x1000, b'\xff') + stored)
    b += b'\xff' * (-len(b) % 0x1000)                       # the bigmem blob on a sector boundary, like in the slot
    if blob: b += blob + b'\xff' * (-len(blob) % 0x1000)
    apps = build_apps()
    table = struct.pack('<4sI', b'WAPS', len(apps))
    at = len(b) + 0x1000
    files = b''
    for wid, version, data in apps:
        table += struct.pack('<32s16sIII', wid.encode(), version.encode(), at + len(files), len(data), zlib.crc32(data) & 0xffffffff)
        files += data + b'\xff' * (-len(data) % 4)
    assert len(table) <= 0x1000
    b += table.ljust(0x1000, b'\xff') + files
    assert BUNDLE_OFF + len(b) <= 0x400000, 'the bundle does not fit the bank (%d KB over)' % ((BUNDLE_OFF + len(b) - 0x400000) >> 10)
    return bytes(b)


def patches(read):
    """The install: the hook into Product1 in place, the loader and the menu into the free gap (libwup/src/fs.c).
    read(addr, n) gives the flash as it is. -> [(flash address, bytes)] in the order to write them: the menu (its
    header last), the loader, the hook stub, and last the bl that reaches it. Until that one the pad boots as before."""
    indx = read(BANK1, 0x70)
    off, size, name, _ = struct.unpack_from('<II4sI', indx, 32)
    if indx[8:12] != b'INDX' or name != b'LVC_' or size != LVC_SIZE: sys.exit('Product1 does not hold firmware 7677')
    lvc = BANK1 + off
    site, at = lvc + HOOK_SITE, lvc + STUB_ADDR
    cur = read(site, 4)
    if cur == thumb_bl(HOOK_SITE, GET_BOOT_MODE):
        dead = read(at, STUB_MAX)
        if not (dead.startswith(b'wpscli_del_wl_prov_svc_ie') and b'wpscli_set_wl_prov_svc_ie' in dead):
            sys.exit('the hook area does not hold the expected unused strings')
    elif cur != thumb_bl(HOOK_SITE, STUB_ADDR): sys.exit('unexpected boot code at LVC 0x%x' % HOOK_SITE)
    stub = build_stub()
    loader, _ = build_loader()
    menu, stored, blob = build_menu()
    hdr = menu_header(menu, stored)
    menu = stored
    blob_at = MENU_IMG + ((len(menu) + 0xfff) & ~0xfff)     # the bigmem blob, after the image on a sector boundary
    return [(MENU_IMG, menu), (blob_at, blob), (MENU_HDR, hdr), (LOADER_FLASH, loader), (at, stub.ljust(STUB_MAX, b'\0')),
            (site, thumb_bl(HOOK_SITE, STUB_ADDR))]


def add_files(f, files):
    import deps; deps.fetch('littlefs')
    tool = os.path.join(paths.local_out('host'), 'fsimage')
    subprocess.run([paths.host_cc(), '-O2', '-w', '-I', os.path.join(paths.THIRD, 'littlefs'), '-o', tool, os.path.join(HERE, 'fsimage.c'),
                    os.path.join(paths.THIRD, 'littlefs', 'lfs.c'), os.path.join(paths.THIRD, 'littlefs', 'lfs_util.c')], check=True)
    tmp = os.path.join(paths.local_out('host'), 'flash.tmp')
    open(tmp, 'wb').write(f)
    subprocess.run([tool, tmp, '--format'] + ['%s=%s' % pl for pl in files], check=True)
    f = bytearray(open(tmp, 'rb').read()); os.remove(tmp)
    return f


def build_image(stock, files=()):
    """stock flash bytes -> CFW flash bytes. files: [(pad path, local path)] put into the pad filesystem"""
    f = bytearray(stock)
    if f[SELECTOR] == 1: sys.exit('this dump boots Product2: expected a stock pad (Product1)')
    for a, d in patches(lambda x, n: bytes(f[x:x + n])): f[a:a + len(d)] = d
    return bytes(add_files(f, files) if files else f)


def main():
    a = sys.argv[1:]
    if len(a) < 2: sys.exit(__doc__)
    files = [tuple(x.split('=', 1)) for x in a[2:]]
    stock = open(a[0], 'rb').read()
    f = build_image(stock, files)
    open(a[1], 'wb').write(f)
    changed = [s for s in range(0, len(f), 0x1000) if f[s:s + 0x1000] != stock[s:s + 0x1000]]
    print('%s: %d sectors changed%s' % (a[1], len(changed), ', %d files' % len(files) if files else ''))


if __name__ == '__main__':
    main()
