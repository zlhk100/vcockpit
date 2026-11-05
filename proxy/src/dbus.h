/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
#ifndef DBUS_H
#define DBUS_H

#include "wayland.h"

/*
 * Connect to D-Bus session bus (DBUS_SESSION_BUS_ADDRESS),
 * find org.qemu, read ConsoleIDs from /org/qemu/Display1/VM.
 * Stores session_bus in ws->session_bus.
 * Updates ws->n_scanouts to minimum of configured vs QEMU-reported.
 * Returns 0 on success, -1 on error.
 */
int dbus_init(WaylandState *ws);

/*
 * For each scanout N:
 *   - Create socketpair()
 *   - Call RegisterListener on /org/qemu/Display1/Console_N
 *     passing one fd via GUnixFDList
 *   - Establish p2p GDBusConnection on our end of the socketpair
 * Stores p2p_conn and p2p_fd in ws->scanouts[N].
 * Returns 0 on success, -1 on error.
 */
int dbus_register_listeners(WaylandState *ws);

/*
 * Close all p2p connections, close session bus, free resources.
 * Safe to call with NULL or partially initialised ws.
 * Called automatically by wayland_destroy().
 */
void dbus_destroy(WaylandState *ws);

#endif /* DBUS_H */
