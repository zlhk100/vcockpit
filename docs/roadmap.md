<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# Roadmap

## Done

- Host compositor in a container: Weston with the IVI shell on the host
  GPU, shown in a host X11 window.
- Reference path: a QNX guest and a Debian guest side by side through
  QEMU's SDL display (`runbooks/qnx-probe-launch.sh`), with layer
  placement and input focus.
- D-Bus proxy: a QNX guest rendered through QEMU's `-display dbus,gl=on`
  into IVI surface 1000 at 1280x720, frames imported as dma-bufs.
- Proxy clean shutdown: SIGINT and SIGTERM end the GLib main loop
  (`g_unix_signal_add`); the proxy closes its D-Bus connections and
  removes its surface from the compositor before it exits.
- Input forwarding: the proxy passes `wl_pointer` and `wl_keyboard` events
  to QEMU through `org.qemu.Display1.Mouse` and `org.qemu.Display1.Keyboard`,
  and draws its own pointer cursor.
- Two guests side by side: a Debian guest next to the QNX guest, each with
  its own QEMU, D-Bus bus and proxy, in two scaled zones. Both surfaces
  have keyboard focus and each proxy sends keys only while the pointer is
  on its surface, so the keyboard follows the pointer
  ([running the proxy](running-the-proxy.md#7-two-guests-side-by-side)).

## Next: finish the proxy

- **Launch script.** One script for compositor, guest, proxy and layer
  placement, checking each step, verified on three consecutive clean runs.

## Known issues

- `proxy/meson.build` uses fixed paths to the wayland-protocols XML files
  (Ubuntu 22.04 layout); it should take them from the `wayland-protocols`
  pkg-config variable.
- `proxy/src/wayland.c` calls `memfd_create` through its x86_64 syscall
  number.
- The render node `/dev/dri/renderD128` is fixed in `proxy/src/main.c`,
  and the container device nodes are fixed in the docs and runbook.
- At start-up the proxy allocates a GBM test buffer before the first
  guest frame arrives; it is not needed once QEMU delivers frames.
- The proxy uses console 0 for every scanout; more than one console per VM
  needs per-console state in the D-Bus listener.
- Keyboard focus is lost when the compositor's window loses focus on the
  host desktop and is not restored when it regains it;
  `runbooks/keyboard-focus-watch.sh` re-applies it as a workaround.
- Copies of the proxy's cursor can stay on screen where the pointer has
  been; the cause is not established.
- The Debian picture stopped updating once after about three minutes; it
  has not recurred since. See
  [running the proxy](running-the-proxy.md#known-issues).

## Later

- **Linux guest with several surfaces.** A guest with `max_outputs=3`,
  its own Weston pinning applications to outputs, the proxy forwarding
  three scanouts, and the host placing them as navigation, infotainment and
  an overlay with the highest stacking order.
- **Dashboard content.** Instrument cluster on the QNX side; navigation,
  infotainment and driver-assistance overlay on the Linux side.
- **Frame path measurement.** Confirm whether buffers stay in GPU memory
  end to end; measure frame latency and jitter with both guests at 60 fps;
  evaluate virglrenderer's thread-synchronised fences.
- **Per-application QNX surfaces.** Needs QNX's DRM virtio graphics stack,
  which is outside the non-commercial SDP.
