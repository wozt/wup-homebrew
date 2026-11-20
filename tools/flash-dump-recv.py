#!/usr/bin/env python3
"""Receive a GamePad flash dump from the flash_backup homebrew app.

    python3 tools/flash-dump-recv.py OUT.bin [--port 9998]

The app sends "WUPFLASH" + u32 size (little endian), then the raw 32 MiB image.
Run this first, then start Flash Backup on the GamePad. The result is a full
SPI flash image (both firmware banks, settings, littlefs) — keep it safe, it
contains the pairing key. Verify the size: it must be exactly 33554432 bytes."""
import argparse, socket, struct, sys, time

MAGIC = b'WUPFLASH'

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--port', type=int, default=9998)
    ap.add_argument('--host', default='0.0.0.0')
    a = ap.parse_args()

    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind((a.host, a.port))
    ls.listen(1)
    print('waiting for the GamePad on port %d ...' % a.port, flush=True)
    c, addr = ls.accept()
    c.settimeout(30)
    print('connected from %s' % (addr[0],), flush=True)

    hdr = b''
    while len(hdr) < 12:
        chunk = c.recv(12 - len(hdr))
        if not chunk: sys.exit('connection closed before header')
        hdr += chunk
    if hdr[:8] != MAGIC: sys.exit('bad magic: %r' % hdr[:8])
    size = struct.unpack('<I', hdr[8:12])[0]
    print('expecting %d bytes (%.1f MiB)' % (size, size / 1048576), flush=True)

    t0 = time.time(); got = 0
    with open(a.out, 'wb') as f:
        while got < size:
            chunk = c.recv(min(1 << 20, size - got))
            if not chunk: sys.exit('connection closed at %d/%d bytes' % (got, size))
            f.write(chunk); got += len(chunk)
            if got % (8 << 20) < (1 << 20):
                print('  %d / %d MiB  (%.0f KiB/s)' % (got >> 20, size >> 20,
                      got / max(time.time() - t0, 0.001) / 1024), flush=True)
    c.close(); ls.close()
    print('done: %s (%d bytes, %.1f s)' % (a.out, got, time.time() - t0), flush=True)

if __name__ == '__main__':
    main()
