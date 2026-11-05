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

## Next: finish the proxy

- **Clean shutdown.** Handle SIGTERM and SIGINT through the GLib main loop
  (`g_unix_signal_add`) instead of a raw `signal()` handler, release the
  surfaces and close the D-Bus connections, so that the surface leaves the
  compositor on exit.
- **Automatic keyboard focus.** On `wl_pointer` enter and leave, move
  keyboard focus to the surface under the pointer.
- **Input forwarding.** Pass `wl_keyboard` and `wl_pointer` events to QEMU
  through `org.qemu.Display1.Keyboard` and `org.qemu.Display1.Mouse`.
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
