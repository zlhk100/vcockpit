#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Lei Zhou
#
# Start, stop and check the whole cockpit with one command: the compositor
# container and its background surface, a QNX guest and a Debian guest each
# with its own D-Bus bus, one proxy per guest, the placement of both guest
# surfaces side by side, and keyboard focus.
#
# The guests start their own content at boot (see docs/qnx-guest.md and
# guest/debian/README.md), so this script never logs into a guest. Both guest
# disks are opened with -snapshot: a run never changes them.
#
# Every run gets its own directory with one log per process, the launcher's
# log, the recorded process IDs and a status snapshot. Run directories are
# never deleted by this script.
#
# Usage: vcockpit.sh start | stop | status | --help
set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_DIR=$(dirname -- "$SCRIPT_DIR")

# ---------------------------------------------------------------- configuration
QNX_DIR=${QNX_DIR:-$HOME/workspace/qnx8/toolchain/images/qemu/qemu/output}
QEMU_BIN=${QEMU_BIN:-$HOME/workspace/qemu/build/qemu-system-x86_64}
PROXY_BIN=${PROXY_BIN:-$REPO_DIR/proxy/build/automotive-dbus-proxy}
QNX_DISK=${QNX_DISK:-$QNX_DIR/vcockpit-qnx.qcow2}
QNX_IFS=${QNX_IFS:-$QNX_DIR/ifs.bin}
DEBIAN_DISK=${DEBIAN_DISK:-$REPO_DIR/vms/vcockpit-debian.qcow2}
COMPOSITOR_IMAGE=${COMPOSITOR_IMAGE:-automotive-dash-platform:latest}
WESTON_INI=${WESTON_INI:-}
WESTON_WIDTH=${WESTON_WIDTH:-}
WESTON_HEIGHT=${WESTON_HEIGHT:-1080}
DRI_CARD=${DRI_CARD:-/dev/dri/card1}
DRI_RENDER=${DRI_RENDER:-/dev/dri/renderD128}
VCOCKPIT_RUN_DIR=${VCOCKPIT_RUN_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/vcockpit/runs}

CONTAINER=dash-test
SHARED=/tmp/weston-shared
BUS_QNX=/tmp/vcockpit-bus-qnx
BUS_DEBIAN=/tmp/vcockpit-bus-debian
CURRENT=$VCOCKPIT_RUN_DIR/current

RUN=
LOG=/dev/null

usage() {
	cat <<EOF
Usage: vcockpit.sh start | stop | status | --help

  start   Start compositor, background, both guests, both proxies, placement
          and keyboard focus. Refuses if a session is already running. On any
          failure it stops what it started and exits non-zero.
  stop    Stop everything the current run started; exit 0 if nothing it
          recorded is left running.
  status  Show the recorded processes and the guest surfaces; exit 0 if all
          recorded processes are alive.

Environment (defaults in brackets):
  QEMU_BIN          QEMU with the D-Bus display [$QEMU_BIN]
  PROXY_BIN         automotive-dbus-proxy [$PROXY_BIN]
  QNX_DISK          QNX disk with the autostart [$QNX_DISK]
  QNX_IFS           QNX kernel image [$QNX_IFS]
  DEBIAN_DISK       Debian disk with the autostart [$DEBIAN_DISK]
  COMPOSITOR_IMAGE  compositor image [$COMPOSITOR_IMAGE]
  WESTON_INI        weston.ini to mount [generated in the run directory]
  WESTON_WIDTH      output width [width of the X root window]
  WESTON_HEIGHT     output height [$WESTON_HEIGHT]
  DRI_CARD, DRI_RENDER  host GPU nodes for the container [$DRI_CARD, $DRI_RENDER]
  VCOCKPIT_RUN_DIR  where run directories go [$VCOCKPIT_RUN_DIR]

Each run: VCOCKPIT_RUN_DIR/<UTC time>/ with launcher.log, compositor.log,
qemu-qnx.log (QEMU messages and the QNX serial console), qemu-debian.log,
bus-qnx.log, bus-debian.log, proxy-qnx.log, proxy-debian.log,
focus-watch.log, pids and status.txt; VCOCKPIT_RUN_DIR/current points to it.
EOF
}

# ---------------------------------------------------------------- helpers
log() {
	printf '%s %s\n' "$(date -u +%H:%M:%S)" "$*" >> "$LOG"
	printf 'vcockpit: %s\n' "$*"
}

die() {
	log "FAILED: $*"
	if [ -n "$RUN" ]; then
		log "stopping what was started"
		do_stop || log "stop after failure left processes running"
		log "run directory: $RUN"
	fi
	exit 1
}

# Run a command with its output appended to the launcher log; log the result.
run_logged() {
	printf '%s $ %s\n' "$(date -u +%H:%M:%S)" "$*" >> "$LOG"
	if "$@" >> "$LOG" 2>&1; then
		return 0
	else
		rc=$?
		printf '%s [exit %s]\n' "$(date -u +%H:%M:%S)" "$rc" >> "$LOG"
		return "$rc"
	fi
}

# wait_for <seconds> <description> <command...>: poll every 0.5 s.
wait_for() {
	limit=$(($1 * 2)); what=$2; shift 2
	n=0
	while ! "$@" > /dev/null 2>&1; do
		n=$((n + 1))
		[ "$n" -lt "$limit" ] || return 1
		sleep 0.5
	done
	log "ready: $what ($((n / 2)) s)"
}

lmc() {
	docker exec -e XDG_RUNTIME_DIR=/tmp/weston-runtime -e WAYLAND_DISPLAY=wayland-1 \
		"$CONTAINER" LayerManagerControl "$@"
}

surface_present() {
	lmc get surfaces 2>/dev/null | grep -q "Surface $1 "
}

# surface_size_is <id> <w> <h>: the surface shows a guest frame of that size.
surface_size_is() {
	lmc get surface "$1" 2>/dev/null | grep -q "original size: *x=$2, y=$3\$"
}

compositor_started() {
	docker logs "$CONTAINER" 2>/dev/null | grep -q 'Compositor started'
}

frame_counter() {
	lmc get surface "$1" 2>/dev/null | awk '/frame counter/ { print $NF }'
}

buses_up() {
	[ -S "$BUS_QNX" ] && [ -S "$BUS_DEBIAN" ]
}

bus_has_qemu() {
	gdbus call --address "unix:path=$1" --dest org.freedesktop.DBus \
		--object-path /org/freedesktop/DBus \
		--method org.freedesktop.DBus.NameHasOwner org.qemu 2>/dev/null | grep -q true
}

container_exists() {
	docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER"
}

port_in_use() {
	ss -ltn | awk '{ print $4 }' | grep -q ":$1\$"
}

# Does process <pid> still run the program recorded as <name>? Checked by its
# command line, so a reused PID is never signalled.
pid_is() {
	[ -r "/proc/$2/cmdline" ] || return 1
	cmd=$(tr '\0' ' ' < "/proc/$2/cmdline")
	case $1 in
	bus-qnx)      pat="--address=unix:path=$BUS_QNX " ;;
	bus-debian)   pat="--address=unix:path=$BUS_DEBIAN " ;;
	qemu-qnx)     pat="-name qemu-qnx " ;;
	qemu-debian)  pat="-name qemu-debian " ;;
	proxy-qnx)    pat="--vm-name qnx " ;;
	proxy-debian) pat="--vm-name debian " ;;
	focus-watch)
		case $cmd in
		*keyboard-focus-watch.sh*|*" behave "*) return 0 ;;
		*) return 1 ;;
		esac ;;
	*) return 1 ;;
	esac
	case $cmd in
	*"$pat"*) return 0 ;;
	*) return 1 ;;
	esac
}

record() {
	printf '%s=%s\n' "$1" "$2" >> "$RUN/pids"
	log "started $1 (pid $2)"
}

# Print "name pid" for every recorded process of the current run.
recorded() {
	[ -f "$CURRENT/pids" ] || return 0
	while IFS='=' read -r name pid; do
		[ -n "$name" ] && printf '%s %s\n' "$name" "$pid"
	done < "$CURRENT/pids"
}

any_recorded_alive() {
	recorded | while read -r name pid; do
		if pid_is "$name" "$pid"; then echo "$name"; fi
	done | grep -q .
}

write_ini() {
	cat > "$1" <<'EOF'
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
EOF
}

# ---------------------------------------------------------------- start
check_prerequisites() {
	for c in docker xdotool xwininfo xhost dbus-daemon gdbus ss awk; do
		command -v "$c" > /dev/null 2>&1 || die "command not found: $c"
	done
	[ -x "$QEMU_BIN" ] || die "QEMU_BIN not executable: $QEMU_BIN"
	[ -x "$PROXY_BIN" ] || die "PROXY_BIN not executable: $PROXY_BIN (build: meson setup proxy/build proxy && ninja -C proxy/build)"
	[ -f "$QNX_DISK" ] || die "QNX disk not found: $QNX_DISK (set QNX_DISK)"
	[ -f "$QNX_IFS" ] || die "QNX kernel image not found: $QNX_IFS (set QNX_IFS)"
	[ -f "$DEBIAN_DISK" ] || die "Debian disk not found: $DEBIAN_DISK (set DEBIAN_DISK)"
	[ -z "$WESTON_INI" ] || [ -f "$WESTON_INI" ] || die "WESTON_INI not found: $WESTON_INI"
	[ -n "${DISPLAY:-}" ] || die "DISPLAY is not set"
	docker image inspect "$COMPOSITOR_IMAGE" > /dev/null 2>&1 || die "image not found: $COMPOSITOR_IMAGE"
}

check_not_running() {
	if any_recorded_alive; then
		die "a session is running (see $CURRENT); run 'vcockpit.sh stop' first"
	fi
	if container_exists; then die "container $CONTAINER exists; stop it first"; fi
	for s in "$BUS_QNX" "$BUS_DEBIAN"; do
		[ ! -e "$s" ] || die "bus socket $s exists; another session is running or it is stale"
	done
	for p in 2222 2223; do
		if port_in_use "$p"; then die "port 127.0.0.1:$p is in use"; fi
	done
}

compute_layout() {
	if [ -z "$WESTON_WIDTH" ]; then
		WESTON_WIDTH=$(xwininfo -root | awk '/Width:/ { print $2 }')
	fi
	case $WESTON_WIDTH$WESTON_HEIGHT in
	*[!0-9]*|'') die "WESTON_WIDTH/WESTON_HEIGHT not numeric: '$WESTON_WIDTH' '$WESTON_HEIGHT'" ;;
	esac
	ZW=$((WESTON_WIDTH / 2))
	ZH=$((ZW * 9 / 16 / 2 * 2))
	[ "$ZH" -le "$WESTON_HEIGHT" ] || die "output ${WESTON_WIDTH}x$WESTON_HEIGHT too wide for two 16:9 zones"
	ZY=$(((WESTON_HEIGHT - ZH) / 2))
	log "output ${WESTON_WIDTH}x$WESTON_HEIGHT; zones ${ZW}x$ZH at y $ZY (Debian x 0, QNX x $ZW)"
}

start_compositor() {
	if [ ! -d "$SHARED" ]; then
		mkdir "$SHARED" || die "cannot create $SHARED"
	fi
	[ "$(stat -c %u "$SHARED")" = "$(id -u)" ] || die "$SHARED is not owned by you (see docs/running-the-proxy.md)"
	run_logged xhost +local:docker || die "xhost +local:docker"
	XHOST_GRANTED=1
	if [ -z "$WESTON_INI" ]; then
		WESTON_INI=$RUN/weston.ini
		write_ini "$WESTON_INI"
	fi
	run_logged docker run --rm -d \
		-v "$SHARED:/tmp/weston-runtime" \
		-v /tmp/.X11-unix:/tmp/.X11-unix \
		-v "$WESTON_INI:/etc/xdg/weston/weston.ini:ro" \
		-e DISPLAY="$DISPLAY" \
		-e WESTON_WIDTH="$WESTON_WIDTH" -e WESTON_HEIGHT="$WESTON_HEIGHT" \
		--device="$DRI_CARD" --device="$DRI_RENDER" \
		--name "$CONTAINER" "$COMPOSITOR_IMAGE" || die "docker run"
	# The socket file of an earlier run may still exist: wait for this compositor's own start line.
	wait_for 20 "compositor" compositor_started || die "compositor not started after 20 s"
	test -S "$SHARED/wayland-1" || die "no Wayland socket in $SHARED"
	run_logged docker exec "$CONTAINER" chmod 777 /tmp/weston-runtime/wayland-1 || die "chmod of the Wayland socket"
	run_logged docker exec -d -e XDG_RUNTIME_DIR=/tmp/weston-runtime -e WAYLAND_DISPLAY=wayland-1 \
		"$CONTAINER" /usr/local/bin/ivi-background || die "start ivi-background"
	wait_for 10 "background surface 10" surface_present 10 || die "no surface 10 after 10 s"
}

start_buses() {
	nohup dbus-daemon --session --address="unix:path=$BUS_QNX" --nofork --print-address \
		> "$RUN/bus-qnx.log" 2>&1 < /dev/null &
	record bus-qnx $!
	nohup dbus-daemon --session --address="unix:path=$BUS_DEBIAN" --nofork --print-address \
		> "$RUN/bus-debian.log" 2>&1 < /dev/null &
	record bus-debian $!
	wait_for 10 "bus sockets" buses_up || die "bus sockets missing after 10 s"
}

start_guests() {
	nohup "$QEMU_BIN" -name qemu-qnx --enable-kvm -smp 4 -m 4G \
		-cpu host,host-phys-bits-limit=40 \
		-drive "file=$QNX_DISK,format=qcow2,if=ide,id=drv0" -snapshot \
		-kernel "$QNX_IFS" \
		-chardev "file,id=ser0,path=$RUN/qemu-qnx.log,append=on" -serial chardev:ser0 \
		-object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
		-object memory-backend-memfd,id=mem,size=4G,share=on -machine memory-backend=mem \
		-vga none -display "dbus,gl=on,addr=unix:path=$BUS_QNX" \
		-device virtio-gpu-gl-pci,max_outputs=1,edid=on,xres=1280,yres=720 \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:2223-:22 -device virtio-net-pci,netdev=net0 \
		>> "$RUN/qemu-qnx.log" 2>&1 < /dev/null &
	record qemu-qnx $!
	nohup "$QEMU_BIN" -name qemu-debian \
		-machine q35,accel=kvm,memory-backend=mem -cpu host -smp 2 -m 2G \
		-object memory-backend-memfd,id=mem,size=2G,share=on \
		-drive "file=$DEBIAN_DISK,if=virtio,format=qcow2" -snapshot \
		-vga none -display "dbus,gl=on,addr=unix:path=$BUS_DEBIAN" \
		-device virtio-vga-gl,edid=on,xres=1920,yres=1080 \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=net0 \
		> "$RUN/qemu-debian.log" 2>&1 < /dev/null &
	record qemu-debian $!
	wait_for 20 "org.qemu on the QNX bus" bus_has_qemu "$BUS_QNX" || die "QNX QEMU not on its bus after 20 s"
	wait_for 20 "org.qemu on the Debian bus" bus_has_qemu "$BUS_DEBIAN" || die "Debian QEMU not on its bus after 20 s"
}

start_proxies() {
	XDG_RUNTIME_DIR=$SHARED WAYLAND_DISPLAY=wayland-1 DBUS_SESSION_BUS_ADDRESS="unix:path=$BUS_QNX" \
		nohup "$PROXY_BIN" --vm-name qnx --wayland-socket wayland-1 --n-scanouts 1 \
		> "$RUN/proxy-qnx.log" 2>&1 < /dev/null &
	record proxy-qnx $!
	XDG_RUNTIME_DIR=$SHARED WAYLAND_DISPLAY=wayland-1 DBUS_SESSION_BUS_ADDRESS="unix:path=$BUS_DEBIAN" \
		nohup "$PROXY_BIN" --vm-name debian --wayland-socket wayland-1 --n-scanouts 1 \
		> "$RUN/proxy-debian.log" 2>&1 < /dev/null &
	record proxy-debian $!
	# The surfaces exist as soon as the proxies run, but the guests change their
	# display size while booting; place them only once they show their final size.
	wait_for 90 "QNX surface 1000 at 1280x720" surface_size_is 1000 1280 720 ||
		die "surface 1000 not at 1280x720 after 90 s"
	wait_for 120 "Debian surface 2000 at 1920x1080" surface_size_is 2000 1920 1080 ||
		die "surface 2000 not at 1920x1080 after 120 s"
}

place() {
	# layer, surface, source w h, destination x y w h
	for spec in "10 10 16 16 0 0 $WESTON_WIDTH $WESTON_HEIGHT" \
		"2000 2000 1920 1080 0 $ZY $ZW $ZH" \
		"1000 1000 1280 720 $ZW $ZY $ZW $ZH"; do
		# shellcheck disable=SC2086 # split the spec into its fields
		set -- $spec
		run_logged lmc create layer "$1" "$WESTON_WIDTH" "$WESTON_HEIGHT" || die "create layer $1"
		run_logged lmc add surface "$2" to layer "$1" || die "add surface $2"
		run_logged lmc set surface "$2" source region 0 0 "$3" "$4" || die "surface $2 source"
		run_logged lmc set surface "$2" destination region "$5" "$6" "$7" "$8" || die "surface $2 destination"
		run_logged lmc set layer "$1" source region 0 0 "$WESTON_WIDTH" "$WESTON_HEIGHT" || die "layer $1 source"
		run_logged lmc set layer "$1" destination region 0 0 "$WESTON_WIDTH" "$WESTON_HEIGHT" || die "layer $1 destination"
		run_logged lmc set layer "$1" visibility 1 || die "layer $1 visibility"
		run_logged lmc set surface "$2" visibility 1 || die "surface $2 visibility"
	done
	run_logged lmc set screen 0 render order 10,2000,1000 || die "render order"
	run_logged docker exec -e XDG_RUNTIME_DIR=/tmp/weston-runtime -e WAYLAND_DISPLAY=wayland-1 \
		"$CONTAINER" ivi-input-set all || die "ivi-input-set all"
	CONTAINER=$CONTAINER nohup "$SCRIPT_DIR/keyboard-focus-watch.sh" \
		> "$RUN/focus-watch.log" 2>&1 < /dev/null &
	record focus-watch $!
}

ready_check() {
	a1=$(frame_counter 1000); a2=$(frame_counter 2000)
	sleep 5
	b1=$(frame_counter 1000); b2=$(frame_counter 2000)
	log "frame counters over 5 s: surface 1000 $a1 -> $b1, surface 2000 $a2 -> $b2"
	[ "${b1:-0}" -gt "${a1:-0}" ] || die "surface 1000 received no frames in 5 s"
	[ "${b2:-0}" -gt "${a2:-0}" ] || die "surface 2000 received no frames in 5 s"
}

do_start() {
	mkdir -p "$VCOCKPIT_RUN_DIR" || { echo "vcockpit: cannot create $VCOCKPIT_RUN_DIR" >&2; exit 1; }
	# Refuse before creating a run directory, so a refused start changes nothing.
	if any_recorded_alive || container_exists || [ -e "$BUS_QNX" ] || [ -e "$BUS_DEBIAN" ]; then
		echo "vcockpit: a session (or its leftovers) is running; see $CURRENT, run 'vcockpit.sh stop' first" >&2
		exit 1
	fi
	RUN=$VCOCKPIT_RUN_DIR/$(date -u +%Y%m%dT%H%M%SZ)
	mkdir "$RUN" || { echo "vcockpit: cannot create $RUN" >&2; exit 1; }
	LOG=$RUN/launcher.log
	: > "$RUN/pids"
	ln -sfn "$RUN" "$CURRENT" || die "cannot link $CURRENT"
	log "start, run directory $RUN"
	check_prerequisites
	check_not_running
	compute_layout
	start_compositor
	start_buses
	start_guests
	start_proxies
	place
	ready_check
	do_status > "$RUN/status.txt" 2>&1 || die "status check after start"
	log "ready; run directory $RUN"
}

# ---------------------------------------------------------------- stop
signal_recorded() {	# <signal> <name>...
	sig=$1; shift
	for want in "$@"; do
		recorded | while read -r name pid; do
			if [ "$name" = "$want" ] && pid_is "$name" "$pid"; then
				log "stopping $name (pid $pid, SIG$sig)"
				kill -s "$sig" "$pid" || log "kill $pid failed"
			fi
		done
	done
}

wait_gone() {	# <seconds> <name>...
	limit=$(($1 * 2)); shift
	n=0
	while [ "$n" -lt "$limit" ]; do
		left=0
		for want in "$@"; do
			if recorded | while read -r name pid; do
				if [ "$name" = "$want" ] && pid_is "$name" "$pid"; then echo x; fi
			done | grep -q x; then left=1; fi
		done
		[ "$left" -eq 1 ] || return 0
		n=$((n + 1)); sleep 0.5
	done
	return 1
}

do_stop() {
	if [ -z "$RUN" ]; then
		[ -L "$CURRENT" ] || [ -d "$CURRENT" ] || { echo "vcockpit: no run recorded in $VCOCKPIT_RUN_DIR"; exit 0; }
		RUN=$(readlink -f "$CURRENT")
		LOG=$RUN/launcher.log
	fi
	log "stop"
	signal_recorded TERM focus-watch
	signal_recorded INT proxy-qnx proxy-debian
	if ! wait_gone 10 proxy-qnx proxy-debian; then
		signal_recorded TERM proxy-qnx proxy-debian
		wait_gone 5 proxy-qnx proxy-debian || log "a proxy did not exit"
	fi
	signal_recorded TERM qemu-qnx qemu-debian
	wait_gone 15 qemu-qnx qemu-debian || log "a QEMU did not exit"
	signal_recorded TERM bus-qnx bus-debian
	wait_gone 5 bus-qnx bus-debian || log "a bus daemon did not exit"
	if container_exists; then
		docker logs "$CONTAINER" > "$RUN/compositor.log" 2>&1 || log "docker logs failed"
		run_logged docker stop "$CONTAINER" || log "docker stop failed"
	fi
	if [ "${XHOST_GRANTED:-1}" = 1 ]; then
		run_logged xhost -local:docker || log "xhost -local:docker failed"
	fi
	rm -f "$BUS_QNX" "$BUS_DEBIAN"
	left=$(recorded | while read -r name pid; do
		if pid_is "$name" "$pid"; then printf '%s ' "$name"; fi
	done)
	if [ -n "$left" ] || container_exists; then
		log "still running: ${left}$(container_exists && echo "container $CONTAINER")"
		return 1
	fi
	log "stopped; run directory $RUN"
	return 0
}

# ---------------------------------------------------------------- status
do_status() {
	[ -f "$CURRENT/pids" ] || { echo "no run recorded in $VCOCKPIT_RUN_DIR"; return 1; }
	echo "run directory: $(readlink -f "$CURRENT")"
	all=0
	while read -r name pid; do
		[ -n "$name" ] || continue
		if pid_is "$name" "$pid"; then
			echo "  $name pid $pid: running"
		else
			echo "  $name pid $pid: not running"; all=1
		fi
	done <<EOF
$(recorded)
EOF
	if container_exists; then
		echo "  container $CONTAINER: running"
		for s in 10 1000 2000; do
			echo "  surface $s: frame counter $(frame_counter "$s")"
		done
	else
		echo "  container $CONTAINER: not running"; all=1
	fi
	return "$all"
}

# ---------------------------------------------------------------- main
case ${1:-} in
start)  do_start ;;
stop)   do_stop ;;
status) do_status ;;
-h|--help|help) usage ;;
*) usage >&2; exit 2 ;;
esac
