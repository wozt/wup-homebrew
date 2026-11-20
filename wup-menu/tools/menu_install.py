#!/usr/bin/env python3
"""Update the Homebrew Menu in the pad's SPI flash over Wi-Fi, through the menu's debug server (Developer Mode).

    WUP_PORT=tcp:192.168.4.100 python3 tools/menu_install.py                          the menu -> the menu slot (0x30000)
    WUP_PORT=tcp:192.168.4.100 python3 tools/menu_install.py loader --yes-boot-area   the loader too (boot critical)

The menu is an app image (menu, built like any app), stored deflated: header sector 0x30000 {"WMNU", stored size,
crc32, inflated size, stamp} written LAST, the image at 0x31000. Each 4 KB sector goes through an uncached buffer and an
on-pad helper (tools/flashwr.c) that erases, writes and verifies it; then the whole image is read back.
'menu' never touches flash outside the menu slot 0x30000-0x8FFFF. The first install (the hook, the loader) goes
over UART: tools/flash/gamepad_flash.py install-cfw."""
import os, struct, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import paths, wupcall          # (wupcall, wupmon: libwup's, found through paths)
from wupmon import WupMon
from mkflash import build_menu, menu_header, MENU_HDR, MENU_IMG, MENU_SLOT_END

BASE = 0x342000        # uncached RAM the menu does not use (libwup's audio ring): the helper, its arguments, buffers




class Flasher:
    def __init__(self, m, lo, hi, base=BASE):
        self.m, self.lo, self.hi = m, lo, hi
        self.args, self.check, self.stage = base + 0x400, base + 0x800, base + 0x1000
        code, syms = wupcall.build(os.path.join(HERE, 'flashwr.c'), base)
        assert len(code) < 0x400
        m.write(base, code); self.entry = syms['flashwr'] | 1

    def sector(self, fa, data):
        assert self.lo <= fa and fa + len(data) <= self.hi and fa % 0x1000 == 0 and 0 < len(data) <= 0x1000, hex(fa)
        self.m.write(self.stage, data)
        self.m.write(self.args, struct.pack('<IIII', fa, self.stage, len(data), self.check))
        r, _ = self.m.call(self.entry, self.args, timeout=30)
        if r: raise RuntimeError('flash sector 0x%x: helper returned %d' % (fa, r - (1 << 32) if r >= 1 << 31 else r))

    def region(self, fa, data, label):
        for o in range(0, len(data), 0x1000):
            self.sector(fa + o, data[o:o + 0x1000])
            print('\r  %s: %d / %d KB' % (label, min(o + 0x1000, len(data)) >> 10, (len(data) + 1023) >> 10), end='', flush=True)
        print()
        back = self.m.flash(fa, len(data), progress=False)
        if back != data: raise RuntimeError('%s: read-back differs' % label)


def main():
    a = sys.argv[1:]
    what = a[0] if a else 'menu'
    m = WupMon(os.environ.get('WUP_PORT', '/dev/cu.usbserial-0001'))
    if what == 'menu':
        menu, img, blob = build_menu()                           # an app image, stored deflated (+ its bigmem blob)
        crc = zlib.crc32(img) & 0xffffffff
        print('menu %d KB, stored %d KB, bigmem %d KB, crc %08x' % (len(menu) >> 10, len(img) >> 10, len(blob) >> 10, crc))
        f = Flasher(m, MENU_HDR, MENU_SLOT_END)
        f.sector(MENU_HDR, b'\xff' * 16)                       # invalidate first: a half-written image never looks valid
        f.region(MENU_IMG, img, 'image')
        if blob:
            at = MENU_IMG + ((len(img) + 0xfff) & ~0xfff)
            f.region(at, blob, 'bigmem')
        f.sector(MENU_HDR, menu_header(menu, img))
        print('menu installed at flash 0x%x (header 0x%x), verified' % (MENU_IMG, MENU_HDR))
    elif what == 'loader':
        # The loader and the menu together, over Wi-Fi (a new loader, a change of the flash layout). Any interruption
        # leaves a pad that boots the stock firmware (then: the UART install): 1. the menu's header goes; 2. the
        # loader's first sector is erased (no "CFW1": the boot hook passes it by), its second written, then its first;
        # 3. the menu's image; 4. the menu's header.
        if '--yes-boot-area' not in a: sys.exit('this writes the loader (boot critical): pass --yes-boot-area')
        from mkflash import build_loader, LOADER_FLASH
        menu, img, blob = build_menu(); loader, _ = build_loader()
        loader = loader.ljust(0x2000, b'\xff')
        crc = zlib.crc32(img) & 0xffffffff
        print('menu %d KB stored, loader %d bytes' % (len(img) >> 10, len(loader.rstrip(b'\xff'))))
        f = Flasher(m, LOADER_FLASH, MENU_SLOT_END)
        f.sector(MENU_HDR, b'\xff' * 16); print('1. menu header erased')
        f.sector(LOADER_FLASH, b'\xff' * 16); f.sector(LOADER_FLASH + 0x1000, loader[0x1000:]); f.sector(LOADER_FLASH, loader[:0x1000])
        if m.flash(LOADER_FLASH, 0x2000, progress=False) != loader: sys.exit('LOADER READ-BACK DIFFERS: the pad boots stock; install over UART')
        print('2. loader written and verified')
        f.region(MENU_IMG, img, '3. menu image')
        if blob: f.region(MENU_IMG + ((len(img) + 0xfff) & ~0xfff), blob, '3b. bigmem')
        f.sector(MENU_HDR, menu_header(menu, img)); print('4. menu header written: done. Power the pad off and on, holding A.')
    else: sys.exit(__doc__)


if __name__ == '__main__':
    main()
