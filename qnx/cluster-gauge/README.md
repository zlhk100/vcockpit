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
vehicle bus or sensor input. It is developed following ASIL-B-oriented
practices; it is not certified.

It publishes the telltale state it commands, so that
[`telltale-monitor`](../telltale-monitor/), a separate process, can check
that the telltales on the screen match it.

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
font or image files. Where each telltale is drawn and in which colours
comes from [`qnx/common/telltale_layout.h`](../common/telltale_layout.h),
the header the monitor reads too.

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

Other targets: `make DEBUG=1` builds with `-O0 -g`; `make install` copies
the binary to `build/stage/system/bin/` (set `DESTDIR` to change the
staging directory). The version string printed by `--version` is
`git describe --always --dirty` at build time.

## Coding standard and `make check`

The code follows the [C coding standard](../../docs/coding-standard.md).
`make check`, with the SDP environment sourced, is the gate for every
change. It runs:

1. the QNX build with the standard's warning set, warnings as errors
2. the host test of the drive cycle
3. the static-storage check: every writable symbol in the object files
   (`nm` types `b B d D`) must be listed in
   [`static-storage-allowlist.txt`](static-storage-allowlist.txt), and every
   entry must match a symbol; the forbidden-call checks (no
   `sigaction` or `signal()`, no `malloc`/`calloc`/`realloc`/`free`); and
   the fault-injection check: a default build must contain no
   fault-injection symbol or option, a fault-injection build must contain
   both
4. `cppcheck` and `clang-tidy` (`cert-*`, `bugprone-*`); a tool that is not
   installed fails the gate

Places where a rule is not met are recorded in
[`DEVIATIONS.md`](DEVIATIONS.md).

## State and threads

One thread runs all of the program; it creates no thread. The Screen and
EGL handles live in a structure on `main`'s stack. The renderer's state,
including a 64 KB vertex buffer, is one structure declared `static` inside
`main`, so it is off the stack; `main` passes it by pointer to the
renderer, the only code that writes it. The drive-cycle state is computed
fresh each frame. Everything else with static storage is a constant table.

SIGINT and SIGTERM are blocked before Screen or EGL start, so threads the
graphics libraries create inherit the mask, and are polled once per frame
with `sigtimedwait()`; there is no signal handler. Screen events are
drained once per frame without blocking; a close event ends the program
like SIGTERM. The memory of the process is locked with `mlockall()` after
start-up.

## Shared telltale state

Once per frame, before drawing, the gauge publishes the telltale state it
is about to draw in a POSIX shared memory object,
`/vcockpit-telltale-state` (4096 bytes, created at start-up, removed at
exit). In the guest it appears as `/dev/shmem/vcockpit-telltale-state`.

The object holds one 64-bit word, written in one atomic store and read in
one atomic load, so a reader never sees half an update:

| Bits | Field |
|---|---|
| 63–32 | sequence number, +1 per frame; 0 means nothing published yet |
| 31–16 | time since start in ms, modulo 65536 |
| 15–0 | telltale bits (`TT_*` in `telltale_layout.h`) |

The gauge only writes the word; the monitor maps it read-only. The layout
and the functions that pack and unpack the word are in
[`telltale_layout.h`](../common/telltale_layout.h).

## Fault-injection build

To test the monitor, the gauge can be built to draw one telltale wrong on
purpose:

```bash
make -C qnx/cluster-gauge clean
make -C qnx/cluster-gauge FAULT_INJECTION=1
```

This build adds one option, `--inject-telltale-fault NAME@S`: from S
seconds after start, telltale NAME is drawn inverted (lit when it should
be off, dim when it should be on), while the published state stays
correct. NAME is one of `turn_left`, `seatbelt`, `high_beam`,
`check_engine`, `turn_right`. The version string ends in
`+fault-injection`, and the build says so at start-up:

```
cluster-gauge: FAULT INJECTION BUILD: high_beam drawn inverted from 20.000 s
cluster-gauge: fault injection: drawing high_beam inverted from t=20.004 s
```

The default build contains no fault-injection code and rejects the
option; `make check` verifies both. Use the fault-injection build for
testing only.

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
| `--seconds N` | Exit with status 0 after N seconds; N must be a finite number greater than 0 |
| `--priority N` | Run at `SCHED_RR` priority N; if that fails, exit with status 1 |
| `--help` | Print usage and exit |
| `--version` | Print the version and exit |

An invalid option or value prints the usage and exits with status 2.
Without `--seconds` it runs until SIGINT, SIGTERM or a Screen close event,
then exits with status 0.

Output:

```
cluster-gauge 192e476: 1280x720, SCHED_RR priority 10 (inherited), mlockall ok
cluster-gauge: publishing telltale state to /vcockpit-telltale-state
cluster-gauge: frames 300, 60.4 fps, overruns 0, longest frame 19.0 ms
...
cluster-gauge: stopped (--seconds elapsed): frames 1802, overruns 0, longest frame 19.4 ms
```

- The start-up line gives the version, the buffer size, the scheduling
  policy and priority (inherited, or set by `--priority`), and whether
  `mlockall()` succeeded; a failure is reported and is not fatal.
- Every 300 frames: the frame rate, the number of overruns since start (a
  frame taking longer than two display periods at 60 Hz, 33.3 ms) and the
  longest frame in those 300 frames.
- On exit: why it stopped, the frame count, overruns and the longest frame
  of the run.
- If the vertex buffer fills during a frame, a line on stderr gives the
  number of vertices dropped in that frame.

If Screen, EGL or OpenGL ES cannot be initialised, or the shared state
object cannot be created, it prints one line
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
([running the proxy](../../docs/running-the-proxy.md), section 5), that
the frame rate lines are close to 60 and that the overrun count stays 0.
