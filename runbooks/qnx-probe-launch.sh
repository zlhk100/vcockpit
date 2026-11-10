#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Lei Zhou
# =============================================================================
# QNX + Debian Dual VM Composition Probe — VERIFIED 2026-04-01
#
#
# QNX display — Screen config on disk-qemu edited once, by hand (docs/qnx-guest.md):
#        /usr/share/screen/graphics-virtio-virgl.conf: no pipelines line, video-mode 1280x720
#        No SSH into QNX required at runtime.
# Debian X — Xorg started in the Debian guest with sudo over SSH each run (idempotent)
#        (startx needs a TTY, which a non-interactive SSH session lacks)
# Layout — QNX layer destination region calculated dynamically from actual surface size
# INPUT — ivi-input-set compiled and deployed in container each run
#          Pointer routing: both surfaces (Weston routes by position)
#          Keyboard focus: Debian (2000) by default
#          Switch focus: run /tmp/ivi-input-set <surface-id> in the container
#          (automatic focus via wl_pointer enter/leave is planned in the proxy)
# =============================================================================

set -e

# =============================================================================
# Path auto-detection — works regardless of where the runbook is installed.
# RUNBOOK_DIR: the directory containing this script.
# WORKSPACE_DIR: the repository root (parent of runbooks/).
# All other paths are auto-detected or use well-known workspace conventions.
# Override any of these by setting env vars before running the script.
# =============================================================================

RUNBOOK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_DIR="$(dirname "$RUNBOOK_DIR")"   # repository root

DOCKER_IMAGE="${DOCKER_IMAGE:-automotive-dash-platform:latest}"
WAYLAND_SOCKET="${WAYLAND_SOCKET:-/tmp/weston-shared/wayland-1}"
WESTON_INI="${WESTON_INI:-/tmp/weston-phase2.ini}"
DEBIAN_IMAGE="${DEBIAN_IMAGE:-$WORKSPACE_DIR/vms/debian-ivi-guest.qcow2}"
QNX_DIR="${QNX_DIR:-$HOME/workspace/qnx8/toolchain/images/qemu/qemu}"
QEMU_BIN="${QEMU_BIN:-$HOME/workspace/qemu/build/qemu-system-x86_64}"
QNX_SDP_ENV="${QNX_SDP_ENV:-$HOME/workspace/qnx8/toolchain/qnxsdp-env.sh}"

# Validate critical paths exist before proceeding
if [ ! -f "$DEBIAN_IMAGE" ]; then
    echo "ERROR: Debian image not found: $DEBIAN_IMAGE"
    echo "  Set DEBIAN_IMAGE env var to override, e.g.:"
    echo "  DEBIAN_IMAGE=/path/to/debian.qcow2 bash $0"
    exit 1
fi
if [ ! -f "$QEMU_BIN" ]; then
    echo "ERROR: QEMU binary not found: $QEMU_BIN"
    echo "  Set QEMU_BIN env var to override"
    exit 1
fi
if [ ! -d "$QNX_DIR/output" ]; then
    echo "ERROR: QNX image directory not found: $QNX_DIR/output"
    echo "  Set QNX_DIR env var to override"
    exit 1
fi

echo "============================================="
echo " QNX + Debian Dual VM Composition Probe"
echo " Runbook dir:  $RUNBOOK_DIR"
echo " Workspace:    $WORKSPACE_DIR"
echo " Debian image: $DEBIAN_IMAGE"
echo " QNX dir:      $QNX_DIR"
echo "============================================="

# Step 1: Write weston.ini
echo "[1/6] Writing weston.ini..."
cat > "$WESTON_INI" << 'EOF'
[core]
shell=ivi-shell.so
modules=ivi-controller.so

[ivi-shell]
ivi-input-module=ivi-input-controller.so
ivi-id-agent-module=ivi-id-agent.so

[desktop-app]
app-title=QEMU (qemu-debian-0)
surface-id=2000

[desktop-app]
app-title=QEMU (qemu-qnx-0)
surface-id=1000

[desktop-app-default]
default-surface-id=30
default-surface-id-max=200

[output]
name=X1
mode=3840x1080
EOF

# Step 2: Start Weston IVI container
echo "[2/6] Starting Weston IVI container..."
# The compositor sets its runtime directory to mode 0700 at start-up. Create
# the host side of that bind mount owned by this user first: a directory that
# Docker creates is owned by root, and this user's QEMU could not reach the
# Wayland socket inside it.
sudo mkdir -p /tmp/weston-shared
sudo chown "$(id -u):$(id -g)" /tmp/weston-shared
xhost +local:docker
docker run --rm -d \
    -v /tmp/weston-shared:/tmp/weston-runtime \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v "$WESTON_INI":/etc/xdg/weston/weston.ini:ro \
    -e DISPLAY=$DISPLAY \
    --device=/dev/dri/card1 \
    --device=/dev/dri/renderD128 \
    --name dash-test \
    "$DOCKER_IMAGE"

for i in $(seq 1 30); do
    [ -S "$WAYLAND_SOCKET" ] && break
    sleep 1
done
sleep 2
sudo chmod 777 "$WAYLAND_SOCKET"
echo "    Weston ready"

# Step 3: Launch both VMs simultaneously
echo "[3/6] Launching Debian and QNX VMs simultaneously..."
export XDG_RUNTIME_DIR=/tmp/weston-shared
export WAYLAND_DISPLAY=wayland-1
export SDL_VIDEODRIVER=wayland

# Debian VM — app-id maps to surface ID 2000
export SDL_VIDEO_WAYLAND_WMCLASS="qemu-debian"
"$QEMU_BIN" \
    -name "qemu-debian" \
    -machine q35,accel=kvm \
    -cpu host \
    -smp 2 \
    -m 2G \
    -drive file="$DEBIAN_IMAGE",if=virtio,format=qcow2 \
    -device virtio-vga-gl \
    -display sdl,gl=on \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=net0 &
DEBIAN_PID=$!

# QNX VM — launched in subshell to prevent env var pollution
# app-id maps to surface ID 1000
# Requires the one-time QNX Screen config edit (docs/qnx-guest.md)
#      /usr/share/screen/graphics-virtio-virgl.conf — do not replace disk-qemu without re-applying
(
    export SDL_VIDEO_WAYLAND_WMCLASS="qemu-qnx"
    source "$QNX_SDP_ENV" > /dev/null 2>&1
    export SDL_VIDEODRIVER=wayland
    export XDG_RUNTIME_DIR=/tmp/weston-shared
    export WAYLAND_DISPLAY=wayland-1
    exec "$QEMU_BIN" \
        -name "qemu-qnx" \
        --enable-kvm \
        -smp 4 \
        -m 4G \
        -cpu host,host-phys-bits-limit=40 \
        -drive file="$QNX_DIR/output/disk-qemu",format=raw,if=ide,id=drv0 \
        -kernel "$QNX_DIR/output/ifs.bin" \
        -serial file:/tmp/qnx-serial.log \
        -object rng-random,filename=/dev/urandom,id=rng0 \
        -device virtio-rng-pci,rng=rng0 \
        -vga none \
        -display sdl,gl=on \
        -device virtio-gpu-gl-pci,max_outputs=1,edid=on,xres=1280,yres=720 \
        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2223-:22 \
        -device virtio-net-pci,netdev=net0
) &
QNX_PID=$!

echo "    Debian PID: $DEBIAN_PID  QNX PID: $QNX_PID"

# Step 4: Wait for surface IDs 1000 (QNX) and 2000 (Debian) to appear
echo "[4/6] Waiting for surfaces 1000 (QNX) and 2000 (Debian) up to 120s..."
for i in $(seq 1 120); do
    SURFACES=$(docker exec dash-test bash -c '
        export XDG_RUNTIME_DIR=/tmp/weston-runtime
        export WAYLAND_DISPLAY=wayland-1
        LayerManagerControl get surfaces 2>/dev/null | grep "Surface" | grep -o "Surface [0-9]*" | awk "{print \$2}"
    ' 2>/dev/null)
    HAS_QNX=$(echo "$SURFACES" | grep -c "^1000$" || true)
    HAS_DEBIAN=$(echo "$SURFACES" | grep -c "^2000$" || true)
    printf "    ${i}s — QNX(1000):${HAS_QNX} Debian(2000):${HAS_DEBIAN}\r"
    if [ "$HAS_QNX" -ge 1 ] && [ "$HAS_DEBIAN" -ge 1 ]; then
        echo ""
        echo "    Both surfaces registered: QNX=1000 Debian=2000"
        break
    fi
    sleep 1
done
echo ""

# -----------------------------------------------------------------------------
# Start Xorg as root (sudo) to bypass VT permission issues.
# startx fails over SSH (no TTY → cannot open virtual console).
# sudo Xorg works reliably — glxgears launched with root XAUTHORITY.
# Idempotent — safe to apply on every run.
# -----------------------------------------------------------------------------
echo "[5a/6] Starting Xorg + glxgears in Debian guest..."
echo "    Waiting for Debian SSH..."
for i in $(seq 1 30); do
    ssh -p 2222 -o ConnectTimeout=2 -o StrictHostKeyChecking=no \
        debian@localhost "echo ok" > /dev/null 2>&1 && break
    sleep 2
    echo "    Waiting for SSH... (attempt $i/30)"
done

# Kill any existing Xorg
ssh -p 2222 -o StrictHostKeyChecking=no debian@localhost \
    "pkill Xorg 2>/dev/null || true; sleep 1"

# Start Xorg as root (bypasses VT open permission denied over SSH)
ssh -p 2222 -o StrictHostKeyChecking=no debian@localhost \
    "nohup sudo Xorg :0 -ac -noreset -nolisten tcp > /tmp/xorg.log 2>&1 &"
sleep 3

# Verify Xorg started
ssh -p 2222 -o StrictHostKeyChecking=no debian@localhost \
    "ls /tmp/.X11-unix/X0 2>/dev/null && echo '    Xorg started OK' || echo '    WARNING: Xorg did not start'"

# Launch glxgears with root XAUTHORITY
ssh -p 2222 -o StrictHostKeyChecking=no debian@localhost \
    "DISPLAY=:0 XAUTHORITY=/root/.Xauthority nohup glxgears -geometry 1920x1056 > /tmp/glxgears.log 2>&1 &"
echo "    Debian Xorg + glxgears started"

# -----------------------------------------------------------------------------
# Dynamic layer destination region based on actual QNX surface size.
# Reads actual width/height from LayerManagerControl, calculates aspect-correct
# height for the 1440px-wide right zone, centers vertically in 1080px output.
# -----------------------------------------------------------------------------
echo "[5b/6] Reading actual QNX surface size for dynamic layer assignment..."
QNX_SIZE=$(docker exec dash-test bash -c '
    export XDG_RUNTIME_DIR=/tmp/weston-runtime
    export WAYLAND_DISPLAY=wayland-1
    LayerManagerControl get surface 1000 2>/dev/null | grep -i "original size"
' 2>/dev/null)
echo "    QNX surface info: $QNX_SIZE"

# Parse width and height from "original size: x=W, y=H"
QNX_W=$(echo "$QNX_SIZE" | grep -o "x=[0-9]*" | grep -o "[0-9]*")
QNX_H=$(echo "$QNX_SIZE" | grep -o "y=[0-9]*" | grep -o "[0-9]*")

# Fallback to safe defaults if parsing fails
if [ -z "$QNX_W" ] || [ -z "$QNX_H" ] || [ "$QNX_W" -eq 0 ]; then
    echo "    WARNING: Could not parse QNX surface size — using fallback 1280x768"
    QNX_W=1280
    QNX_H=768
fi

echo "    QNX surface size: ${QNX_W}x${QNX_H}"

# Aspect-correct destination: 1440px wide right zone, centered vertically
DEST_W=1440
DEST_H=$((DEST_W * QNX_H / QNX_W))
DEST_Y=$(( (1080 - DEST_H) / 2 ))
[ "$DEST_Y" -lt 0 ] && DEST_Y=0

echo "    QNX layer destination: x=1440 y=${DEST_Y} w=${DEST_W} h=${DEST_H}"

echo "[5c/6] Assigning surfaces to layers..."
docker exec dash-test bash -c "
    export XDG_RUNTIME_DIR=/tmp/weston-runtime
    export WAYLAND_DISPLAY=wayland-1

    echo 'Current surfaces:'
    LayerManagerControl get surfaces

    # Debian = surface 2000, left half (1440px wide)
    LayerManagerControl create layer 4000 1920 1080 2>/dev/null || true
    LayerManagerControl add surface 2000 to layer 4000
    LayerManagerControl set surface 2000 destination region 0 0 1920 1080
    LayerManagerControl set surface 2000 source region 0 0 1920 1080
    LayerManagerControl set layer 4000 destination region 0 0 1440 1080
    LayerManagerControl set layer 4000 visibility 1
    LayerManagerControl set surface 2000 visibility 1

    # QNX = surface 1000, right half — aspect-correct
    LayerManagerControl create layer 3000 ${QNX_W} ${QNX_H} 2>/dev/null || true
    LayerManagerControl add surface 1000 to layer 3000
    LayerManagerControl set surface 1000 destination region 0 0 ${QNX_W} ${QNX_H}
    LayerManagerControl set surface 1000 source region 0 0 ${QNX_W} ${QNX_H}
    LayerManagerControl set layer 3000 destination region 1440 ${DEST_Y} ${DEST_W} ${DEST_H}
    LayerManagerControl set layer 3000 visibility 1
    LayerManagerControl set surface 1000 visibility 1

    LayerManagerControl set screen 0 render order 4000,3000
    echo 'Done: Debian(2000,left) | QNX(1000,right)'
"

# -----------------------------------------------------------------------------
# INPUT FIX: Build and deploy ivi-input-set inside container each run.
# Sets pointer+touch focus per-surface (Weston routes by cursor position).
# Sets keyboard focus to Debian (2000) by default.
# The proxy is planned to switch keyboard focus automatically via wl_pointer events.
# -----------------------------------------------------------------------------
echo "[5d/6] Building ivi-input-set and configuring input focus..."
docker exec dash-test bash -c '
cat > /tmp/ivi-input-set.c << CSRC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ilm/ilm_common.h"
#include "ilm/ilm_control.h"
#include "ilm/ilm_input.h"

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <surface_id>\n", argv[0]);
        fprintf(stderr, "  Gives keyboard focus to one surface, pointer to both\n");
        return 1;
    }
    t_ilm_surface target = (t_ilm_surface)atoi(argv[1]);
    t_ilm_surface both[2] = { 1000, 2000 };

    if (ilm_init() != ILM_SUCCESS) {
        fprintf(stderr, "ilm_init() failed\n");
        return 1;
    }

    t_ilm_string seat = NULL;
    ilm_getDefaultSeat(&seat);
    if (!seat) seat = strdup("default");

    /* Accept seat input on both surfaces */
    for (int i = 0; i < 2; i++)
        ilm_setInputAcceptanceOn(both[i], 1, &seat);

    /* Clear keyboard from all surfaces */
    ilm_setInputFocus(both, 2, ILM_INPUT_DEVICE_KEYBOARD, ILM_FALSE);

    /* Set keyboard only on target surface */
    ilm_setInputFocus(&target, 1, ILM_INPUT_DEVICE_KEYBOARD, ILM_TRUE);

    /* Set pointer+touch per surface — Weston routes by cursor position */
    for (int i = 0; i < 2; i++)
        ilm_setInputFocus(&both[i], 1,
            ILM_INPUT_DEVICE_POINTER | ILM_INPUT_DEVICE_TOUCH, ILM_TRUE);

    ilm_commitChanges();
    printf("Keyboard focus -> surface %u\n", target);
    ilm_destroy();
    free(seat);
    return 0;
}
CSRC
gcc /tmp/ivi-input-set.c \
    -I/usr/local/include \
    -L/usr/local/lib \
    -lilmCommon -lilmControl -lilmInput \
    -Wl,-rpath,/usr/local/lib \
    -o /tmp/ivi-input-set
' && echo "    ivi-input-set compiled OK" || echo "    WARNING: ivi-input-set compile failed"

# Set initial keyboard focus to Debian (left panel)
docker exec dash-test bash -c "
    export XDG_RUNTIME_DIR=/tmp/weston-runtime
    export WAYLAND_DISPLAY=wayland-1
    /tmp/ivi-input-set 2000
" && echo "    Initial keyboard focus: Debian (surface 2000)" || \
   echo "    WARNING: Could not set input focus"

# Step 6: Verification
echo "[6/6] Scene verification:"
docker exec dash-test bash -c '
    export XDG_RUNTIME_DIR=/tmp/weston-runtime
    export WAYLAND_DISPLAY=wayland-1
    LayerManagerControl get scene
' | grep -E "render order|surface [0-9]|original size|frame counter"

echo ""
echo "============================================="
echo " Left:  Debian VM  (surface 2000)"
echo " Right: QNX demolauncher  (surface 1000)"
echo " QNX serial: tail -f /tmp/qnx-serial.log"
echo ""
echo " Keyboard focus (run in any terminal):"
echo "   Switch to Debian: docker exec dash-test bash -c 'export XDG_RUNTIME_DIR=/tmp/weston-runtime; export WAYLAND_DISPLAY=wayland-1; /tmp/ivi-input-set 2000'"
echo "   Switch to QNX:    docker exec dash-test bash -c 'export XDG_RUNTIME_DIR=/tmp/weston-runtime; export WAYLAND_DISPLAY=wayland-1; /tmp/ivi-input-set 1000'"
echo ""
echo " To stop: kill $DEBIAN_PID $QNX_PID && docker stop dash-test"
echo "============================================="
