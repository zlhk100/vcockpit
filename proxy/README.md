<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# automotive-dbus-proxy

Bridges QEMU's D-Bus display (`-display dbus,gl=on`) to a Wayland
compositor: each guest console becomes an `xdg_toplevel` surface whose
buffers are the guest's dma-bufs.

## Build

Dependencies: GLib and GIO (with `gio-unix-2.0`), `wayland-client`,
`wayland-protocols`, `wayland-scanner`, GBM, libdrm headers, Meson, Ninja.
On Ubuntu 22.04:

```bash
sudo apt install libglib2.0-dev libwayland-dev wayland-protocols \
    libgbm-dev libdrm-dev meson ninja-build
meson setup build && ninja -C build
```

## Run

```
automotive-dbus-proxy --vm-name <name> --wayland-socket <name-or-path> [--n-scanouts <1-8>]
```

- `--vm-name`: short name used in the surface title,
  `QEMU (qemu-<name>-<index>)`; map that title to an IVI surface ID in
  `weston.ini`.
- `--wayland-socket`: compositor socket, e.g. `wayland-1`.
- `--n-scanouts`: number of surfaces to create; reduced to the number of
  consoles QEMU reports.

QEMU must already be on the session bus as `org.qemu`
(`DBUS_SESSION_BUS_ADDRESS` must be set). Full sequence:
[docs/running-the-proxy.md](../docs/running-the-proxy.md).

## Behaviour

| QEMU call | Proxy action |
|---|---|
| `ScanoutDMABUF` | Import the descriptor as a `wl_buffer`, attach, damage and commit |
| `UpdateDMABUF` | Commit the current buffer with the damaged region, then reply |
| `Disable` | Detach the buffer |
| `Scanout`, `Update` (pixel copy) | Refused with an error |

## Source

| File | Contents |
|---|---|
| `src/main.c` | Options, start-up, main loop |
| `src/wayland.c` | Wayland connection, surfaces, dma-buf import, GLib source for the Wayland socket |
| `src/dbus.c` | Session bus, `ConsoleIDs`, `RegisterListener` and the peer-to-peer connections |
| `src/listener.c` | `org.qemu.Display1.Listener` implementation |
