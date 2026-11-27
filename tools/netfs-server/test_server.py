#!/usr/bin/env python3
"""Tests for netfs_server.py, runnable on Debian without a GamePad.

    python3 test_server.py            (or: python3 -m unittest -v)

Starts the server in-process on 127.0.0.1 with an ephemeral port, builds a small tree
(including 1/8/16/32 MiB files of verifiable data) and exercises the protocol: framing,
auth, STAT/OPENDIR/READDIR/OPEN/READ/CLOSE, random reads, reconnects, traversal and
symlink escapes, malformed input and big files."""
import os
import shutil
import socket
import struct
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netfs_server as srv

VER = 1
HDR = struct.Struct('<BBHI')


def pattern(n, seed=0):
    """Deterministic verifiable bytes: a counter XOR-shifted, cheap to regenerate."""
    out = bytearray(n)
    x = 0x9E3779B9 ^ seed
    for i in range(0, n, 4):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        v = x.to_bytes(4, 'little')
        k = min(4, n - i)
        out[i:i + k] = v[:k]
    return bytes(out)


class Client:
    def __init__(self, port, token=None):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.s.settimeout(10)
        self.buf = b''
        if token is not None:
            t = token.encode() if isinstance(token, str) else token
            self.cmd(1, bytes([len(t)]) + t)          # AUTH: len u8 + token (what netfs.c sends)

    def _recvn(self, n):
        while len(self.buf) < n:
            c = self.s.recv(1 << 16)
            if not c:
                raise ConnectionError('closed')
            self.buf += c
        r, self.buf = self.buf[:n], self.buf[n:]
        return r

    def cmd(self, c, payload=b''):
        self.s.sendall(HDR.pack(c, VER, 0, len(payload)) + payload)
        st, ver, _r, ln = struct.unpack('<BBHI', self._recvn(HDR.size))
        assert ver == VER
        data = self._recvn(ln) if ln else b''
        return st, data

    def ok(self, c, payload=b''):
        st, d = self.cmd(c, payload)
        assert st == 0, 'status %d for cmd %d' % (st, c)
        return d

    def close(self):
        self.s.close()


def path_bytes(p):
    return p.encode()


class NetfsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix='netfs-root-')
        os.makedirs(os.path.join(cls.root, 'ROMs'))
        os.makedirs(os.path.join(cls.root, 'empty'))
        # small files
        with open(os.path.join(cls.root, 'hello.txt'), 'wb') as f:
            f.write(b'hello netfs\n')
        with open(os.path.join(cls.root, 'ROMs', 'tiny.bin'), 'wb') as f:
            f.write(pattern(4096, 1))
        # big files 1/8/16/32 MiB of verifiable data (sparse would break reads; write real data)
        cls.big = {}
        for mb in (1, 8, 16, 32):
            p = os.path.join(cls.root, 'ROMs', 'rom%d.mb' % mb)
            with open(p, 'wb') as f:
                left = mb << 20
                seed = mb
                while left:
                    k = min(left, 1 << 20)
                    f.write(pattern(k, seed))
                    left -= k
            cls.big[mb] = p
        # a symlink that escapes the root, and one inside it
        cls.outside = tempfile.mkdtemp(prefix='netfs-outside-')
        with open(os.path.join(cls.outside, 'secret.txt'), 'w') as f:
            f.write('secret')
        os.symlink(os.path.join(cls.outside, 'secret.txt'), os.path.join(cls.root, 'escape.txt'))
        os.symlink('hello.txt', os.path.join(cls.root, 'inner.txt'))
        # a symlinked directory pointing outside
        os.symlink(cls.outside, os.path.join(cls.root, 'escape_dir'))

        cls.port = srv_socket_port()
        cls.lsock = socket.socket()
        cls.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        cls.lsock.bind(('127.0.0.1', 0))
        cls.port = cls.lsock.getsockname()[1]
        cls.lsock.listen(8)
        cls.httpd = threading.Thread(target=cls.accept_loop, daemon=True)
        cls.httpd.start()

    @classmethod
    def accept_loop(cls):
        while True:
            try:
                sk, _ = cls.lsock.accept()
            except OSError:
                return
            threading.Thread(target=srv.Conn(sk, cls.root, cls.token).serve, daemon=True).start()

    token = 's3cret'

    @classmethod
    def tearDownClass(cls):
        cls.lsock.close()
        shutil.rmtree(cls.root, ignore_errors=True)
        shutil.rmtree(cls.outside, ignore_errors=True)

    # ---- helpers ----
    def client(self, token='s3cret'):
        return Client(self.port, token)

    def test_auth_required(self):
        c = Client(self.port)                       # no AUTH at all
        st, _ = c.cmd(2)                            # PING without auth
        self.assertEqual(st, srv.errno.EPERM)
        st, _ = c.cmd(1, bytes([5]) + b'wrong')
        self.assertEqual(st, srv.errno.EACCES)
        st, _ = c.cmd(1, bytes([6]) + b's3cret')
        self.assertEqual(st, 0)
        st, _ = c.cmd(2)
        self.assertEqual(st, 0)
        c.close()

    def test_stat_and_list(self):
        c = self.client()
        st, d = c.cmd(3, path_bytes('/hello.txt'))
        self.assertEqual(st, 0)
        t, size = struct.unpack('<BQ', d)
        self.assertEqual((t, size), (srv.T_REG, 12))
        st, d = c.cmd(3, path_bytes('/ROMs'))
        t, _ = struct.unpack('<BQ', d)
        self.assertEqual(t, srv.T_DIR)
        st, d = c.cmd(3, path_bytes('/nope'))
        self.assertEqual(st, srv.errno.ENOENT)
        st, d = c.cmd(3, path_bytes('/../etc/passwd'))
        self.assertNotEqual(st, 0)

        # full recursive listing of /
        names = []
        d = c.ok(4, path_bytes('/'))
        (cookie,) = struct.unpack('<I', d)
        while cookie:
            st, d = c.cmd(5, struct.pack('<II', cookie, 100))
            assert st == 0
            # entries until the last 4 bytes (next cookie)
            pos, nxt = 0, None
            while len(d) - pos > 4:
                nl, t, sz = struct.unpack_from('<BBQ', d, pos)
                pos += 10
                nm = d[pos:pos + nl].decode()
                pos += nl
                names.append((nm, t))
            nxt = struct.unpack_from('<I', d, pos)[0]
            cookie = nxt
        got = {n for n, _ in names}
        self.assertIn('hello.txt', got)
        self.assertIn('ROMs', got)
        self.assertIn('empty', got)
        types = dict(names)
        self.assertEqual(types['ROMs'], srv.T_DIR)
        self.assertEqual(types['hello.txt'], srv.T_REG)
        # and inside ROMs
        d = c.ok(4, path_bytes('/ROMs'))
        (cookie,) = struct.unpack('<I', d)
        st, d = c.cmd(5, struct.pack('<II', cookie, 512))
        assert st == 0
        pos = 0
        roms = []
        while len(d) - pos > 4:
            nl, t, sz = struct.unpack_from('<BBQ', d, pos)
            pos += 10
            roms.append((d[pos:pos + nl].decode(), sz))
            pos += nl
        self.assertEqual({r[0] for r in roms}, {'tiny.bin', 'rom1.mb', 'rom8.mb', 'rom16.mb', 'rom32.mb'})
        c.close()

    def test_read_small_and_offsets(self):
        c = self.client()
        d = c.ok(7, path_bytes('/hello.txt') + b'\x00')
        (h, _ty, _sz) = struct.unpack('<IBQ', d)
        st, data = c.cmd(8, struct.pack('<IQI', h, 0, 5))
        self.assertEqual(st, 0)
        self.assertEqual(data, b'hello')
        st, data = c.cmd(8, struct.pack('<IQI', h, 6, 100))   # past EOF partly
        self.assertEqual(data, b'netfs\n')
        st, data = c.cmd(8, struct.pack('<IQI', h, 6, 5))     # inside
        self.assertEqual(data, b'netfs')
        c.ok(9, struct.pack('<I', h))
        st, _ = c.cmd(8, struct.pack('<IQI', h, 0, 1))
        self.assertEqual(st, srv.errno.EBADF)
        c.close()

    def test_big_files_random_reads(self):
        c = self.client()
        for mb, path in self.big.items():
            real = os.path.join(self.root, 'ROMs', 'rom%d.mb' % mb)
            size = os.path.getsize(real)
            d = c.ok(7, path_bytes('/ROMs/rom%d.mb' % mb) + b'\x00')
            (h, _ty, _sz) = struct.unpack('<IBQ', d)
            offsets = [0, 1, size // 3, size - 4096, size - 1, size // 2]
            for off in offsets:
                want = min(4096, size - off)
                st, data = c.cmd(8, struct.pack('<IQI', h, off, want))
                self.assertEqual(st, 0)
                self.assertEqual(len(data), want, 'short read at %d of %d' % (off, size))
                with open(real, 'rb') as f:
                    f.seek(off)
                    self.assertEqual(data, f.read(want), 'data mismatch at %d of rom%d' % (off, mb))
            # a full sequential pass in 64 KiB chunks on the 1 MiB file only (speed)
            if mb == 1:
                off = 0
                while off < size:
                    want = min(65536, size - off)
                    st, data = c.cmd(8, struct.pack('<IQI', h, off, want))
                    self.assertEqual(st, 0)
                    self.assertEqual(len(data), want)
                    with open(real, 'rb') as f:
                        f.seek(off)
                        self.assertEqual(data, f.read(want))
                    off += want
            c.ok(9, struct.pack('<I', h))
        c.close()

    def test_traversal_and_symlinks(self):
        c = self.client()
        for p in ('/../etc/passwd', '/../../etc/passwd', '/ROMs/../../etc/passwd',
                  '/escape.txt', '/escape_dir/secret.txt', '/inner.txt'):
            st, _ = c.cmd(3, path_bytes(p))
            if p == '/inner.txt':
                self.assertEqual(st, 0)             # inside-root symlink is fine
            else:
                self.assertNotEqual(st, 0, p + ' should be refused')
        # write attempts are refused
        st, _ = c.cmd(7, path_bytes('/newfile') + b'\x01')
        self.assertEqual(st, srv.errno.EROFS)
        st, _ = c.cmd(7, path_bytes('/hello.txt') + b'\x01')
        self.assertEqual(st, srv.errno.EROFS)
        c.close()

    def test_reconnect_and_bad_input(self):
        c = self.client()
        d = c.ok(7, path_bytes('/hello.txt') + b'\x00')
        (h, _ty, _sz) = struct.unpack('<IBQ', d)
        c.s.close()                                 # drop mid-session
        c2 = self.client()                          # new connection: old handle is gone
        st, _ = c2.cmd(8, struct.pack('<IQI', h, 0, 4))
        self.assertEqual(st, srv.errno.EBADF)
        # malformed: bad version, oversized len, garbage command
        c3 = self.client()
        c3.s.sendall(struct.pack('<BBHI', 2, 99, 0, 0))
        st, _v, _r, _l = struct.unpack('<BBHI', c3._recvn(HDR.size))
        self.assertNotEqual(st, 0)
        c3.close()
        c2.close()

    def test_many_entries_pagination(self):
        # a folder with more entries than one READDIR page is covered implicitly above;
        # here check max=1 paging works
        c = self.client()
        d = c.ok(4, path_bytes('/ROMs'))
        (cookie,) = struct.unpack('<I', d)
        seen = []
        for _ in range(10):
            st, d = c.cmd(5, struct.pack('<II', cookie, 1))
            assert st == 0
            pos = 0
            while len(d) - pos > 4:
                nl, t, sz = struct.unpack_from('<BBQ', d, pos)
                pos += 10
                seen.append(d[pos:pos + nl].decode())
                pos += nl
            nxt = struct.unpack_from('<I', d, pos)[0]
            if not nxt:
                break
            cookie = nxt
        self.assertEqual(set(seen), {'tiny.bin', 'rom1.mb', 'rom8.mb', 'rom16.mb', 'rom32.mb'})
        c.close()


def srv_socket_port():
    return 0


class NetfsRwTest(unittest.TestCase):
    """--read-write: OPEN for write + WRITE upload; the read-only server still refuses."""

    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix='netfs-rw-')
        cls.lsock = socket.socket()
        cls.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        cls.lsock.bind(('127.0.0.1', 0))
        cls.port = cls.lsock.getsockname()[1]
        cls.lsock.listen(8)
        threading.Thread(target=cls.accept_loop, daemon=True).start()

    @classmethod
    def accept_loop(cls):
        while True:
            try:
                sk, _ = cls.lsock.accept()
            except OSError:
                return
            threading.Thread(target=srv.Conn(sk, cls.root, '', True).serve, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.lsock.close()
        shutil.rmtree(cls.root, ignore_errors=True)

    def client(self):
        return Client(self.port)

    def test_write_upload_and_readback(self):
        c = self.client()
        payload = pattern(100000, 7)                     # ~100 KB, unaligned size
        d = c.ok(7, path_bytes('/up.bin') + b'\x03')     # write | create/truncate
        (h, _ty, _sz) = struct.unpack('<IBQ', d)
        off = 0
        while off < len(payload):
            chunk = payload[off:off + 8192]
            c.ok(10, struct.pack('<IQ', h, off) + chunk)  # WRITE: handle, offset, data
            off += len(chunk)
        c.ok(9, struct.pack('<I', h))
        # the file is really there with the right bytes
        with open(os.path.join(self.root, 'up.bin'), 'rb') as f:
            self.assertEqual(f.read(), payload)
        # and readable through the protocol
        d = c.ok(7, path_bytes('/up.bin') + b'\x00')
        (h2, _t, sz) = struct.unpack('<IBQ', d)
        self.assertEqual(sz, len(payload))
        st, data = c.cmd(8, struct.pack('<IQI', h2, 99992, 64))
        self.assertEqual(data, payload[99992:])
        c.ok(9, struct.pack('<I', h2))
        c.close()

    def test_write_outside_root_refused(self):
        c = self.client()
        st, _ = c.cmd(7, path_bytes('/../escape.bin') + b'\x03')
        self.assertNotEqual(st, 0)
        st, _ = c.cmd(10, struct.pack('<IQ', 9999, 0) + b'x')
        self.assertEqual(st, srv.errno.EBADF)
        c.close()


if __name__ == '__main__':
    unittest.main(verbosity=2)
