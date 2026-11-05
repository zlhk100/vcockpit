/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
#ifndef LISTENER_H
#define LISTENER_H

#include "wayland.h"

/*
 * Register the org.qemu.Display1.Listener object on the p2p connection.
 * Required to prevent QEMU from throwing UnknownMethod errors.
 */
void listener_setup(WaylandState *ws, int scanout_index);

#endif /* LISTENER_H */
