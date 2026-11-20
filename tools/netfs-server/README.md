# netfs-server

A small read-only file server that gives the Wii U GamePad's `/net` mount its files.
See `PROTOCOL.md` for the wire format (version 1).

## Requirements

Python 3.8+ (standard library only), a folder to share.

## Run

```sh
./run.sh /srv/gamepad --host 0.0.0.0 --port 8734 --token SECRET
```

- `--root DIR` — the only folder reachable (default: required).
- `--host` — default `127.0.0.1`; use `0.0.0.0` so the GamePad on the LAN can reach it.
- `--port` — default `8734`.
- `--token` — shared secret the GamePad's Network Storage settings must carry. Omit only
  on a trusted LAN.

**The protocol is not encrypted.** Anyone on the network who knows (or guesses) the token
can read every file under the root. Do not expose this port to the Internet; prefer a
LAN-only bind or a firewall rule.

## GamePad side

In the Homebrew Menu: Settings → Network → Network Storage → set the server IP, port and
token, enable it. Apps then see the share at `/net` (e.g. `/net/ROMs/game.gbc`).

## Tests

```sh
python3 test_server.py
```

Covers framing, auth, directory walks, random reads, big files, `..`/symlink escapes,
disconnects and malformed input.
