<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# Pitfalls and fixes

Problems hit while building vcockpit, with the cause and the fix. Most of
them fail silently: the command succeeds and nothing appears on screen.

## QEMU display

<a id="vga-none"></a>
### `-display dbus` needs `-vga none`

**Symptom:** the proxy receives scanouts at 640x480 and 720x400 only, and
`ConsoleIDs` lists two consoles.
**Cause:** without `-vga none`, QEMU adds a legacy VGA device as console 0
and the virtio GPU becomes console 1. The listener registers on the VGA
console and EDID never reaches the guest.
**Fix:** `-vga none` together with
`-device virtio-gpu-gl-pci,max_outputs=1,edid=on,xres=1280,yres=720`.
`ConsoleIDs` then reads `[0]`.

### `SetUIInfo` cannot set the guest resolution here

**Symptom:** `org.qemu.Display1.Console.SetUIInfo` returns
`org.qemu.Display1.Error.Unsupported`.
**Cause:** QEMU accepts it only if the display device implements the
`ui_info` callback, and `virtio-gpu-gl-pci` does not.
**Fix:** set the resolution on the guest side and keep it identical to
QEMU's `xres`/`yres` ([qnx-guest.md](qnx-guest.md)).

### With the non-blob virgl path, every frame is a new `ScanoutDMABUF`

**Symptom:** the surface shows one frame and never updates, although QEMU
is busy; no `UpdateDMABUF` calls arrive.
**Cause:** the expected pattern, one `ScanoutDMABUF` followed by
`UpdateDMABUF` per frame, belongs to the blob-resource path. A guest
using the older virgl stack without blob resources makes QEMU send a
`ScanoutDMABUF` with a new buffer for each frame instead.
**Fix:** attach and commit on every `ScanoutDMABUF`. Keep the
`UpdateDMABUF` handler, which the blob path will use.

### QEMU's default of 16 virtual outputs fragments a QNX screen

**Symptom:** the QNX desktop, launcher and application windows arrive as
separate surfaces of varying size.
**Cause:** virtio-gpu advertises 16 outputs by default and QNX Screen
spreads its windows across them.
**Fix:** `max_outputs=1`.

## Wayland client in a GLib main loop

### Importing a dma-buf inside a D-Bus callback can deadlock

**Symptom:** the dma-buf import never completes and the surface keeps its
placeholder buffer.
**Cause:** `zwp_linux_buffer_params_v1_create()` answers asynchronously, and
waiting for the answer with `wl_display_roundtrip()` from inside a GDBus
method handler competes with the GLib source that already owns the
Wayland file descriptor.
**Fix:** `zwp_linux_buffer_params_v1_create_immed()`, which returns the
`wl_buffer` at once; the compositor validates it on first use.

## IVI compositor

<a id="stale-ivi-state"></a>
### Stale IVI state makes `set screen 0 render order` fail silently

**Symptom:** the command prints nothing, `get screens` shows no layers,
and the Weston log reports that the layer does not exist even after
`create layer` succeeded.
**Cause:** a proxy restarted against a compositor that still holds the
previous run's surface and layer.
**Fix:** restart the compositor container before each run.

### `render order` takes a comma-separated list

`LayerManagerControl set screen 0 render order 1000,2000` works;
`"1000 2000"` with a space is split by the parser and fails.

### Input focus needs input acceptance as well

**Symptom:** focus is set but no pointer appears over the surface.
**Cause:** `ilm_setInputFocus()` alone does not route events.
**Fix:** call `ilm_setInputAcceptanceOn(surface, 1, &seat)` first, with
the seat from `ilm_getDefaultSeat()` (see `compositor/ivi-input-set.c`).
This makes input reach QEMU's SDL windows in the reference runbook. The
proxy does not handle input yet, so its surface still shows no pointer
([roadmap](roadmap.md)).

### Weston's IVI shell does not show xdg-shell clients by itself

The IVI shell speaks `ivi_application`; xdg-shell clients such as QEMU's
SDL window or the proxy connect without error but stay invisible. Load
`ivi-id-agent.so` and map window titles to surface IDs in `weston.ini`
(`[desktop-app]` sections). Match on the title: QEMU's SDL window sets the
xdg `app_id` to its binary name and ignores `SDL_VIDEO_WAYLAND_WMCLASS`,
and its title has the form `QEMU (<-name value>-<display index>)`. The
proxy uses the same title format.

### COVESA wayland-ivi-extension 2.3.x needs libweston 10

Build 2.3.1 against Weston 10 with `-DLIBWESTON_VER=10`. Newer Weston
releases break its CMake version handling.

## Containers and hosts

### Weston in a container needs the host's X server and GPU

Running Weston's x11 backend against Xvfb with software rendering stalls
`eglSwapBuffers` after two frames. Pass the host X11 socket and the DRI
device nodes into the container and run `xhost +local:docker`.

### Fix the Wayland socket permissions after the socket exists

The container recreates the socket at start-up, so a `chmod` made before
`docker run` is lost. Wait for the socket, then `chmod`.

### Set the render order only after the surfaces have registered

Layer commands sent before the surfaces exist are ignored. Poll
`LayerManagerControl get surfaces` until the expected IDs appear.

## QNX

### The QNX SDP environment script changes SDL variables

`qnxsdp-env.sh` overwrites `SDL_VIDEODRIVER` among others. Source it in a
subshell and set the Wayland variables again afterwards.

### The QSTI `.vmdk` descriptor does not load

Use the raw `disk-qemu` file with `format=raw`.

### Edit QNX configuration with `sed`, not `vi`, over a serial console

See [qnx-guest.md](qnx-guest.md).

## Debian test guest

### Start Xorg with `sudo` over SSH

`startx` fails from a non-interactive SSH session because there is no TTY.
`sudo Xorg :0 -ac -noreset -nolisten tcp` works.
