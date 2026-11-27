#!/usr/bin/env python3
"""Wire-level test of the GamePad client (libwup/src/netfs.c) against the real server.

Replays exactly what the C code puts on the wire — AUTH as `len u8 + token`, STAT `<BQ`,
OPENDIR cookie, READDIR `<II` pages parsed as `<BBQ`+name until the 4-byte trailer,
OPEN `path + flags u8` answered `<IBQ`, READ `<IQI` at absolute offsets, CLOSE `<I>` —
plus the behaviours netfs.c relies on: absolute offsets in any order, a short read at
EOF, and EBADF for a handle from a dropped connection.

    python3 test_client_proto.py
"""
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
from test_server import pattern

VER = 1
HDR = struct.Struct('<BBHI')
CMD_AUTH, CMD_PING, CMD_STAT, CMD_OPENDIR, CMD_READDIR, CMD_FREEDIR, CMD_OPEN, CMD_READ, CMD_CLOSE = range(1, 10)
CMD_DELETE = 11


def recvn(s, buf, n):
    while len(buf[0]) < n:
        c = s.recv(1 << 16)
        if not c:
            raise ConnectionError('closed')
        buf[0] += c
    r, buf[0] = buf[0][:n], buf[0][n:]
    return r


class Pad:
    """the client half of netfs.c, message for message"""
    def __init__(self, port, token):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.s.settimeout(10)
        self.buf = [b'']
        t = token.encode()
        self.x(CMD_AUTH, bytes([len(t)]) + t)

    def x(self, cmd, payload=b''):
        self.s.sendall(HDR.pack(cmd, VER, 0, len(payload)) + payload)
        st, ver, _r, ln = struct.unpack('<BBHI', recvn(self.s, self.buf, HDR.size))
        assert ver == VER
        data = recvn(self.s, self.buf, ln) if ln else b''
        return st, data

    def ok(self, cmd, payload=b''):
        st, d = self.x(cmd, payload)
        assert st == 0, 'status %d cmd %d' % (st, cmd)
        return d

    # ---- what netfs.c does ----
    def stat(self, path):                          # remote_stat: '<BQ' -> type, size
        d = self.ok(CMD_STAT, path.encode())
        t, size = struct.unpack('<BQ', d)
        return t, size

    def open(self, path):                           # remote_open: path bytes + b'\x00' flags
        d = self.ok(CMD_OPEN, path.encode() + b'\x00')
        hid, typ, size = struct.unpack('<IBQ', d)   # 13 bytes: id, type, size
        return hid, typ, size

    def read_at(self, hid, off, ln):                # one READ: '<IQI'
        return self.x(CMD_READ, struct.pack('<IQI', hid, off, ln))

    def close(self):
        self.s.close()


class ClientProtoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix='netfs-cli-')
        os.makedirs(os.path.join(cls.root, 'ROMs'))
        with open(os.path.join(cls.root, 'hello.txt'), 'wb') as f:
            f.write(b'hello netfs\n')
        with open(os.path.join(cls.root, 'todelete.txt'), 'wb') as f:
            f.write(b'doomed\n')
        # 300 KiB: bigger than the 32 KiB cache window and than one READDIR page of names is irrelevant;
        # exercises both the cached-window path and straight-through reads
        with open(os.path.join(cls.root, 'ROMs', 'mid.bin'), 'wb') as f:
            left = 300 << 10
            while left:
                k = min(left, 1 << 16)
                f.write(pattern(k, 7))
                left -= k
        cls.real = os.path.join(cls.root, 'ROMs', 'mid.bin')
        cls.lsock = socket.socket()
        cls.lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        cls.lsock.bind(('127.0.0.1', 0))
        cls.port = cls.lsock.getsockname()[1]
        cls.lsock.listen(8)
        cls.th = threading.Thread(target=cls.loop, daemon=True)
        cls.th.start()
        # a second listener with --read-write, for DELETE/WRITE
        cls.lsock_rw = socket.socket()
        cls.lsock_rw.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        cls.lsock_rw.bind(('127.0.0.1', 0))
        cls.port_rw = cls.lsock_rw.getsockname()[1]
        cls.lsock_rw.listen(8)
        threading.Thread(target=cls.loop_rw, daemon=True).start()

    @classmethod
    def loop(cls):
        while True:
            try:
                sk, _ = cls.lsock.accept()
            except OSError:
                return
            threading.Thread(target=srv.Conn(sk, cls.root, 'tok').serve, daemon=True).start()

    @classmethod
    def loop_rw(cls):
        while True:
            try:
                sk, _ = cls.lsock_rw.accept()
            except OSError:
                return
            threading.Thread(target=srv.Conn(sk, cls.root, 'tok', rw=True).serve, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.lsock.close()
        cls.lsock_rw.close()
        shutil.rmtree(cls.root, ignore_errors=True)

    def pad(self):
        return Pad(self.port, 'tok')

    def test_stat_open_close(self):
        p = self.pad()
        t, size = p.stat('/hello.txt')
        self.assertEqual((t, size), (srv.T_REG, 12))
        t, _ = p.stat('/ROMs')
        self.assertEqual(t, srv.T_DIR)
        hid, typ, size = p.open('/hello.txt')
        self.assertEqual((typ, size), (srv.T_REG, 12))
        st, d = p.read_at(hid, 0, 5)
        self.assertEqual((st, d), (0, b'hello'))
        st, d = p.read_at(hid, 6, 100)                 # past EOF: short read
        self.assertEqual(d, b'netfs\n')
        p.ok(CMD_CLOSE, struct.pack('<I', hid))
        st, _ = p.read_at(hid, 0, 1)
        self.assertEqual(st, srv.errno.EBADF)
        p.close()

    def test_readdir_pages(self):
        p = self.pad()
        d = p.ok(CMD_OPENDIR, b'/ROMs')
        (cookie,) = struct.unpack('<I', d)
        # netfs.c asks 48 entries per page and parses <BBQ+name until 4 bytes remain
        seen = []
        for _ in range(16):
            st, d = p.x(CMD_READDIR, struct.pack('<II', cookie, 48))
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
        self.assertEqual(seen, ['mid.bin'])
        p.close()

    def test_random_reads_big_file(self):
        p = self.pad()
        size = os.path.getsize(self.real)
        hid, typ, rsize = p.open('/ROMs/mid.bin')
        self.assertEqual(rsize, size)
        # absolute offsets, any order — what fseek+fread becomes
        for off in (0, 1, size // 2, size - 100, size // 3, 32768 - 1, 32768):
            want = min(5000, size - off)
            st, d = p.read_at(hid, off, want)
            self.assertEqual(st, 0)
            self.assertEqual(len(d), want)
            with open(self.real, 'rb') as f:
                f.seek(off)
                self.assertEqual(d, f.read(want), 'mismatch at %d' % off)
        # sequential pass in 16 KiB steps (the cache window is 32 KiB: two READs per window)
        off = 0
        while off < size:
            want = min(16384, size - off)
            st, d = p.read_at(hid, off, want)
            self.assertEqual(st, 0)
            self.assertEqual(len(d), want)
            with open(self.real, 'rb') as f:
                f.seek(off)
                self.assertEqual(d, f.read(want))
            off += want
        p.ok(CMD_CLOSE, struct.pack('<I', hid))
        p.close()

    def test_delete(self):
        # the read-only listener refuses DELETE
        p = self.pad()
        st, _ = p.x(CMD_DELETE, b'/todelete.txt\x00')
        self.assertEqual(st, srv.errno.EROFS)
        p.close()
        # the --read-write one removes it; a second DELETE finds nothing
        p2 = Pad(self.port_rw, 'tok')
        p2.ok(CMD_DELETE, b'/todelete.txt\x00')
        self.assertFalse(os.path.exists(os.path.join(self.root, 'todelete.txt')))
        st, _ = p2.x(CMD_DELETE, b'/todelete.txt\x00')
        self.assertEqual(st, srv.errno.ENOENT)
        # a non-empty directory is refused too (rmdir)
        st, _ = p2.x(CMD_DELETE, b'/ROMs\x00')
        self.assertEqual(st, srv.errno.ENOTEMPTY)
        p2.close()

    def test_reconnect_loses_handles(self):
        p = self.pad()
        hid, _, _ = p.open('/hello.txt')
        p.close()                                    # link drops
        p2 = self.pad()                               # reconnect: AUTH again
        st, _ = p2.read_at(hid, 0, 4)
        self.assertEqual(st, srv.errno.EBADF)         # old handle is gone server-side
        hid2, _, _ = p2.open('/hello.txt')            # re-OPEN by path works
        st, d = p2.read_at(hid2, 0, 5)
        self.assertEqual(d, b'hello')
        p2.close()

    def test_server_unreachable_is_fast(self):
        # nothing listens here: the pad's connect must fail, not hang (bounded by CONN_MS)
        s = socket.socket()
        s.bind(('127.0.0.1', 0))
        dead = s.getsockname()[1]
        s.close()
        t0 = __import__('time').monotonic()
        try:
            Pad(dead, 'tok')
            self.fail('should not connect')
        except OSError:
            pass
        self.assertLess(__import__('time').monotonic() - t0, 10)


if __name__ == '__main__':
    unittest.main(verbosity=2)
