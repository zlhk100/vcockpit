<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# telltale-monitor

A QNX 8.0 program that checks, ten times a second, that the telltales
on the screen are the ones [`cluster-gauge`](../cluster-gauge/) means
to show. It is a separate process from the gauge. It reads what the
gauge commands, captures what the display actually shows, and compares
the two. If a telltale is wrong for three checks in a row, it logs a
fault and puts up a red warning band.

This is the pattern used for telltale integrity in instrument clusters:
an independent check of what is displayed against what was commanded.
Like the gauge, it is developed following ASIL-B-oriented practices; it
is not certified. Read [Limits](#limits) before drawing conclusions from
it.

## What it checks

The five telltales of the gauge: left and right turn, seat belt, high
beam, check engine. Where each one is on the screen, and what colours it
has when on and when off, come from one header shared with the gauge,
[`qnx/common/telltale_layout.h`](../common/telltale_layout.h).

Once per period (100 ms by default) the monitor:

1. reads the gauge's commanded telltale state from shared memory (see
   [the gauge's README](../cluster-gauge/README.md#shared-telltale-state));
   if the state has not changed for 500 ms, the gauge is not updating,
   and that is a fault
2. captures the composed display, the same picture the host receives
3. reads the colour at two or three points inside each telltale and
   classifies it as lit (every point within 8 per channel of the on
   colour), dim (every point within 8 of the off colour), or unknown
   (anything else, including black or an empty screen)
4. compares the result with the commanded state: lit when commanded off,
   dim when commanded on, and unknown in either state are mismatches
5. counts mismatches per telltale; three in a row is a fault

If the commanded state changed while the display was being captured, the
cycle is skipped rather than judged on a picture that may be one frame
behind. A failed capture counts as unknown for every telltale, so a
capture that keeps failing becomes a fault.

## What it does on a fault

On the first fault it logs one line naming the telltale and both states,
then enters its fault state:

- it creates a window at the top of the display, fills it with a red band
  and a warning sign, and posts it above the gauge; the band is drawn with
  Screen's fill operation, not with OpenGL, so it does not share the
  gauge's rendering path
- it stops comparing; it keeps running and keeps printing status lines
  until it is stopped

The window does not exist until the first fault, and nothing is drawn
before then.

## Build

As for the gauge: QNX SDP 8.0, with the SDP environment sourced in a
subshell. From the repository root:

```bash
(
    source ~/workspace/qnx8/toolchain/qnxsdp-env.sh
    make -C qnx/telltale-monitor
)
```

The binary is `qnx/telltale-monitor/build/telltale-monitor`. It links
against `libscreen` only.

## Coding standard and `make check`

The code follows the [C coding standard](../../docs/coding-standard.md),
and `make check` has the same four parts as the gauge's: the QNX build
with warnings as errors, the host test, the static-storage and
forbidden-call checks, and `cppcheck` and `clang-tidy`. It also checks
that the binary imports no OpenGL or EGL function.

The monitor has no writable static storage: its
[`static-storage-allowlist.txt`](static-storage-allowlist.txt) is empty.
It uses no heap and no signal handler; SIGINT and SIGTERM are blocked and
polled once per cycle. [`DEVIATIONS.md`](DEVIATIONS.md) records no
deviations.

## Run in the guest (development)

Set up the guest as in the gauge's README, and fetch both binaries. Start
the gauge first, then the monitor:

```bash
./cluster-gauge &
./telltale-monitor
```

The monitor waits up to 10 s for the gauge's shared state to appear. Run
it as root: capturing the display needs a display-manager context.

| Option | Effect |
|---|---|
| `--period-ms N` | Check every N ms, 50 to 1000 (default 100) |
| `--priority N` | Run at `SCHED_RR` priority N; recommended above the gauge's |
| `--help` | Print usage and exit |
| `--version` | Print the version and exit |

An invalid option prints the usage and exits with status 2. A start-up
failure prints one line naming the call that failed and exits with
status 1. SIGINT or SIGTERM stops it with status 0.

Output:

```
telltale-monitor 86181a8: display 1280x720, period 100 ms, SCHED_RR priority 10 (inherited), mlockall ok, reading /vcockpit-telltale-state
telltale-monitor: cycles 50, skipped 0, overruns 0, longest 42.2 ms, faults 0
telltale-monitor: FAULT high_beam commanded=off displayed=lit t=20288
telltale-monitor: fault state: checks stopped, overlay shown
telltale-monitor: stopped (SIGTERM): cycles 240, skipped 0, overruns 0, longest 42.2 ms, faults 1
```

- Every 50 cycles: cycles run, cycles skipped, overruns (a cycle longer
  than the period), the longest cycle and the number of faults.
- A fault line gives the telltale, what was commanded, what was
  displayed, and `t=`, the gauge's time since start in ms (modulo 65536)
  for the state that was checked. A gauge that stops updating gives
  `FAULT gauge not updating seq=<n> t=<ms>`.

## Tests

The classification and comparison logic is tested on the host:

```bash
make -C qnx/telltale-monitor host-test
```

It builds synthetic screens and checks: lit and dim telltales, the edges
of the colour tolerance, that black and the background colour classify
as unknown, that a blank screen faults every telltale, the three-cycle
debounce and its reset, and the packing of the shared state.

In the guest:

- **No false faults.** Run the gauge and the monitor together for 130 s,
  two full drive cycles: no fault, and the longest cycle stays under the
  period.
- **Injected fault.** Build the gauge with `make FAULT_INJECTION=1` (see
  the gauge's README) and run it with
  `--inject-telltale-fault high_beam@20` or `seatbelt@1`, with the
  monitor running: a fault line for that telltale within 500 ms of the
  injection time.
- **Gauge not updating.** Stop the gauge with `kill -STOP` while the
  monitor runs: `FAULT gauge not updating` within 1 s.

## Limits

What this shows: a check of the displayed telltales that is independent
of the gauge's own drawing. It runs in a separate process with its own
memory, maps the shared state read-only, looks at the composed display
rather than at the gauge's buffers, and draws its warning without OpenGL.

What it does not show, so the demonstration is not over-read:

- The gauge and the monitor share the guest OS, the Screen compositor,
  the virtual GPU and the host GPU. A fault in any of them can affect
  both.
- The commanded state comes from the gauge, the same process that draws
  the telltales. In a vehicle it would come from the signal source.
- The warning band is shown by the same compositor whose output is being
  checked.
- The timing values (100 ms period, three-cycle debounce, 500 ms and 1 s
  detection) are design targets. They are not derived from a hazard
  analysis or a fault-tolerant time interval.
- The visible reaction depends on the guest's window manager. The QNX
  guest image runs it in a demo mode that pauses programs in the
  background, so once the monitor's window appears the gauge stops
  updating, and the window manager decides how the window is shown. A
  production cluster would not work this way. In the tested setup, after
  a fault the host showed the gauge frozen or a blank screen, not the red
  band. A warning layer drawn on the host, placed above all guest
  surfaces by the IVI compositor, would not depend on the guest's
  compositor; that is a possible later step.

vcockpit is not certified.
