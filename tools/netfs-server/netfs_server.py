#!/usr/bin/env python3
"""netfs-server: the read-only network filesystem the Wii U GamePad mounts at /net.

    python3 netfs_server.py --root /srv/gamepad [--host 0.0.0.0] [--port 8734] [--token SECRET]

Serves a folder over the small framed TCP protocol of PROTOCOL.md (version 1). One thread per
client; every request is length-framed and size-capped. The service is meant for a trusted LAN:
traffic is NOT encrypted (use --token as light authentication only, never expose it to the
Internet). Mutations need --read-write: OPEN for write, WRITE (uploads) and DELETE.
"""
import argparse
import errno
import hmac
import os
import socket
import stat as statmod
import struct
import sys
import threading

VER = 1
CMD_AUTH, CMD_PING, CMD_STAT, CMD_OPENDIR, CMD_READDIR, CMD_FREEDIR, CMD_OPEN, CMD_READ, CMD_CLOSE, CMD_WRITE = range(1, 11)
CMD_DELETE = 11
T_REG, T_DIR = 0, 1

MAX_PATH = 512
MAX_PAYLOAD = 1 << 20          # 1 MiB per message
MAX_READ = 256 * 1024          # one READ at a time
MAX_READDIR = 512              # entries per READDIR answer
IO_TIMEOUT = 30.0              # seconds of silence tolerated on a connection

HDR_REQ = struct.Struct('<BBH I')     # cmd, ver, rsv, len
HDR_REP = struct.Struct('<BBH I')     # status, ver, rsv, len


def os_error(code):
    """an OSError whose .errno is set (OSError(code) alone leaves it None)"""
    return OSError(code, os.strerror(code))


class Conn:
    """One client: framed request -> framed response, all under IO_TIMEOUT."""

    def __init__(self, sock, root, token, rw=False):
        self.sock = sock
        self.root = os.path.realpath(root)
        self.token = token.encode() if token else None
        self.rw = rw
        self.authed = self.token is None
        self.handles = {}       # id -> open file
        self.dirs = {}          # cookie -> os.scandir iterator + dirfd path
        self.next_id = 1
        self.buf = b''

    # ---- transport ----
    def _recvn(self, n):
        while len(self.buf) < n:
            if not self.buf:
                self.sock.settimeout(IO_TIMEOUT)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError('peer closed')
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _sendall(self, data):
        view = memoryview(data)
        while view:
            view = view[self.sock.send(view):]

    def reply(self, status, payload=b''):
        self._sendall(HDR_REP.pack(status if isinstance(status, int) else 0, VER, 0, len(payload)) + payload)

    # ---- path safety: inside root, no escaping through '..' or symlinks ----
    def resolve(self, path):
        if not isinstance(path, str) or not path.startswith('/') or len(path) > MAX_PATH or '\x00' in path:
            raise os_error(errno.EINVAL)
        real = os.path.realpath(os.path.join(self.root, path.lstrip('/')))
        if real != self.root and not real.startswith(self.root + os.sep):
            raise os_error(errno.EACCES)
        return real

    def open_read(self, path):
        real = self.resolve(path)
        # O_NOFOLLOW on the last component; the realpath check above covers the rest
        return os.open(real, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_CLOEXEC', 0))

    def open_write(self, path, flags):
        real = self.resolve(path)
        # flags bit0 = write, bit1 = create/truncate; only with --read-write
        if not self.rw:
            raise os_error(errno.EROFS)
        mode = os.O_WRONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_CLOEXEC', 0)
        if flags & 2:
            mode |= os.O_CREAT | os.O_TRUNC
        return os.open(real, mode, 0o644)

    # ---- commands ----
    def cmd_auth(self, p):
        if self.token is None:
            return 0
        # payload: token_len u8 + token bytes (what libwup's netfs.c sends)
        tok = p[1:1 + p[0]] if p else b''
        if hmac.compare_digest(tok, self.token):
            self.authed = True
            return 0
        return errno.EACCES

    def cmd_stat(self, p):
        real = self.resolve(p.rstrip(b'\x00').decode('utf-8', 'surrogateescape'))
        st = os.stat(real)          # follows links already proven inside root
        if statmod.S_ISDIR(st.st_mode):
            return 0, struct.pack('<BQ', T_DIR, 0)
        return 0, struct.pack('<BQ', T_REG, st.st_size)

    def cmd_opendir(self, p):
        real = self.resolve(p.rstrip(b'\x00').decode('utf-8', 'surrogateescape'))
        it = os.scandir(real)       # NotADirectoryError / FileNotFoundError raised here
        cookie = self.next_id
        self.next_id += 1
        self.dirs[cookie] = it
        return 0, struct.pack('<I', cookie)

    def cmd_readdir(self, p):
        if len(p) < 8:
            raise os_error(errno.EINVAL)
        cookie, want = struct.unpack_from('<II', p)
        want = min(want or MAX_READDIR, MAX_READDIR)
        it = self.dirs.get(cookie)
        if it is None:
            raise os_error(errno.EBADF)
        out = bytearray()
        n = 0
        try:
            while n < want:
                e = next(it)
                if e.name in ('.', '..'):
                    continue
                name = os.fsencode(e.name)
                if len(name) > 255:
                    continue
                try:
                    is_dir = e.is_dir(follow_symlinks=False)
                    size = e.stat(follow_symlinks=False).st_size if not is_dir else 0
                except OSError:
                    continue
                out += struct.pack('<BBQ', len(name), T_DIR if is_dir else T_REG, size & 0xFFFFFFFFFFFFFFFF) + name
                n += 1
        except StopIteration:
            del self.dirs[cookie]
            return 0, bytes(out) + struct.pack('<I', 0)
        return 0, bytes(out) + struct.pack('<I', cookie)

    def cmd_freedir(self, p):
        if len(p) >= 4:
            (cookie,) = struct.unpack_from('<I', p)
            it = self.dirs.pop(cookie, None)
            if it is not None:
                it.close()
        return 0, b''

    def cmd_open(self, p):
        if not p:
            raise os_error(errno.EINVAL)
        path, flags = p[:-1], p[-1]
        if flags & 1:                       # writable handle (--read-write only)
            fd = self.open_write(path.rstrip(b'\x00').decode('utf-8', 'surrogateescape'), flags)
        else:
            fd = self.open_read(path.rstrip(b'\x00').decode('utf-8', 'surrogateescape'))
        hid = self.next_id
        self.next_id += 1
        self.handles[hid] = fd
        try:
            st = os.fstat(fd)
            size = st.st_size if statmod.S_ISREG(st.st_mode) else 0
            typ = T_DIR if statmod.S_ISDIR(st.st_mode) else T_REG
        except OSError:
            size, typ = 0, T_REG
        return 0, struct.pack('<IBQ', hid, typ, size & 0xFFFFFFFFFFFFFFFF)

    def cmd_read(self, p):
        if len(p) < 16:
            raise os_error(errno.EINVAL)
        hid, off, ln = struct.unpack_from('<IQI', p)   # handle u32, offset u64, len u32 (netfs.c)
        ln = min(ln, MAX_READ)
        fd = self.handles.get(hid)
        if fd is None:
            raise os_error(errno.EBADF)
        os.lseek(fd, off, os.SEEK_SET)
        data = os.read(fd, ln)
        return 0, data

    def cmd_write(self, p):
        if len(p) < 12:
            raise os_error(errno.EINVAL)
        hid, off = struct.unpack_from('<IQ', p)         # handle u32, offset u64, then data
        fd = self.handles.get(hid)
        if fd is None:
            raise os_error(errno.EBADF)
        data = p[12:]
        if data:
            if hasattr(os, 'pwrite'):
                os.pwrite(fd, data, off)
            else:
                os.lseek(fd, off, os.SEEK_SET)
                os.write(fd, data)
        return 0, b''

    def cmd_close(self, p):
        if len(p) >= 4:
            (hid,) = struct.unpack_from('<I', p)
            fd = self.handles.pop(hid, None)
            if fd is not None:
                os.close(fd)
        return 0, b''

    def cmd_delete(self, p):
        # path bytes; files go, directories only when empty (rmdir refuses a non-empty one)
        real = self.resolve(p.rstrip(b'\x00').decode('utf-8', 'surrogateescape'))
        try:
            os.remove(real)
        except IsADirectoryError:
            os.rmdir(real)
        return 0, b''

    def dispatch(self, cmd, payload):
        if cmd == CMD_AUTH:
            return self.cmd_auth(payload)
        if not self.authed:
            return errno.EPERM
        if cmd == CMD_PING:
            return 0, b''
        if cmd == CMD_STAT:
            return self.cmd_stat(payload)
        if cmd == CMD_OPENDIR:
            return self.cmd_opendir(payload)
        if cmd == CMD_READDIR:
            return self.cmd_readdir(payload)
        if cmd == CMD_FREEDIR:
            return self.cmd_freedir(payload)
        if cmd == CMD_OPEN:
            return self.cmd_open(payload)
        if cmd == CMD_READ:
            return self.cmd_read(payload)
        if cmd == CMD_CLOSE:
            return self.cmd_close(payload)
        if cmd == CMD_WRITE:
            return self.cmd_write(payload) if self.rw else errno.EROFS
        if cmd == CMD_DELETE:
            return self.cmd_delete(payload) if self.rw else errno.EROFS
        return errno.ENOSYS

    def serve(self):
        try:
            while True:
                hdr = self._recvn(HDR_REQ.size)
                cmd, ver, _rsv, ln = HDR_REQ.unpack(hdr)
                if ver != VER or ln > MAX_PAYLOAD:
                    self.reply(errno.EPROTONOSUPPORT if ver != VER else errno.EFBIG)
                    return
                payload = self._recvn(ln) if ln else b''
                try:
                    r = self.dispatch(cmd, payload)
                    if isinstance(r, int):
                        status, data = r, b''
                    else:
                        status, data = r
                except OSError as e:
                    status, data = e.errno or errno.EIO, b''
                except (ConnectionError, struct.error):
                    return
                except Exception:
                    import traceback
                    traceback.print_exc()
                    return
                self.reply(status, data)
        except (ConnectionError, OSError, socket.timeout):
            pass
        finally:
            for fd in self.handles.values():
                try:
                    os.close(fd)
                except OSError:
                    pass
            for it in self.dirs.values():
                try:
                    it.close()
                except OSError:
                    pass
            self.sock.close()


def main():
    ap = argparse.ArgumentParser(description='netfs read-only file server for the Wii U GamePad')
    ap.add_argument('--root', required=True, help='the folder to share (real directory)')
    ap.add_argument('--host', default='127.0.0.1', help='address to listen on (default 127.0.0.1: LAN only, never the Internet)')
    ap.add_argument('--port', type=int, default=8734)
    ap.add_argument('--token', default='', help='shared secret; required when set (light auth, LAN only)')
    ap.add_argument('--read-write', action='store_true', help='allow OPEN for write + WRITE (uploads); default stays read-only')
    a = ap.parse_args()
    root = os.path.realpath(a.root)
    if not os.path.isdir(root):
        sys.exit('root is not a directory: %s' % root)
    if a.host not in ('127.0.0.1', '::1'):
        print('warning: listening on %s — the protocol is NOT encrypted; keep it on a trusted LAN' % a.host, file=sys.stderr)
    lsock = socket.socket(socket.AF_INET6 if ':' in a.host else socket.AF_INET, socket.SOCK_STREAM)
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind((a.host, a.port))
    lsock.listen(8)
    print('netfs serving %s on %s:%d%s' % (root, a.host, a.port, ' (token required)' if a.token else ''))
    try:
        while True:
            sock, peer = lsock.accept()
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass
            threading.Thread(target=Conn(sock, root, a.token, a.read_write).serve, daemon=True).start()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
