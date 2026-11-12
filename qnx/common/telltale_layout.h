/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * Telltale layout shared by cluster-gauge (which draws the telltales) and
 * telltale-monitor (which checks them on the composed display): for each
 * telltale its state bit, its box and sample points in the 1280x720
 * layout, and the colours it is drawn in when on and when off. One source
 * of truth for where each telltale is and how it looks.
 *
 * Layout coordinates are pixels, origin top left, y down. A program
 * drawing into, or reading from, a w x h surface scales the layout
 * uniformly by min(w / TELLTALE_LAYOUT_W, h / TELLTALE_LAYOUT_H) and
 * centres it.
 *
 * Everything here is a macro or a static const table: read-only data.
 */
#ifndef VCOCKPIT_TELLTALE_LAYOUT_H
#define VCOCKPIT_TELLTALE_LAYOUT_H

#include <stddef.h>

/* Telltale state bits, as commanded by the gauge's drive cycle and as
 * published to the monitor. Bits outside TT_ALL are never set. */
#define TT_TURN_LEFT     (1u << 0)
#define TT_TURN_RIGHT    (1u << 1)
#define TT_SEATBELT      (1u << 2)
#define TT_HIGH_BEAM     (1u << 3)
#define TT_CHECK_ENGINE  (1u << 4)
#define TT_ALL           (TT_TURN_LEFT | TT_TURN_RIGHT | TT_SEATBELT | \
			  TT_HIGH_BEAM | TT_CHECK_ENGINE)

#define TELLTALE_LAYOUT_W 1280.0f
#define TELLTALE_LAYOUT_H 720.0f

#define TELLTALE_COUNT 5u
#define TELLTALE_MAX_SAMPLES 3u
#define TELLTALE_NAME_MAX 16u

/* An 8-bit-per-channel colour, opaque. */
struct telltale_rgb {
	unsigned char r, g, b;
};

/* A point in the layout. */
struct telltale_point {
	float x, y;
};

/* A box in the layout: top-left corner and size. */
struct telltale_box {
	float x, y, w, h;
};

struct telltale_def {
	char name[TELLTALE_NAME_MAX];	/* used in logs and on command lines */
	unsigned bit;			/* one of the TT_* bits */
	struct telltale_box box;	/* the shape is drawn centred in this box */
	struct telltale_rgb on;		/* colour of the shape when on */
	size_t n_samples;		/* entries used in samples[] */
	/* Points inside the shape, clear of its edges and of anything drawn
	 * over it, where the displayed colour is checked. */
	struct telltale_point samples[TELLTALE_MAX_SAMPLES];
};

/* Colour of every telltale shape when off. */
static const struct telltale_rgb telltale_off = { 25, 28, 33 };

/* The telltale row, left to right. */
static const struct telltale_def telltale_table[TELLTALE_COUNT] = {
	{ "turn_left", TT_TURN_LEFT, { 60.0f, 30.0f, 80.0f, 80.0f }, { 38, 217, 64 },
	  2, { { 117.0f, 70.0f }, { 86.0f, 70.0f } } },
	{ "seatbelt", TT_SEATBELT, { 430.0f, 30.0f, 80.0f, 80.0f }, { 242, 38, 31 },
	  3, { { 470.0f, 48.0f }, { 478.0f, 70.0f }, { 462.0f, 92.0f } } },
	{ "high_beam", TT_HIGH_BEAM, { 600.0f, 30.0f, 80.0f, 80.0f }, { 51, 115, 255 },
	  3, { { 654.0f, 70.0f }, { 650.0f, 60.0f }, { 620.0f, 65.0f } } },
	{ "check_engine", TT_CHECK_ENGINE, { 770.0f, 30.0f, 80.0f, 80.0f }, { 255, 166, 13 },
	  3, { { 808.0f, 71.0f }, { 795.0f, 60.0f }, { 820.0f, 80.0f } } },
	{ "turn_right", TT_TURN_RIGHT, { 1140.0f, 30.0f, 80.0f, 80.0f }, { 38, 217, 64 },
	  2, { { 1163.0f, 70.0f }, { 1194.0f, 70.0f } } },
};

#endif
