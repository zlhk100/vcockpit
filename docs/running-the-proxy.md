<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# Running the proxy

These are the manual steps that put a QNX guest on screen through
`automotive-dbus-proxy`, and a Debian guest next to it through a second
proxy ([section 7](#7-two-guests-side-by-side)). They have been run on
the tested setup described in the [README](../README.md). The quick
start below runs all of them with one script.

## Quick start

`runbooks/vcockpit.sh` starts the whole cockpit: the compositor and its
background, both guests with their own buses, both proxies, the
placement of the two zones and keyboard focus. It needs guest disks
whose content starts at boot (see [the QNX guest](qnx-guest.md#autostart-overlay)
and [the Debian guest](../guest/debian/README.md#autostart-for-the-launcher)),
the compositor image, and the proxy built in `proxy/build`:

```bash
meson setup proxy/build proxy && ninja -C proxy/build
runbooks/vcockpit.sh start
runbooks/vcockpit.sh status
runbooks/vcockpit.sh stop
```

`start` waits for each step, places the surfaces once each guest shows its
final display size, checks that both surfaces receive frames and prints
its run directory. If a step fails it stops what it started and exits
non-zero; it refuses to start while a session is running. The gauge
appears about a minute after start, once QNX has booted. `stop` stops
only the processes the run recorded. `runbooks/vcockpit.sh --help` lists
the settings (QEMU, proxy, guest disks, output size, run directory) and
their defaults. The guest disks are opened with `-snapshot`, so a run
never changes them.

Each run keeps its logs in its own directory, by default under
`~/.local/state/vcockpit/runs/`: one log per process, the launcher's
log, the recorded process IDs and a status snapshot.

The rest of this page is the same sequence done by hand.

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

[desktop-app]
app-title=vcockpit-background
surface-id=10

[desktop-app-default]
default-surface-id=30
default-surface-id-max=200

[output]
name=X1
```

Save it as `/tmp/weston-phase2.ini`. The compositor's output size, which
is also the size of its window on the host desktop, is set with
`WESTON_WIDTH` and `WESTON_HEIGHT` (default 3840x1080), not in the ini.
Choose a size your screen can show; the tested setup uses 2880x1080.
Then:

```bash
sudo mkdir -p /tmp/weston-shared
sudo chown "$(id -u):$(id -g)" /tmp/weston-shared
xhost +local:docker
docker run --rm -d \
    -v /tmp/weston-shared:/tmp/weston-runtime \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v /tmp/weston-phase2.ini:/etc/xdg/weston/weston.ini:ro \
    -e DISPLAY=$DISPLAY \
    -e WESTON_WIDTH=2880 -e WESTON_HEIGHT=1080 \
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

A guest display has no transparency, so the proxy imports a guest frame
whose format has an alpha channel as the same format without it, and marks
its surface opaque. It logs each substitution once:

```
ScanoutDMABUF: importing fourcc 0x34324241 as opaque 0x34324258
```

(0x34324241 is ABGR8888, 0x34324258 XBGR8888.) Without this, the
compositor blends the guest picture with whatever was on screen before
wherever the guest leaves the alpha byte undefined, as an X server's
depth-24 framebuffer does, and old cursor images and old frames show
through.

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

## 6. Input

The proxy forwards pointer and keyboard events that the compositor
delivers to its own surface to QEMU's `org.qemu.Display1.Mouse` and
`org.qemu.Display1.Keyboard` interfaces on `Console_0`:

- pointer position, as an absolute position in guest pixels (or as
  relative motion while QEMU reports a relative mouse); the proxy follows
  QEMU's `IsAbsolute` property, which turns true once the guest's input
  driver is up
- left, right and middle buttons, side and extra buttons, and the
  vertical wheel
- keys, as QEMU key numbers (AT set 1). The keyboard layout is the
  guest's. Keys without a number in the proxy's table are not sent; they
  are counted

Buttons and keys still held when the pointer or keyboard leaves the
surface, or when the proxy stops, are released in the guest.

The compositor sends keyboard events to surface 1000 only after it has
been given keyboard focus:

```bash
docker exec dash-test bash -c '
    export XDG_RUNTIME_DIR=/tmp/weston-runtime
    export WAYLAND_DISPLAY=wayland-1
    ivi-input-set 1000'
```

Run it after section 4. With no second guest it also prints
`surface ID 2000 not found`, which is expected. If the compositor's window
loses focus on the host desktop, keyboard focus on surface 1000 is lost
too; focus the compositor's window and run `ivi-input-set 1000` again.
Pointer input does not need this.

Every button press is logged with the guest position:

```
Input: Console_0 button 0 press at (717,449)
```

The proxy draws its own pointer cursor, a small white arrow, while the
pointer is over its surface; the guest's cursor does not reach the host.

The proxy sends a key only while the pointer is on its surface. Keys typed
with the pointer elsewhere are not sent; they are counted as gated keys.
Keys still held when the pointer leaves the surface are released in the
guest. With one guest, keep the pointer over its surface while typing.

## 7. Two guests side by side

A Debian guest runs next to the QNX guest through its own QEMU, its own
D-Bus bus and its own proxy. Each QEMU with `-display dbus` takes the name
`org.qemu` on its bus, and a second QEMU on the same bus would wait behind
the first, so each guest gets a private bus.

### Compositor

As section 1. The zones below assume `WESTON_WIDTH=2880` and
`WESTON_HEIGHT=1080`. The size must fit on your screen: with the default
3840x1080 on a 2880-pixel-wide screen the window was 2880 pixels wide and
the right-hand part of the output was never shown. The size given on the
command line overrides any `mode` in the ini's `[output]` section.
Check the size after the compositor starts:

```bash
docker logs dash-test 2>&1 | grep 'x11 output'
xwininfo -root -tree | grep 'Weston Compositor'
```

Both should report 2880x1080.

### Background

The IVI shell has no background, and Weston repaints only areas covered
by a surface: output pixels outside both zones would keep every old
cursor image. Start the background client, which shows an opaque black
surface titled `vcockpit-background` (surface 10 in the ini of section 1):

```bash
docker exec -d -e XDG_RUNTIME_DIR=/tmp/weston-runtime -e WAYLAND_DISPLAY=wayland-1 \
    dash-test /usr/local/bin/ivi-background
```

It runs until the container stops or it receives SIGINT or SIGTERM. It is
placed under both zones in the placement below.

### Buses

Start each bus before its QEMU:

```bash
dbus-daemon --session --address=unix:path=/tmp/vcockpit-bus-qnx --nofork --print-address &
dbus-daemon --session --address=unix:path=/tmp/vcockpit-bus-debian --nofork --print-address &
```

### Guests

QNX: the command in section 2, run with
`DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/vcockpit-bus-qnx` and with the
display option changed to

```
-display dbus,gl=on,addr=unix:path=/tmp/vcockpit-bus-qnx
```

Debian: the guest image prepared as in
[guest/debian/README.md](../guest/debian/README.md), at `$DEBIAN_IMAGE`.
`-snapshot` discards everything the guest writes to its disk.

```bash
DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/vcockpit-bus-debian \
"${QEMU_BIN:-qemu-system-x86_64}" \
    -name qemu-debian \
    -machine q35,accel=kvm,memory-backend=mem -cpu host -smp 2 -m 2G \
    -object memory-backend-memfd,id=mem,size=2G,share=on \
    -drive file=$DEBIAN_IMAGE,if=virtio,format=qcow2 -snapshot \
    -vga none -display dbus,gl=on,addr=unix:path=/tmp/vcockpit-bus-debian \
    -device virtio-vga-gl,edid=on,xres=1920,yres=1080 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=net0 > /tmp/qemu-debian.log 2>&1 &
```

Each bus should list `org.qemu`, and each QEMU should report one console:

```bash
for g in qnx debian; do
    gdbus call --address unix:path=/tmp/vcockpit-bus-$g -d org.qemu \
        -o /org/qemu/Display1/VM -m org.freedesktop.DBus.Properties.Get \
        org.qemu.Display1.VM ConsoleIDs
done
```

Once the Debian guest answers on SSH, start Xorg and a GL client in it:

```bash
ssh -p 2222 debian@127.0.0.1 'nohup sudo Xorg :0 -ac -noreset -nolisten tcp > /tmp/xorg.log 2>&1 &'
ssh -p 2222 debian@127.0.0.1 'DISPLAY=:0 nohup glxgears -geometry 1920x1080+0+0 > /tmp/glxgears.log 2>&1 &'
```

### Proxies

One proxy per guest, each on its guest's bus:

```bash
export XDG_RUNTIME_DIR=/tmp/weston-shared
export WAYLAND_DISPLAY=wayland-1
DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/vcockpit-bus-qnx \
    build/automotive-dbus-proxy --vm-name qnx --wayland-socket wayland-1 --n-scanouts 1 &
DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/vcockpit-bus-debian \
    build/automotive-dbus-proxy --vm-name debian --wayland-socket wayland-1 --n-scanouts 1 &
```

The compositor maps `QEMU (qemu-debian-0)` to surface 2000 and
`QEMU (qemu-qnx-0)` to surface 1000. The Debian proxy logs
`ScanoutDMABUF: scanout=0 1920x1080`.

### Placement

Two equal 16:9 zones, side by side and vertically centred in the
2880x1080 output, over the background, which covers the whole output.
Each guest picture is scaled to its zone:

| Picture | Surface | Source region | Destination region | Scale |
|---|---|---|---|---|
| Background 16x16 | 10 | 0 0 16 16 | 0 0 2880 1080 | 180 x 67.5 |
| Debian 1920x1080 | 2000 | 0 0 1920 1080 | 0 135 1440 810 | 0.75 |
| QNX 1280x720 | 1000 | 0 0 1280 720 | 1440 135 1440 810 | 1.125 |

```bash
docker exec dash-test bash -c '
    export XDG_RUNTIME_DIR=/tmp/weston-runtime
    export WAYLAND_DISPLAY=wayland-1
    LayerManagerControl create layer 10 2880 1080
    LayerManagerControl add surface 10 to layer 10
    LayerManagerControl set surface 10 source region 0 0 16 16
    LayerManagerControl set surface 10 destination region 0 0 2880 1080
    LayerManagerControl set layer 10 source region 0 0 2880 1080
    LayerManagerControl set layer 10 destination region 0 0 2880 1080
    LayerManagerControl set layer 10 visibility 1
    LayerManagerControl set surface 10 visibility 1
    LayerManagerControl create layer 2000 2880 1080
    LayerManagerControl add surface 2000 to layer 2000
    LayerManagerControl set surface 2000 source region 0 0 1920 1080
    LayerManagerControl set surface 2000 destination region 0 135 1440 810
    LayerManagerControl set layer 2000 source region 0 0 2880 1080
    LayerManagerControl set layer 2000 destination region 0 0 2880 1080
    LayerManagerControl set layer 2000 visibility 1
    LayerManagerControl set surface 2000 visibility 1
    LayerManagerControl create layer 1000 2880 1080
    LayerManagerControl add surface 1000 to layer 1000
    LayerManagerControl set surface 1000 source region 0 0 1280 720
    LayerManagerControl set surface 1000 destination region 1440 135 1440 810
    LayerManagerControl set layer 1000 source region 0 0 2880 1080
    LayerManagerControl set layer 1000 destination region 0 0 2880 1080
    LayerManagerControl set layer 1000 visibility 1
    LayerManagerControl set surface 1000 visibility 1
    LayerManagerControl set screen 0 render order 10,2000,1000'
```

The proxies need no scale: pointer positions reach them in the guest's own
pixels, and a click at a zone's corner arrives at the guest picture's
corner.

### Keyboard follows the pointer

Give keyboard focus to both surfaces:

```bash
docker exec dash-test bash -c '
    export XDG_RUNTIME_DIR=/tmp/weston-runtime
    export WAYLAND_DISPLAY=wayland-1
    ivi-input-set all'
```

The compositor then delivers every key to both proxies, and each proxy
sends it to its guest only while the pointer is on its surface (section
6). The keyboard therefore goes to the guest under the pointer; with the
pointer on neither surface, keys reach no guest.

`ivi-input-set all` exists in images built from this tree. With an older
image, compile the tool in the running container and call it as
`/tmp/ivi-input-set`:

```bash
docker cp compositor/ivi-input-set.c dash-test:/tmp/ivi-input-set.c
docker exec dash-test sh -c 'gcc /tmp/ivi-input-set.c -I/usr/local/include -L/usr/local/lib \
    -lilmCommon -lilmControl -lilmInput -Wl,-rpath,/usr/local/lib -o /tmp/ivi-input-set'
```

When the compositor's window loses focus on the host desktop, Weston
cancels the keyboard grab and the IVI input controller drops keyboard
focus from both surfaces; focusing the window again does not restore it.
`runbooks/keyboard-focus-watch.sh` runs `ivi-input-set all` each time the
window is focused again. It is a convenience for demos, not a fix in the
compositor:

```bash
runbooks/keyboard-focus-watch.sh &
# with a tool compiled as above:
IVI_INPUT_SET=/tmp/ivi-input-set runbooks/keyboard-focus-watch.sh &
```

Without the watcher, run `ivi-input-set all` again after focusing the
window.

### Stop

Stop each proxy with Ctrl+C or SIGTERM, then the two QEMUs, then the two
bus daemons, then `docker stop dash-test`, which also ends the background
client. Stopping one proxy does not stop the other surface's frames.

### Known issues

- glxgears draws a 16:9 window cut off at the top and bottom. The guest's
  own picture shows the same cut, so it comes from the client, not from
  placement.
- Without the background and the opaque import (proxy builds before this
  change), copies of the cursor stayed on screen where the pointer had
  been: on the background around the zones and in the Debian zone, where
  old frames of the gears also showed through after the view was rotated.
  With both in place, a sweep along every zone edge and across the
  background leaves only the cursor at the pointer, and rotated gears leave
  no trace.
- In one earlier run the Debian picture stopped updating after about three
  minutes (glxgears fell from about 260 frames per second to almost none,
  and surface 2000 received no more frames) with no error in the guest
  kernel log. It has not recurred since: 20 minutes with Debian alone, and
  more than 5 minutes with both guests and input.

## Stop

Stop the proxy with Ctrl+C or SIGTERM. It closes its D-Bus connections,
removes its surface from the compositor and exits with status 0:

```
Shutting down (SIGINT)...
Input: 601 motion, 10 button, 14 key calls; 0 unmapped keys; 7 gated keys; 0 call errors
Shut down: 1 surface(s) removed
```

The `Input:` line counts the calls made to QEMU, the keys dropped because
they have no QEMU key number, the keys not sent because the pointer was
not on the surface, and the calls QEMU answered with an error.

`LayerManagerControl get surfaces` no longer lists surface 1000 and the
compositor keeps running. Then `docker stop dash-test`, then stop QEMU.

The compositor keeps layer 1000 after the proxy exits. A proxy started
again against the same compositor brings surface 1000 back with frames,
but not on the layer and not visible. Restart the container before the
next run ([findings](findings.md#stale-ivi-state)).
