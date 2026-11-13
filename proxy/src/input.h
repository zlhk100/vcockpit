/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
#ifndef INPUT_H
#define INPUT_H

#include "wayland.h"

/*
 * Bind the compositor's wl_seat (version min(version, 5)) and listen for
 * its capabilities; pointer and keyboard objects follow the capabilities.
 * Called from the registry listener for the "wl_seat" global.
 * Precondition: ws and registry are valid. On a bind failure the seat stays
 * NULL and no input is forwarded.
 */
void input_bind_seat(WaylandState *ws, struct wl_registry *registry, uint32_t name,
		     uint32_t version);

/*
 * Draw the pointer cursor (a 16x24 arrow, hotspot 0,0) into a wl_shm buffer
 * attached to a cursor surface; the proxy sets it on each pointer enter on its
 * own surfaces. Precondition: ws->compositor and ws->shm are bound.
 * Returns 0, or -1 after printing the failing call; on failure no cursor is
 * set and everything created here is released by wayland_destroy().
 */
int input_cursor_init(WaylandState *ws);

/*
 * For each scanout: read QEMU's Mouse.IsAbsolute on Console_<i> (5 s timeout;
 * on failure assume relative and log) and subscribe to PropertiesChanged so
 * that is_absolute follows QEMU's mouse mode.
 * Precondition: ws->session_bus is connected (dbus_init succeeded).
 */
void input_dbus_init(WaylandState *ws);

/*
 * Release every button and key the proxy holds down in the guest, unsubscribe
 * from PropertiesChanged, destroy the keyboard, pointer and seat, flush the
 * session bus and print the counters line:
 *   Input: <m> motion, <b> button, <k> key calls; <u> unmapped keys; <g> gated keys;
 *   <e> call errors
 * (gated keys: key presses not sent because the pointer was not on a proxy surface)
 * Call before dbus_destroy(). Safe to call when no seat was bound.
 */
void input_shutdown(WaylandState *ws);

#endif /* INPUT_H */
