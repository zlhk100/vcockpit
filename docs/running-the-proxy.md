<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# Running the proxy

These are the manual steps that put a QNX guest on screen through
`automotive-dbus-proxy`. They have been run on the tested setup described
in the [README](../README.md); a launch script that runs them in order
and checks each step is on the [roadmap](roadmap.md).

Start from a clean compositor container. IVI surface and layer state left
over from an earlier proxy run makes `set screen 0 render order` fail
without an error message ([findings](findings.md#stale-ivi-state)).

## 1. Compositor

The compositor needs a `weston.ini` that maps window titles to IVI
surface IDs:

```ini
[core]
shell=ivi-shell.so
modules=ivi-controller.so

[ivi-shell]
ivi-input-module=ivi-input-controller.so
ivi-id-agent-module=ivi-id-agent.so

[desktop-app]
app-title=QEMU (qemu-qnx-0)
surface-id=1000

[desktop-app]
app-title=QEMU (qemu-debian-0)
surface-id=2000

[desktop-app-default]
default-surface-id=30
default-surface-id-max=200

[output]
name=X1
mode=3840x1080
```

Save it as `/tmp/weston-phase2.ini`, then:

```bash
sudo mkdir -p /tmp/weston-shared
sudo chown "$(id -u):$(id -g)" /tmp/weston-shared
xhost +local:docker
docker run --rm -d \
    -v /tmp/weston-shared:/tmp/weston-runtime \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v /tmp/weston-phase2.ini:/etc/xdg/weston/weston.ini:ro \
    -e DISPLAY=$DISPLAY \
    --device=/dev/dri/card1 \
    --device=/dev/dri/renderD128 \
    --name dash-test \
    automotive-dash-platform:latest
sleep 3 && sudo chmod 777 /tmp/weston-shared/wayland-1
```

The compositor sets `/tmp/weston-shared` to mode 0700 at start-up. Creating
it yourself first keeps it yours; a directory that Docker creates is owned
by root, and QEMU and the proxy, running as you, could not reach the socket.

## 2. QNX guest with the D-Bus display

The guest image must have had the one-time Screen configuration edit
described in [qnx-guest.md](qnx-guest.md).

```bash
(
    source ~/workspace/qnx8/toolchain/qnxsdp-env.sh > /dev/null 2>&1
    QNX=~/workspace/qnx8/toolchain/images/qemu/qemu/output
    "${QEMU_BIN:-qemu-system-x86_64}" \
        -name qemu-qnx \
        --enable-kvm -smp 4 -m 4G \
        -cpu host,host-phys-bits-limit=40 \
        -drive file=$QNX/disk-qemu,format=raw,if=ide,id=drv0 \
        -kernel $QNX/ifs.bin \
        -serial file:/tmp/qnx-serial.log \
        -object rng-random,filename=/dev/urandom,id=rng0 \
        -device virtio-rng-pci,rng=rng0 \
        -object memory-backend-memfd,id=mem,size=4G,share=on \
        -machine memory-backend=mem \
        -vga none \
        -display dbus,gl=on \
        -device virtio-gpu-gl-pci,max_outputs=1,edid=on,xres=1280,yres=720 \
        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2223-:22 \
        -device virtio-net-pci,netdev=net0
) > /tmp/qemu-qnx.log 2>&1 &
```

Set `QEMU_BIN` to your QEMU build if it is not the `qemu-system-x86_64` on
your `PATH`, for example `QEMU_BIN=~/qemu/build/qemu-system-x86_64`.

`-vga none` is required. Without it QEMU adds a legacy VGA console as
console 0, the virtio GPU becomes console 1, and the guest comes up at
720x400 ([findings](findings.md#vga-none)).

QEMU registers on your session bus as `org.qemu`:

```bash
dbus-send --session --dest=org.freedesktop.DBus --type=method_call \
    --print-reply /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep qemu
```

Wait until the serial log shows QNX Screen has started:

```bash
grep -m1 'Starting Screen' /tmp/qnx-serial.log
```

## 3. Proxy

```bash
export XDG_RUNTIME_DIR=/tmp/weston-shared
export WAYLAND_DISPLAY=wayland-1
build/automotive-dbus-proxy --vm-name qnx --wayland-socket wayland-1 --n-scanouts 1
```

Expected output includes:

```
ConsoleIDs: [0]
Console_0: RegisterListener OK — p2p connection established
ScanoutDMABUF: scanout=0 1280x720 fourcc=0x... modifier=0x...
ScanoutDMABUF: committed 1280x720 to surface
```

## 4. Place the surface

```bash
docker exec dash-test bash -c '
    export XDG_RUNTIME_DIR=/tmp/weston-runtime
    export WAYLAND_DISPLAY=wayland-1
    LayerManagerControl create layer 1000 1280 720
    LayerManagerControl add surface 1000 to layer 1000
    LayerManagerControl set surface 1000 destination region 0 0 1280 720
    LayerManagerControl set surface 1000 source region 0 0 1280 720
    LayerManagerControl set layer 1000 destination region 0 0 1280 720
    LayerManagerControl set layer 1000 visibility 1
    LayerManagerControl set surface 1000 visibility 1
    LayerManagerControl set screen 0 render order 1000'
```

## 5. Check

```bash
docker exec dash-test bash -c '
    export XDG_RUNTIME_DIR=/tmp/weston-runtime
    export WAYLAND_DISPLAY=wayland-1
    LayerManagerControl get surface 1000' | grep -E 'original size|frame counter'
```

`original size` should read `x=1280, y=720`. The frame counter rises when
the guest redraws; a static screen sends few frames.

Input is not forwarded yet: the QNX surface does not react to keyboard or
mouse.

## Stop

Stop the proxy with Ctrl+C or SIGTERM. It closes its D-Bus connections,
removes its surface from the compositor and exits with status 0:

```
Shutting down (SIGINT)...
Shut down: 1 surface(s) removed
```

`LayerManagerControl get surfaces` no longer lists surface 1000 and the
compositor keeps running. Then `docker stop dash-test`, then stop QEMU.

The compositor keeps layer 1000 after the proxy exits. A proxy started
again against the same compositor brings surface 1000 back with frames,
but not on the layer and not visible. Restart the container before the
next run ([findings](findings.md#stale-ivi-state)).
