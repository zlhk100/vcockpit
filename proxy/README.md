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

Guest frames are shown as opaque. A frame whose format has an alpha
channel (ABGR8888, ARGB8888 and their 2101010 forms) is imported as the
same format without it, logged once per format, and each surface's opaque
region covers the whole surface, so the compositor never blends a guest
picture with what was on screen before.

## Input

Pointer and keyboard events that the compositor delivers to a proxy
surface are forwarded to QEMU's `org.qemu.Display1.Mouse` and
`org.qemu.Display1.Keyboard` interfaces on the console that feeds the
surface: positions (absolute or relative, following QEMU's `IsAbsolute`),
buttons, the vertical wheel, and keys as QEMU key numbers from a fixed
table; keys not in the table are dropped and counted. A key is sent only
while the pointer is on the proxy's surface; keys arriving while the
pointer is elsewhere are not sent and are counted as gated keys, so with
keyboard focus on several surfaces the keyboard follows the pointer. Held
buttons and keys are released when the pointer or keyboard leaves the
surface and at shutdown, which prints the input counters. Keyboard focus
is given by the compositor (`ivi-input-set`, see
[running the proxy](../docs/running-the-proxy.md#6-input)).

Neither guest's cursor reaches the host, so the proxy sets its own cursor
on pointer enter: a 16x24 arrow, white with a black outline, hotspot at
its tip, drawn once at start-up into a `wl_shm` buffer.

## Source

| File | Contents |
|---|---|
| `src/main.c` | Options, start-up, main loop |
| `src/wayland.c` | Wayland connection, surfaces, dma-buf import, GLib source for the Wayland socket |
| `src/dbus.c` | Session bus, `ConsoleIDs`, `RegisterListener` and the peer-to-peer connections |
| `src/listener.c` | `org.qemu.Display1.Listener` implementation |
| `src/input.c` | Pointer and keyboard forwarding to `org.qemu.Display1.Mouse` and `Keyboard` |
