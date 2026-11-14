<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# Codelab: QNX 8.0 graphics from QEMU into a Wayland compositor

**What you get:** in about an hour, a QNX 8.0 Screen/OpenGL ES application running in QEMU appears as a surface in a
Wayland compositor on your Linux desktop — the frames go from QNX's virtual GPU to the compositor as dma-buf file
descriptors, never copied. From there it can be placed, scaled and composed with other virtual machines, the way a
vehicle cockpit composes an instrument cluster with an infotainment system.

**What you learn:**

- which QEMU display and GPU options QNX 8.0's Screen needs, and why
- how QEMU's D-Bus display hands frames to another process
- how a COVESA IVI compositor names, places and stacks surfaces
- how to build a Screen + GLES 2 program with the QNX SDP 8.0 toolchain and run it in the guest

**Compared with what you may use today:** QEMU's own SDL or GTK window shows one VM per window and nothing else. QEMU's
D-Bus display (`-display dbus`) lets a separate process receive the frames; general-purpose viewers exist
(qemu-display). This codelab uses a small purpose-built proxy that turns each guest display into a numbered surface of
an automotive IVI compositor, so its position and stacking are set by policy, not by a window manager.

## What you need

- Linux host with a GPU that has a Mesa driver with dma-buf export (tested: Ubuntu 22.04, AMD RDNA3)
- Docker; `meson`, `ninja`, a C compiler, `libwayland-dev`, `libgbm-dev`, `libglib2.0-dev`, `wayland-protocols`
- QNX SDP 8.0 and its QEMU target image, installed with QNX Software Center under your own licence (a free
  non-commercial licence is available through QNX Everywhere)
- QEMU 9.2 built with OpenGL, virglrenderer and the D-Bus display (step 1)
- This repository: `git clone https://github.com/zlhk100/vcockpit && cd vcockpit`

Paths below use `QNX_DIR` for the QEMU target image directory (the one that contains `output/ifs.bin`) and
`QEMU_BIN` for your QEMU binary:

```bash
export QNX_DIR=~/qnx800/images/qemu/qemu          # adjust to your install
export QEMU_BIN=~/qemu/build/qemu-system-x86_64   # adjust to your install
```

Steps 3–8 use two terminals in the repository directory: the first runs the bus, QEMU and the proxy; the second
the compositor, the placement commands and the guest console. Set `QNX_DIR` and `QEMU_BIN` in the first.

## 1. QEMU with OpenGL and the D-Bus display

QEMU 9.2.4, unmodified, configured with:

```bash
./configure --target-list=x86_64-softmmu --enable-kvm --enable-opengl \
    --enable-virglrenderer --enable-slirp
make -j"$(nproc)"
```

The D-Bus display is built whenever GLib's GIO is available. Check:

```bash
"$QEMU_BIN" -display help | grep -x dbus
```

## 2. A disk overlay and the one Screen setting QEMU needs

Never write the image QNX Software Center installed. Put every change into an overlay; QEMU opens the original
read-only:

```bash
qemu-img create -f qcow2 -b "$QNX_DIR/output/disk-qemu" -F raw "$QNX_DIR/output/codelab.qcow2"
```

The image's Screen configuration does not match the display QEMU offers. Boot the overlay on the serial console:

```bash
"$QEMU_BIN" -enable-kvm -smp 4 -m 4G -cpu host,host-phys-bits-limit=40 \
    -kernel "$QNX_DIR/output/ifs.bin" \
    -drive file="$QNX_DIR/output/codelab.qcow2",format=qcow2,if=ide \
    -nographic -serial mon:stdio
```

Log in as `root` (password `root` on the QNX Everywhere QEMU image) and run:

```sh
sed -i '/pipelines/d' /usr/share/screen/graphics-virtio-virgl.conf
sed -i 's/video-mode = 1280 x 768 @ 60/video-mode = 1280 x 720 @ 60/' \
    /usr/share/screen/graphics-virtio-virgl.conf
sync; shutdown -b
```

When the console goes quiet, leave QEMU with `Ctrl+A X`.

- The `gpu_virtio` driver rejects the `pipelines` option and Screen exits without the first edit.
- A video mode that differs from QEMU's `xres`/`yres` makes the EDID negotiation fail and Screen falls back to 720x400.
- Use `sed`, not `vi`: editing with `vi` over a serial console corrupts lines.
- QNX `shutdown` without `-b` reboots.

## 3. Boot QNX with the D-Bus display

Each QEMU with `-display dbus` claims the bus name `org.qemu`, so give the guest a private bus. In the first
terminal:

```bash
dbus-daemon --session --address=unix:path=/tmp/codelab-bus --nofork --print-address &
BUS_PID=$!
"$QEMU_BIN" -name qemu-qnx -enable-kvm -smp 4 -m 4G -cpu host,host-phys-bits-limit=40 \
    -kernel "$QNX_DIR/output/ifs.bin" \
    -drive file="$QNX_DIR/output/codelab.qcow2",format=qcow2,if=ide -snapshot \
    -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
    -object memory-backend-memfd,id=mem,size=4G,share=on -machine memory-backend=mem \
    -vga none -display dbus,gl=on,addr=unix:path=/tmp/codelab-bus \
    -device virtio-gpu-gl-pci,max_outputs=1,edid=on,xres=1280,yres=720 \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -serial unix:/tmp/codelab-serial,server=on,wait=off &
QEMU_PID=$!
```

Why each option:

| Option | Without it |
|---|---|
| `-vga none` | QEMU adds a legacy VGA console as console 0; the virtio GPU becomes console 1 and the guest comes up at 720x400 |
| `virtio-gpu-gl-pci` (not `virtio-vga-gl`) | the device can fall back to legacy VGA |
| `max_outputs=1` | Screen spreads the desktop, launcher and each window over QEMU's default 16 outputs |
| `virtio-rng-pci` with `rng-random` | the guest has no hardware random source; the program in step 7 draws its first frames and then stops |
| `memory-backend-memfd,share=on` | guest memory cannot be shared as dma-buf |
| `-snapshot` | everything you do in the guest is written to the overlay |

Check that QEMU is on the bus:

```bash
DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/codelab-bus gdbus call --session \
    --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.ListNames | grep -o org.qemu
```

## 4. The compositor

Weston 10 with the COVESA IVI shell, in a container, drawing into a window on your desktop. In the second
terminal:

```bash
docker build -t automotive-dash-platform:latest compositor/
mkdir -p /tmp/weston-shared        # yours, not root's: QEMU and the proxy must reach the socket
cat > /tmp/codelab-weston.ini <<'INI'
[core]
shell=ivi-shell.so
modules=ivi-controller.so

[ivi-shell]
ivi-input-module=ivi-input-controller.so
ivi-id-agent-module=ivi-id-agent.so

[desktop-app]
app-title=QEMU (qemu-qnx-0)
surface-id=1000
INI
xhost +local:docker
docker run --rm -d --name codelab-weston \
    -v /tmp/weston-shared:/tmp/weston-runtime -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v /tmp/codelab-weston.ini:/etc/xdg/weston/weston.ini:ro -e DISPLAY="$DISPLAY" \
    -e WESTON_WIDTH=1280 -e WESTON_HEIGHT=720 \
    --device=/dev/dri/card1 --device=/dev/dri/renderD128 \
    automotive-dash-platform:latest                     # card/render nodes: see ls /dev/dri
sleep 3; docker exec codelab-weston chmod 777 /tmp/weston-runtime/wayland-1
```

`ivi-id-agent` gives a client whose window title matches `app-title` the IVI surface number `1000`.

## 5. The proxy

In the first terminal (the proxy stays in the foreground):

```bash
meson setup proxy/build proxy && ninja -C proxy/build
XDG_RUNTIME_DIR=/tmp/weston-shared WAYLAND_DISPLAY=wayland-1 \
DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/codelab-bus \
    proxy/build/automotive-dbus-proxy --vm-name qnx --wayland-socket wayland-1 --n-scanouts 1
```

Expect `RegisterListener OK`, then `ScanoutDMABUF: scanout=0 1280x720 …` once QNX's Screen has started (about a
minute after boot). The proxy names its surface `QEMU (qemu-qnx-0)`, so it becomes surface 1000. Its start-up hint
names the container `dash-test`, which the repository's own scripts use; here it is `codelab-weston`.

## 6. Place the surface

The IVI shell shows nothing it has not been told to place. In the second terminal:

```bash
docker exec codelab-weston sh -c 'export XDG_RUNTIME_DIR=/tmp/weston-runtime WAYLAND_DISPLAY=wayland-1
  LayerManagerControl create layer 1000 1280 720
  LayerManagerControl add surface 1000 to layer 1000
  LayerManagerControl set surface 1000 source region 0 0 1280 720
  LayerManagerControl set surface 1000 destination region 0 0 1280 720
  LayerManagerControl set layer 1000 visibility 1
  LayerManagerControl set surface 1000 visibility 1
  LayerManagerControl set screen 0 render order 1000'
```

The QNX desktop is now in the compositor's window. Change the destination region and the same frames are scaled
and moved — no change in the guest.

## 7. Your own Screen + GLES 2 program

Build the repository's instrument cluster with the SDP toolchain:

```bash
. ~/qnx800/qnxsdp-env.sh                            # adjust to your install; in this shell only
make -C qnx/cluster-gauge
```

Serve it on the loopback address and fetch it from the guest (the host is `10.0.2.2` under QEMU user networking):

```bash
python3 -m http.server 8099 --bind 127.0.0.1 --directory qnx/cluster-gauge/build &
HTTP_PID=$!
socat - UNIX-CONNECT:/tmp/codelab-serial            # the guest console; log in as root
```

```sh
cd /tmp && curl -sS -O http://10.0.2.2:8099/cluster-gauge && chmod +x cluster-gauge
./cluster-gauge
```

The gauge fills the surface at 60 frames per second. What makes it work on this virtual GPU, in
`qnx/cluster-gauge/src/main.c`: the EGL config is picked by its native visual ID equal to `SCREEN_FORMAT_RGBX8888`,
because `eglChooseConfig` may return a 10-bit format that the virtio display rejects.

## 8. Clean up

In the first terminal, `Ctrl+C` the proxy, then:

```bash
kill "$QEMU_PID" "$BUS_PID"      # QEMU ran with -snapshot: the overlay is unchanged
```

`socat` in the second terminal ends when QEMU stops. Then, in the second terminal:

```bash
kill "$HTTP_PID"
docker stop codelab-weston; xhost -local:docker
rm -f /tmp/codelab-bus /tmp/codelab-serial /tmp/codelab-weston.ini
```

QEMU and `dbus-daemon` remove their sockets when they exit; `rm -f` covers an interrupted run.

## Where to go next

- `runbooks/vcockpit.sh start` runs a QNX guest and a Debian guest side by side, each through its own proxy, with
  keyboard and mouse, and the guests' content starting at boot — see `docs/running-the-proxy.md`.
- `qnx/telltale-monitor` reads the composed display back with `screen_read_display` and checks it against what the
  gauge commands.
- `docs/findings.md` lists every pitfall met on the way.
