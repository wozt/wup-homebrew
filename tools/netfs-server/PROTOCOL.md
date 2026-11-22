# netfs protocol — version 1

A small framed binary TCP protocol between the Wii U GamePad (libwup's `src/netfs.c`)
and `netfs_server.py`. Read-only by default; `--read-write` additionally allows
OPEN-for-write and WRITE (uploads). Every message is length-framed, little-endian,
versioned, and size-capped. **The traffic is not encrypted**: it is meant for a trusted LAN.
The optional token is light authentication, not secrecy.

## Framing

Every message starts with an 8-byte header:

| offset | size | field |
|---|---|---|
| 0 | 1 | `cmd` (client→server) or `status` (server→client) |
| 1 | 1 | `ver` = 1 |
| 2 | 2 | reserved = 0 |
| 4 | 4 | `len` = payload bytes that follow (u32 LE) |

(struct `<BBHI`, 8 bytes)

Then `len` payload bytes. A response's `status` is 0 on success, otherwise a POSIX errno
value (`ENOENT` = 2, `EACCES` = 13, `EBADF` = 9, `EROFS` = 30, `ENOSYS` = 38, ...).

Limits: payload ≤ 1 MiB, path ≤ 512 bytes, one READ ≤ 256 KiB, one READDIR ≤ 512 entries,
connection idle timeout 30 s.

## Commands (client → server)

| cmd | name | request payload | response payload |
|---|---|---|---|
| 1 | AUTH | `token_len u8` + token bytes (0–255) | empty |
| 2 | PING | empty | empty |
| 3 | STAT | path (utf-8, starts with `/`) | `type u8` (0=reg, 1=dir), `size u64` |
| 4 | OPENDIR | path | `cookie u32` |
| 5 | READDIR | `cookie u32`, `max u32` | entries + `next u32` trailer |
| 6 | FREEDIR | `cookie u32` | empty |
| 7 | OPEN | path bytes + `flags u8` (bit0 = write, bit1 = create/truncate) | `handle u32`, `type u8`, `size u64` |
| 8 | READ | `handle u32`, `offset u64`, `len u32` | file bytes (short at EOF) |
| 9 | CLOSE | `handle u32` | empty |
| 10 | WRITE | `handle u32`, `offset u64`, then data bytes (`--read-write` only, else `EROFS`) | empty |
| 11 | DELETE | path bytes (`--read-write` only, else `EROFS`) | empty |

### READDIR entry format

Each entry: `nlen u8`, `type u8` (0=reg, 1=dir), `size u64`, then `nlen` name bytes
(NOT NUL-terminated, `.`/`..` never sent). After the last entry a `u32 next` trailer:
nonzero = same cookie still has entries (call READDIR again), 0 = end of directory.
The client parses entries until exactly 4 bytes remain.

### Session rules

- If the server was started with `--token`, the first command must be AUTH with the
  same token, else every other command gets `EPERM` (and AUTH gets `EACCES`).
- Handles and dir cookies live until CLOSE/FREEDIR or the connection drops.
- READ takes an **absolute offset**: the server keeps no per-handle position, so a
  client may read anywhere, any order, and reconnect (re-AUTH + re-OPEN) freely.
- Paths are resolved inside the configured root only: `..`, absolute escapes and
  symlinks pointing outside are refused (`EACCES`/`ENOENT`).
- WRITE takes an absolute offset like READ (`pwrite`); OPEN with bit0 set needs a
  server started with `--read-write`, otherwise `EROFS`. bit1 adds `O_CREAT|O_TRUNC`.
- DELETE removes a file, or an empty directory (`rmdir`); it also needs `--read-write`.

## Client behaviour (libwup)

- Lazy connect on first `/net` access; config from `/config/netfs` (flash).
- 32 KiB read cache (configurable) serving sequential reads; larger reads go direct.
- All socket ops bounded by timeouts; a broken connection drops every handle and the
  next operation reconnects (bounded retries, no infinite wait).
