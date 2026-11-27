# netfs-server

A small file server that gives the Wii U GamePad's `/net` mount its files — the Homebrew
Menu's **Network Storage** lists the `.wup` apps at the share's root and runs them straight
from it, and apps see the whole tree under `/net`.
See `PROTOCOL.md` for the wire format (version 1).

## Requirements

Python 3.8+ (standard library only), a folder to share.

## Run

```sh
./run.sh /path/to/share --host 0.0.0.0 --port 8734 --token SECRET --read-write
```

or directly:

```sh
python3 netfs_server.py --root /path/to/share --host 0.0.0.0 --port 8734 --token SECRET --read-write
```

- `--root DIR` — the only folder reachable (required). Put your `.wup` apps here: they show
  up in the menu's home screen under **Network Storage**.
- `--host` — default `127.0.0.1`; use `0.0.0.0` so the GamePad on the LAN can reach it.
- `--port` — default `8734`.
- `--token` — shared secret the GamePad's Network Storage settings must carry. Omit only
  on a trusted LAN.
- `--read-write` — allows uploads (OPEN for write + WRITE) and **DELETE** (removing apps
  from the share with `-` → Delete App in the menu). Without it the server is read-only:
  browsing and running apps work, uploads and deletes answer `EROFS`.

Example, the way it is usually run on the machine that holds the share:

```sh
python3 netfs_server.py --root /home/you/wup-share --host 0.0.0.0 --port 8734 --read-write
```

**The protocol is not encrypted.** Anyone on the network who knows (or guesses) the token
can read — and with `--read-write`, delete — every file under the root. Do not expose this
port to the Internet; prefer a LAN-only bind or a firewall rule.

## GamePad side

In the Homebrew Menu: Settings → Network → Network Storage → set the server IP, port and
token, enable it. The share's `.wup` files then appear on the home screen under
**Network Storage**: A runs one (its code streams into memory, nothing is kept on the
pad), `-` shows its info and offers Delete.

## Tests

```sh
python3 test_server.py && python3 test_client_proto.py
```

Covers framing, auth, directory walks, random reads, big files, uploads, deletes,
`..`/symlink escapes, disconnects and malformed input.
