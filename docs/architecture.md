<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# Architecture

## Scope

vcockpit targets software development without vehicle hardware: guest
VMs run on a desktop PC and their displays are composed into one
dashboard window. Isolation between the guests is the hypervisor's job;
the host compositor only arranges buffers it is given. It is not a
safety-certified design.

## Composition in two levels

```
 Guest VM ──► its own compositor (QNX Screen, guest Weston)
                 │  one or more scanouts per VM
                 ▼
 Host ──────► Weston with the IVI shell
                 places every scanout as a numbered surface
                 in a fixed zone, with an explicit stacking order
```

Each guest composes its own windows. The host composes the guests. A guest
may hand over one finished screen (QNX today) or several scanouts that the
host places separately (planned for the Linux guest).

## Frame path

1. The guest's graphics driver sends virtio-gpu commands to QEMU's
   `virtio-gpu-gl-pci` device; virglrenderer executes them on the host GPU.
2. QEMU runs with `-display dbus,gl=on`. The proxy registers a listener on
   each console with `RegisterListener`, which gives it a private D-Bus
   connection to QEMU per console.
3. QEMU calls `ScanoutDMABUF` on that connection, passing a dma-buf file
   descriptor with its size, stride, format and modifier.
4. The proxy wraps the descriptor in a `wl_buffer` with
   `zwp_linux_dmabuf_v1` and commits it to the surface for that console.
5. `ivi-id-agent` gives the surface its IVI ID from its title, and the
   layer configuration places it on screen.

The proxy refuses QEMU's pixel-copy methods (`Scanout`, `Update`) with an
error, so a guest that cannot provide dma-bufs fails visibly instead of
falling back to copying frames through the CPU.

## Surface IDs

| Surface title | IVI surface ID | Source |
|---|---|---|
| `QEMU (qemu-qnx-0)` | 1000 | QNX guest, console 0 |
| `QEMU (qemu-debian-0)` | 2000 | Debian test guest (reference runbook) |
| `QEMU (qemu-agl-0)` … `-2` | 2000, 2100, 2200 | planned: Linux guest with three scanouts |

The proxy titles its surfaces `QEMU (qemu-<vm-name>-<console index>)`, the
same format QEMU's SDL display uses, so the same `weston.ini` serves both.

## Design decisions

| Decision | Reason |
|---|---|
| One hypervisor, QEMU, for every guest | QNX needs full PC emulation with firmware, which lighter VMMs such as crosvm do not provide. |
| The display bridge is a separate process | A fault in the display path must not stop the hypervisor. |
| One proxy per VM, handling all of that VM's consoles | QEMU exposes one D-Bus display per instance; several clients would compete for it. |
| Weston with the IVI shell as host compositor | Layout is set in pixel coordinates per surface and layer. `agl-compositor` assigns roles (fullscreen, split, float) rather than coordinates, which does not give a fixed multi-zone layout without custom policy. |
| QNX contributes one pre-composed surface | The graphics stack in the non-commercial QNX SDP composes all QNX windows into one output; per-application output needs the newer DRM virtio stack. |
| Linux guest shows individual apps as separate host surfaces (planned) | With `max_outputs=N`, the guest sees N display connectors; its own Weston pins applications to outputs through `weston.ini`. |
| Keyboard focus follows the pointer (planned) | The proxy owns all of a VM's surfaces and receives `wl_pointer` enter and leave events for them. |
| Viewing through a host X11 window | Weston's x11 backend with the host GPU. Remote streaming is a later step. |
| QEMU's SDL display as reference path | Fastest way to prove the guest-to-host path end to end; the proxy replaces it. |

## Open questions

- Whether each VM's rendering should move into a separate
  `vhost-user-gpu` process, for isolation between guests that share the
  host GPU.
- Whether the frame path is zero-copy end to end. The proxy itself never
  copies; what QEMU and the compositor do with the buffers has not yet been
  measured ([roadmap](roadmap.md)).
