#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Lei Zhou
#
# Re-apply keyboard focus to all guest surfaces each time the compositor's
# window is focused on the host desktop.
#
# When the Weston window (X11 backend) loses focus on the host, Weston
# cancels the keyboard grab and ivi-input-controller drops keyboard focus
# from every surface. Focusing the window again does not restore it, so
# keys reach no guest. This watcher runs `ivi-input-set all` in the
# compositor container on every focus-in. It is a convenience for demos,
# not a fix in the compositor.
#
# Usage:       keyboard-focus-watch.sh          (runs until interrupted)
# Environment: CONTAINER           compositor container (default: dash-test)
#              IVI_INPUT_SET       focus tool in the container (default: ivi-input-set)
#              WESTON_WINDOW_CLASS host window class pattern (default: ^Weston Compositor$)
# Requires:    xdotool on the host, DISPLAY set, docker.
set -eu

CONTAINER=${CONTAINER:-dash-test}
IVI_INPUT_SET=${IVI_INPUT_SET:-ivi-input-set}
WESTON_WINDOW_CLASS=${WESTON_WINDOW_CLASS:-^Weston Compositor\$}

command -v xdotool > /dev/null || { echo "xdotool not found" >&2; exit 1; }
command -v docker > /dev/null || { echo "docker not found" >&2; exit 1; }

# Weston's X11 window sets _NET_WM_NAME but not WM_NAME, which `xdotool search
# --name` reads in some xdotool versions; its WM_CLASS class is matched instead.
echo "Waiting for a window of class '$WESTON_WINDOW_CLASS'; keyboard focus is re-applied on each focus-in"
exec xdotool search --sync --class "$WESTON_WINDOW_CLASS" behave %@ focus exec \
    docker exec "$CONTAINER" sh -c \
    "XDG_RUNTIME_DIR=/tmp/weston-runtime WAYLAND_DISPLAY=wayland-1 $IVI_INPUT_SET all"
