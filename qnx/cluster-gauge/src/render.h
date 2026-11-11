/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * cluster-gauge: renderer. Draws a gauge_state with OpenGL ES 2 into the
 * current context; knows nothing of Screen or EGL.
 */
#ifndef CLUSTER_GAUGE_RENDER_H
#define CLUSTER_GAUGE_RENDER_H

#include "drive_cycle.h"

/* Compile the shader program for a width x height viewport. On failure,
 * print the failing call and return -1. */
int render_init(int width, int height);

/* Draw one frame for state s. */
void render_frame(const struct gauge_state *s);

void render_shutdown(void);

#endif
