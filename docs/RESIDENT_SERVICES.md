# Resident network services — study for a shared lwIP / Tailscale (MicroLink)

Status: **study only** — nothing here is implemented. Goal: what it would take
to run one TCP/IP stack (and later a Tailscale client) shared by the menu and
all apps, instead of each app bringing its own lwIP.

## What already is resident

More than it looks. The **radio and its driver are already in the kernel**:

- `kernel/src/wifi.c` + `kernel/src/radio.c`: Broadcom SDIO firmware load,
  SDPCM, ioctls/iovars, WPA2 join (`wifi_join` takes the PMK — the menu
  derives it), link state, MAC.
- `KAPI` (`libwup/include/wupk.h`, version 4): `wifi_up`, `wifi_ioctl`,
  `wifi_var`, `wifi_join`, `wifi_leave`, `wifi_state`, `wifi_tx`,
  `wifi_tx_room`, `wifi_hooks(rx, link)`, `mac`.

Every app gets **raw Ethernet frames** through `wifi_hooks` — there is no
per-app Wi-Fi state to duplicate. What is *not* shared is everything above
layer 2.

## What is per-app today

`libwup/src/net.c` + `src/wup_netif.c`: a full lwIP instance per app —
`tcpip` thread(s), `netif` bound to the kernel's frame hooks, `MEM_SIZE`
heap + all `MEMP_*` pools in the app's `.bss.net`, sockets as fds 3..14.
Costs, per app that uses `net = yes`:

- roughly **100–200 KB** of the app's ~1.8 MB block (pools + heap + thread
  stacks), plus join/teardown latency on every app start/exit.
- `netfs.c` rides on top of this (a `/net` VFS over one TCP connection to
  the share) — it is already stack-agnostic: it only needs `socket/connect/
  send/recv/close`, so it would survive a resident-stack migration unchanged.

The menu itself is an app: its FTP server, Netloader, wuplink, debug server,
SNTP and NetFS share screens all live in *its* lwIP instance.

## The memory wall

RAM map (`kernel/README.md`, `libwup/include/wup_ram.h`), 4 MiB total:

| Region | Size | Free? |
|---|---|---|
| kernel: code/data/stacks/heap | `0x0–0x7f000` (~500 KB) | kernel heap only: image ~73 KB + stacks ⇒ **roughly 300–350 KB free** |
| hand-off block | `0x7f000` | fixed |
| app block | `0x80000–0x253000` (~1.8 MB) | the running app's; empty between apps |
| sound/mic rings | `0x253000+` | small, in use |
| screens | `0x26fb00+` (~1.6 MB) | FB0/FB1; FB1 doubles as stage *between* apps only |

**There is no unallocated RAM region.** A resident lwIP must live in the
kernel heap (~300 KB) — feasible for lwIP core + modest pools (current app
defaults already fit in ~150 KB), tight once Tailscale's userspace
(WireGuard crypto, DERP, DNS, state) is added on top. Options, in increasing
intrusiveness:

1. **Shrink pools globally** (`lwipopts.h` overrides exist: `WUP_NET_*`)
   and run the resident stack in kernel heap. Cheapest; caps concurrent
   connections.
2. **Grow the kernel region**: move `KERNEL_END`/`WUP_APP_START` — but the
   app block is fixed-size by the ABI and the screens sit right above;
   there is nowhere to take the memory from without shrinking the app
   block (which hurts every app).
3. **Resident service as a kernel task with a carved arena**: e.g. reserve
   part of the app block *before* the app loads? No — the app owns the
   whole block. Between apps the block is free, but a service must run
   *while* apps run.
4. **Service in the menu's context**: the menu is an app too; it dies when
   an app runs. Dead end unless the kernel hosts it.

Honest assessment: a shared **lwIP** fits in kernel heap with discipline.
A shared **Tailscale** does not — MicroLink-class clients need hundreds of
KB to MB of state plus a UDP data plane; on 4 MiB with two screens pinned
there is no room without a bigger architectural change (e.g. compressing
screens, a smaller app block, or external RAM — none exists).

## What the ABI would need (WUPK v5 sketch)

The table grows only at its end (`wupk.h`); `version` gates features. A
resident stack implies:

- **Ownership**: sockets/connections belong to an app instance. The kernel
  must tag each connection with the owning app (task/app id) and tear them
  down when the app exits — or explicitly hand them off (menu → app).
- **API shape**: either (a) keep POSIX sockets and have libwup's `net.c`
  become a *client* of a kernel-resident lwIP via a control channel
  (ioctl-style, like `wifi_ioctl`), or (b) move the whole `netif`+tcpip
  into a kernel task and expose `socket/bind/connect/send/recv` as KAPI
  calls. (a) reuses lwIP unchanged in kernel space; (b) is more code in
  kernel but keeps the ABI thin.
- **Frame ownership**: today `wifi_hooks` delivers every frame to the
  app's lwIP. With a resident stack the kernel's netif consumes them;
  apps never see Ethernet frames (good — `wup_netif.c` disappears from
  apps).
- **NetFS compatibility**: `netfs.c` needs no change — it sits on POSIX
  sockets. Its reconnect/timeout logic is per-file-handle and unaffected.
- **Multiple apps**: the menu's services (FTP/Netloader) and an app's
  sockets would share one stack — need per-app socket tables in kernel
  or a proxy daemon. Single-client-at-a-time is the pragmatic v1 of this.

## Routing and lifecycle constraints

- The kernel already reaps app state on exit (`wifi`/`audio`/`video`/`cam`
  all "stopped whenever an app ends", `wupk.h`). A resident netif must
  *not* be reaped — it belongs to the kernel, like the radio.
- Wi-Fi join is app-driven today (`wifi_join` by whoever connects first).
  A resident stack should own join state: the menu joins once, apps inherit
  connectivity. `wifi_state` already reports it.
- DHCP: `wup_netif.c` does static config today ("no DHCP" comment in
  `net.c`). A resident stack could add DHCP once, benefiting all apps.
- Power management: the radio must stay up across app switches (it does —
  kernel-owned), but `wifi_leave` on app exit must go if apps stop owning
  the link.

## Suggested order (when we get there)

1. Measure: actual kernel-heap free (`kheap_free` exists — log it at boot).
2. Prototype resident lwIP *without* changing the app ABI: kernel task +
   netif, apps keep their own stacks (both can coexist — frames would need
   demuxing by ethertype or a flag day).
3. KAPI v5: socket proxy API; libwup `net.c` switches to it; `net = yes`
   shrinks to a stub. `netfs.c` untouched.
4. Only then evaluate Tailscale/MicroLink — and realistically it needs a
   memory plan first (the 4 MiB wall above).

## References

- `kernel/src/wifi.c`, `kernel/src/radio.c`, `kernel/src/kapi.c`
- `libwup/include/wupk.h` (ABI, version 4), `libwup/include/wup_ram.h`
- `libwup/src/net.c`, `libwup/src/wup_netif.c`, `libwup/config/lwipopts.h`
- `libwup/src/netfs.c` (the client that must survive the migration)
- https://github.com/CamM2325/microlink (evaluation deferred per plan)
