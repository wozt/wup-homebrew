#!/usr/bin/env python3
"""Host test of source/fwpatch.c, the installer's patcher, against the pad's own stock dump.

    python3 test/test_fwpatch.py STOCK_FLASH.bin [EMU_FLASH_OUT.bin]     (wup-menu: $WUPMENU, else ../wup-menu)

1. A stand-in for the console's drc_fw.bin: Product1's sections from the dump, in DRXUtil's format.
2. fwpatch (built for the host) makes the installer's firmware; it must refuse damaged and other firmware.
3. Its data must be the stock data with exactly the UART install's two patches (tools/mkflash.py) and the bundle.
4. EMU_FLASH_OUT: the dump as a Wii U install leaves it: the new firmware in Product2, the bank selector on it, the
   gap (0x10000-0xFFFFF) blank as on a pad that never had homebrew. Boot it in the emulator holding A."""
import os, struct, subprocess, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
WUPMENU = os.environ.get('WUPMENU') or os.path.join(os.path.dirname(ROOT), 'wup-menu')
sys.path.insert(0, os.path.join(WUPMENU, 'tools'))
import paths, mkflash, mkinstaller          # (wup-menu's)

def crc(b): return zlib.crc32(b) & 0xffffffff

def blob(data, version=0x190c0117, block=0x1000, seq=0x20):
    sub = bytearray(0x4000)
    for k, i in enumerate(range(0, len(data), 0x1000)): struct.pack_into('<I', sub, 4 * k, crc(data[i:i + 0x1000]))
    h = bytearray(0x1000)
    struct.pack_into('<I', h, 0, 0x01010000)
    for i in range(4): struct.pack_into('<I', h, 4 + 4 * i, crc(sub[i * 0x1000:(i + 1) * 0x1000]))
    struct.pack_into('<I', h, 0xffc, crc(h[:0xffc]))
    img = bytes(h) + bytes(sub) + data
    return struct.pack('>IIII', version, block, seq, len(img)) + img

def parse(b):
    ver, block, seq, size = struct.unpack_from('>IIII', b)
    assert size == len(b) - 16
    h, sub, d = b[16:16 + 0x1000], b[16 + 0x1000:16 + 0x5000], b[16 + 0x5000:]
    assert struct.unpack_from('<I', h, 0xffc)[0] == crc(h[:0xffc]), 'header CRC'
    for i in range(4): assert struct.unpack_from('<I', h, 4 + 4 * i)[0] == crc(sub[i * 0x1000:(i + 1) * 0x1000]), 'super CRC'
    for k, i in enumerate(range(0, len(d), 0x1000)): assert struct.unpack_from('<I', sub, 4 * k)[0] == crc(d[i:i + 0x1000]), 'sub CRC %d' % k
    return ver, block, seq, h, d

def main():
    stock = open(sys.argv[1], 'rb').read()
    out = os.path.join(ROOT, 'build', 'test'); os.makedirs(out, exist_ok=True)
    cli = os.path.join(out, 'fwpatch_cli')
    subprocess.run([paths.host_cc(), '-O2', '-Wall', '-Wextra', '-o', cli, os.path.join(HERE, 'fwpatch_cli.c'),
                    os.path.join(HERE, '..', 'source', 'fwpatch.c')], check=True)
    payload = os.path.join(out, 'payload.bin'); open(payload, 'wb').write(mkinstaller.build())
    data = stock[mkflash.BANK1:mkflash.BANK1 + mkflash.FW_DATA_SIZE]
    orig = os.path.join(out, 'drc_fw.test.bin'); open(orig, 'wb').write(blob(data))
    def run(src):
        dst = os.path.join(out, 'drc_fw.patched.bin')
        if os.path.exists(dst): os.remove(dst)
        r = subprocess.run([cli, src, payload, dst], capture_output=True, text=True)
        return r.returncode, r.stdout.strip(), dst
    # refusals
    bad = bytearray(blob(data)); bad[16 + 0x5000 + 0x1234] ^= 1
    for name, b in (('damaged', bad), ('other version', blob(data, version=0x190c0118)),
                    ('other LVC', blob(data[:0x200] + b'\0' + data[0x201:])), ('truncated', blob(data)[:-1])):
        p = os.path.join(out, 'drc_fw.bad.bin'); open(p, 'wb').write(b)
        rc, msg, _ = run(p)
        assert rc == 1, (name, rc, msg); print('refuses %-14s %s' % (name + ':', msg))
    rc, msg, dst = run(orig); assert rc == 0, msg; print(msg)
    new = open(dst, 'rb').read()
    ver, block, seq, h, d = parse(new)
    assert (ver, block, seq) == (0x190c0117, 0x1000, 0x20) and h[:4] == struct.pack('<I', 0x01010000)
    # expected: stock data, the UART install's patches applied where they land in Product1, then the bundle
    f = bytearray(stock)
    for a, b in mkflash.patches(lambda x, n: bytes(f[x:x + n])):
        if mkflash.BANK1 <= a < mkflash.BANK1 + 0x400000: f[a:a + len(b)] = b
    exp = bytes(f[mkflash.BANK1:mkflash.BANK1 + mkflash.FW_DATA_SIZE]).ljust(mkflash.BUNDLE_OFF, b'\xff') + mkflash.build_bundle()[:0]
    bundle = d[mkflash.BUNDLE_OFF:]
    assert d[:mkflash.BUNDLE_OFF] == exp, 'the firmware differs from stock + the two patches'
    diff = [i for i in range(mkflash.FW_DATA_SIZE) if d[i] != data[i]]
    print('changed in the firmware: %d bytes, %06x..%06x' % (len(diff), diff[0], diff[-1]))
    assert bundle[:4] == b'CFW1' and bundle[0x2000:0x2004] == b'WMNU'
    size, mcrc = struct.unpack_from('<II', bundle, 0x2004)
    assert crc(bundle[0x3000:0x3000 + size]) == mcrc
    at = (0x3000 + size + 0xfff) & ~0xfff                              # the apps the menu puts on the pad (menu/bundle.c)
    if bundle[at:at + 4] == b'WUPB':                                   # the bigmem blob may sit before the table (menu/bundle.c)
        at = (at + 16 + struct.unpack_from('<I', bundle, at + 4)[0] + 0xfff) & ~0xfff
    assert bundle[at:at + 4] == b'WAPS'
    for i in range(struct.unpack_from('<I', bundle, at + 4)[0]):
        wid, ver, off, n, c = struct.unpack_from('<32s16sIII', bundle, at + 8 + 60 * i)
        assert crc(bundle[off:off + n]) == c, wid
        print('  bundled: %s %s (%d KB)' % (wid.rstrip(b'\0').decode(), ver.rstrip(b'\0').decode(), n >> 10))
    print('bundle: loader %d bytes, menu %d KB: ok' % (len(bundle[:0x2000].rstrip(b'\xff')), size >> 10))
    if len(sys.argv) > 2:
        emu = bytearray(stock)
        emu[0x10000:0x100000] = b'\xff' * (0x100000 - 0x10000)        # a pad that never had homebrew
        emu[0x500000:0x900000] = d.ljust(0x400000, b'\xff')
        emu[0xf000] = 1                                               # what CCRCDCSoftwareActivate leaves
        open(sys.argv[2], 'wb').write(emu); print('%s: Product2 active, gap blank' % sys.argv[2])
    print('ok')

if __name__ == '__main__':
    main()
