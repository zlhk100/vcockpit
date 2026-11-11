<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# cluster-gauge

A QNX 8.0 program that draws an instrument cluster with QNX Screen and
OpenGL ES 2. Run in the QNX guest, it appears on the host through
`automotive-dbus-proxy` as IVI surface 1000, like any other Screen
application.

It is a demonstration workload, not a vehicle function: it reads no
vehicle bus or sensor input and makes no safety claim.

## What it shows

At 1280x720, redrawn once per display refresh:

- a 0–200 km/h dial with a filled arc, tick marks and a needle
- the speed as three seven-segment digits
- an RPM bar, 0–7000, with a red zone above 6000
- five telltales: left and right turn, seat belt, high beam, check engine
  (dimmed when off)

The values come from a scripted drive cycle: a 60 s loop of idle,
acceleration to 100 km/h, cruise, a left turn with the blinker, braking,
a right turn, and a night segment with high beam. For the first 2 s after
start every telltale is on (bulb check); the seat belt telltale stays on
until 5 s. The same time since start always gives the same picture.

All shapes are drawn from triangles in the program itself; there are no
font or image files.

## Build

Needs QNX SDP 8.0 (installed through QNX Software Center under your own
licence). The target is x86_64, for the QNX SDP 8.0 QEMU target image
(QSTI) described in [QNX guest](../../docs/qnx-guest.md).

From the repository root:

```bash
(
    source ~/workspace/qnx8/toolchain/qnxsdp-env.sh
    make -C qnx/cluster-gauge
)
```

This runs `qcc -Vgcc_ntox86_64` and links against `libscreen`, `libEGL`
and `libGLESv2` from the SDP. The binary is
`qnx/cluster-gauge/build/cluster-gauge`. Source the SDP environment in a
subshell, as above, for the reason given in
[QNX guest](../../docs/qnx-guest.md).

## Run in the guest (development)

Start the compositor, the QNX guest and the proxy as in
[running the proxy](../../docs/running-the-proxy.md), with two changes to
the QEMU command of section 2:

- add `-snapshot`, so nothing you do in the guest is written to
  `disk-qemu`
- use `-serial mon:stdio` instead of `-serial file:...` if you want the
  guest's serial console in your terminal

Serve the binary from the host, on the loopback address only:

```bash
python3 -m http.server 8099 --bind 127.0.0.1 --directory qnx/cluster-gauge/build
```

At the guest console, logged in as root, fetch and run it. With QEMU user
networking the host is reachable from the guest as `10.0.2.2`:

```bash
cd /tmp
curl -sS -o cluster-gauge http://10.0.2.2:8099/cluster-gauge
chmod +x cluster-gauge
./cluster-gauge
```

The gauge fills the QNX surface. Stop it with Ctrl+C.

Starting the gauge automatically at guest boot is not covered here yet.

## Options and output

| Option | Effect |
|---|---|
| `--seconds N` | Exit with status 0 after N seconds |

Without options it runs until SIGINT or SIGTERM, then exits with status 0.
It prints the buffer size once and a frame rate line every 300 frames:

```
cluster-gauge: 1280x720
cluster-gauge: frames 300, 60.6 fps
```

If Screen, EGL or OpenGL ES cannot be initialised, it prints one line
naming the call that failed and exits with status 1.

The EGL config is chosen by its native visual, which must equal the
window's Screen format (RGBX8888). `eglChooseConfig()` is not used: on the
QEMU virtio GPU it can return a 10-bit config first, and Screen cannot
allocate window buffers in that format.

## Tests

The drive cycle has no QNX dependency and is tested on the host:

```bash
make -C qnx/cluster-gauge host-test
```

It checks the speed band and the telltales at fixed times, that the same
time always gives the same state, and that speed stays within 0–200 km/h
and RPM within 0–7000.

In the guest, run `./cluster-gauge --seconds 60` and check that the
frame counter of surface 1000 rises through the run
([running the proxy](../../docs/running-the-proxy.md), section 5) and
that the frame rate lines are close to 60.
