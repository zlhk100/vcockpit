<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# QNX guest

vcockpit does not contain any QNX software. It uses the QNX SDP 8.0 QEMU
target image (QSTI) that QNX Software Center installs for you under your
own QNX licence.

## Files the runbooks expect

The runbooks look for the image under `QNX_DIR`, by default
`~/workspace/qnx8/toolchain/images/qemu/qemu`:

| File | Use |
|---|---|
| `output/ifs.bin` | QNX kernel image, passed to QEMU with `-kernel` |
| `output/disk-qemu` | Raw disk image, passed with `-drive ...,format=raw` |
| `output/disk-qemu.vmdk` | Do not use: its descriptor does not point at a usable extent; use `disk-qemu` directly |

The QNX SDP environment script (`QNX_SDP_ENV`, default
`~/workspace/qnx8/toolchain/qnxsdp-env.sh`) changes variables that QEMU's
SDL display reads, including `SDL_VIDEODRIVER`. Always source it inside a
subshell, as the runbook does, never in the shell you launch other VMs
from.

## One-time Screen configuration edit

The QSTI image's Screen configuration does not match the display QEMU
offers. Fix it once on `disk-qemu`, in
`/usr/share/screen/graphics-virtio-virgl.conf`:

1. Delete the `pipelines` line. The `gpu_virtio` driver rejects it
   (`unknown option for drm-virtio is specified`) and Screen exits.
2. Change `video-mode = 1280 x 768 @ 60` to `video-mode = 1280 x 720 @ 60`,
   so that it matches QEMU's `xres=1280,yres=720`. Any mismatch makes the
   EDID negotiation fail and Screen falls back to 720x400.

The QSTI image accepts SSH only with keys that are not shipped with it,
so make the edit over the serial console. Boot the image with
`-nographic -serial mon:stdio`, log in, and run:

```sh
sed -i '/pipelines/d' /usr/share/screen/graphics-virtio-virgl.conf
sed -i 's/video-mode = 1280 x 768 @ 60/video-mode = 1280 x 720 @ 60/' \
    /usr/share/screen/graphics-virtio-virgl.conf
cat /usr/share/screen/graphics-virtio-virgl.conf
```

Notes from doing this:

- Use `sed -i`, not `vi`. Editing with `vi` over the serial console
  silently truncates and corrupts lines.
- `mount -uo rw /system` reports "Not supported", but the filesystem is
  already writable. Check with `touch`.
- Leave QEMU with `Ctrl+A X`; QNX `reboot` may not exit QEMU in this mode.
- Make every edit in one boot; each boot takes a few minutes.

Keep a copy of the edited `disk-qemu`; reinstalling or updating the image
through QNX Software Center may replace it.

## QEMU device settings that go with it

```
-vga none
-device virtio-gpu-gl-pci,max_outputs=1,edid=on,xres=1280,yres=720
```

`max_outputs=1` stops QNX Screen from spreading its windows over several
virtual outputs: with the QEMU default of 16 outputs it assigns the
desktop, the launcher and each application window to separate ones.
`virtio-gpu-gl-pci`, unlike `virtio-vga-gl`, has no legacy VGA mode to fall
back to.

## Autostart overlay

`runbooks/vcockpit.sh` never logs into the guest: the cluster gauge and
its telltale monitor start at boot from an overlay disk,
`output/vcockpit-qnx.qcow2`, whose backing file is `disk-qemu`. QEMU opens
the backing file read-only, so `disk-qemu` itself is never written.

```bash
qemu-img create -f qcow2 -b "$QNX_DIR/output/disk-qemu" -F raw \
    "$QNX_DIR/output/vcockpit-qnx.qcow2"
```

The overlay was prepared once by booting it without `-snapshot`, with the
`-drive` line pointing at the overlay (`format=qcow2`) and the serial
console on a unix socket, and, as root on that console:

1. Copying `qnx/cluster-gauge/build/cluster-gauge` and
   `qnx/telltale-monitor/build/telltale-monitor` (default builds) to
   `/system/bin/` (`/system` is `/usr`, a writable qnx6 partition).
2. Copying `/system/etc/startup/post_startup.sh` to `post_startup.sh.orig`.
3. Adding these lines to `post_startup.sh`, just before its final
   `exit 0`:

   ```sh
   # vcockpit: start the cluster gauge and its telltale monitor once the window
   # manager's launcher (started after this script) is up, so that the gauge is
   # the foreground window
   (
       i=0
       while [ $i -lt 60 ] && ! pidin -f an | grep -q demolauncher; do sleep 1; i=$((i+1)); done
       sleep 10
       /system/bin/cluster-gauge > /tmp/cluster-gauge.log 2>&1 &
       sleep 2
       /system/bin/telltale-monitor > /tmp/telltale-monitor.log 2>&1 &
   ) &
   ```

4. `sync; shutdown -b`, then stopping QEMU (SIGTERM) once the console has
   gone quiet. QNX `shutdown` without `-b` reboots, and the halted guest
   does not power off.

The window manager and the launcher are started by `full_startup.sh`,
after `post_startup.sh`. A gauge started directly from `post_startup.sh`
ends up behind the launcher, and the monitor then reports every
commanded telltale as not displayed; hence the wait for the launcher.
Both programs log to `/tmp` in the guest.

## Limits of this image

The QSTI image ships the older `gpu_virtio` graphics stack, which composes
all QNX windows into one output. Routing individual QNX applications to
separate host surfaces needs the newer DRM virtio stack, which is not part
of the non-commercial SDP.
