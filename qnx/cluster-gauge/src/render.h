/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * cluster-gauge: renderer. Draws a gauge_state with OpenGL ES 2 into the
 * current context; knows nothing of Screen or EGL.
 */
#ifndef CLUSTER_GAUGE_RENDER_H
#define CLUSTER_GAUGE_RENDER_H

#include <stddef.h>

#include <GLES2/gl2.h>

#include "drive_cycle.h"

/* Vertex buffer capacity: 8192 vertices of two floats, 64 KB. */
#define RENDER_MAX_VERTS 8192

/* Size of the viewport the layout is drawn into, in pixels. */
struct render_viewport {
	int width;
	int height;
};

/* Renderer state. Its owner creates one, passes it to render_init() and
 * then to every other render_ call; nothing else writes it. */
struct renderer {
	GLuint program;
	GLint loc_pos;
	GLint loc_color;
	float scale_x;		/* layout pixels to normalised device coordinates */
	float scale_y;
	float offset_x;
	float offset_y;
	size_t nverts;		/* vertices in verts[] not yet drawn */
	size_t dropped;		/* vertices dropped since the frame began */
	GLfloat verts[RENDER_MAX_VERTS * 2];
};

/* Compile the shader program and set the layout transform for viewport vp:
 * the 1280x720 layout is scaled uniformly by min(w/1280, h/720) and
 * centred. Precondition: an OpenGL ES 2 context is current; r is not
 * initialised. Returns 0, or -1 after printing the failing call; on
 * failure nothing is left to release. */
int render_init(struct renderer *r, struct render_viewport vp);

/* Draw one frame for state s. Precondition: render_init(r) succeeded.
 * Returns the number of vertices dropped because the vertex buffer was
 * full (0 when the frame is complete). */
size_t render_frame(struct renderer *r, const struct gauge_state *s);

/* Release what render_init() created. Safe to call once after a
 * successful render_init(). Cannot fail. */
void render_shutdown(struct renderer *r);

#endif
