<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# vcockpit

A vehicle cockpit is one screen that shows the work of several systems
which must stay isolated from each other: a safety-relevant instrument
cluster next to an infotainment stack that is updated weekly. The display
is where they meet, so the display path decides whether the isolation
survives. vcockpit is a proof of concept that composes the output of
separate virtual machines into one dashboard on a desktop PC, so that
software for that dashboard can be developed and integrated before the
vehicle hardware exists.

Each guest VM renders with its own virtual GPU. Its frames leave QEMU as
dma-buf file descriptors, a small proxy process hands them to a host
compositor as Wayland surfaces, and the compositor places each surface in
a fixed zone of the dashboard with an explicit stacking order.

> **Status: work in progress.** A QNX guest renders live into the host
> compositor through the D-Bus proxy. Keyboard and mouse input are not yet
> forwarded through the proxy, and the proxy has no verified launch script
> yet. See [docs/roadmap.md](docs/roadmap.md).

## How it works

```
 QNX guest (QNX Screen, gpu_virtio)
        │ virtio-gpu commands
        ▼
 QEMU  ─ virtio-gpu-gl-pci, max_outputs=1 ─ virglrenderer on host GPU
        │ -display dbus,gl=on
        │ D-Bus peer-to-peer connection, one per console:
        │   ScanoutDMABUF(fd, w, h, stride, fourcc, modifier)
        ▼
 automotive-dbus-proxy        (separate process from QEMU)
        │ zwp_linux_dmabuf_v1 → wl_buffer → wl_surface.commit
        │ surface title "QEMU (qemu-qnx-0)"
        ▼
 Weston, IVI shell, in a container
   ivi-id-agent: title "QEMU (qemu-qnx-0)" → IVI surface 1000
   LayerManagerControl: surface → layer → zone, render order
        │ x11-backend
        ▼
 Window on the host desktop
```

The proxy passes file descriptors, not pixels: it never maps or copies a
frame. It runs as its own process so that a fault in the display path
does not take the hypervisor down with it.

## Repository layout

| Path | Contents |
|---|---|
| [`proxy/`](proxy/) | `automotive-dbus-proxy`: QEMU D-Bus display listener to Wayland surface bridge (C, Meson) |
| [`compositor/`](compositor/) | Container image for the host compositor: Weston 10 with the COVESA IVI extension 2.3.1, plus `ivi-input-set` for input focus |
| [`runbooks/`](runbooks/) | `qnx-probe-launch.sh`: QNX and Debian side by side through QEMU's SDL display (the reference path the proxy replaces) |
| [`guest/debian/`](guest/debian/) | cloud-init template for the Debian test guest |
| [`docs/`](docs/) | [Architecture](docs/architecture.md), [running the proxy](docs/running-the-proxy.md), [QNX guest setup](docs/qnx-guest.md), [pitfalls and fixes](docs/findings.md), [roadmap](docs/roadmap.md) |

## Tested setup

Everything here has been run on one machine only:

- Ubuntu 22.04 host with an AMD RDNA3 GPU
- QEMU 9.2.4, an unmodified upstream release, built from source with the
  SDL and D-Bus displays and OpenGL support, linked against
  virglrenderer 1.3.0
- SDL2 2.28.5 built from source with only the Wayland video driver (X11
  disabled); the reference runbook has been run with this build
- Docker, with the host X11 socket and the GPU device nodes passed into
  the compositor container
- QNX SDP 8.0 QEMU target image (QSTI), installed through QNX Software
  Center under your own licence; it is not part of this repository
- Debian 12 cloud image for the second guest of the reference runbook

Device paths (`/dev/dri/card1`, `/dev/dri/renderD128`) and the default
locations of QEMU and the QNX image are those of the test machine. The
runbook takes them from environment variables; see the top of
[`runbooks/qnx-probe-launch.sh`](runbooks/qnx-probe-launch.sh).

## Quick start

```bash
# 1. Host compositor image
docker build -t automotive-dash-platform:latest compositor/

# 2. Proxy
meson setup build proxy && ninja -C build
```

The runbook defaults to the image tag of the test machine; set
`DOCKER_IMAGE=automotive-dash-platform:latest` to use the image built above.
Then follow [docs/running-the-proxy.md](docs/running-the-proxy.md) to
start the compositor, a QNX guest with `-display dbus,gl=on`, and the
proxy.

## Related work

- [qemu-display](https://gitlab.com/marcandre.lureau/qemu-display):
  Rust crates for QEMU's D-Bus display, with a GTK 4 viewer and VNC and
  RDP servers. It is the general-purpose client for `-display dbus`;
  vcockpit's proxy is narrower, and exists to place each guest scanout as
  a numbered IVI surface for a fixed, policy-driven layout.
- [QEMU D-Bus display interface](https://qemu.org/docs/master/interop/dbus-display.html):
  the protocol the proxy implements the listener side of.
- [COVESA wayland-ivi-extension](https://github.com/COVESA/wayland-ivi-extension):
  the IVI controller, layer management API and `LayerManagerControl` used
  by the compositor.

## Licence

Code is licensed under [GPL-2.0-or-later](LICENSES/GPL-2.0-or-later.txt).
Documentation is licensed under [CC BY 4.0](LICENSES/CC-BY-4.0.txt). Each
file carries an SPDX identifier.

Developed with AI assistance (Claude, with Gemini as a cross-reviewer).
