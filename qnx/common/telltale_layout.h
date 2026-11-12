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
#include <stdint.h>

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

/* ---- Commanded telltale state, published by the gauge ----
 *
 * POSIX shared memory object TELLTALE_STATE_SHM_NAME, TELLTALE_STATE_SHM_SIZE
 * bytes, created by the gauge (read-write) and mapped read-only by the
 * monitor. It holds one _Atomic uint64_t word at offset 0, stored by the
 * gauge once per frame and loaded by the monitor, each in one atomic
 * operation:
 *
 *   bits 63..32  seq   frame sequence number, +1 per published frame,
 *                      never 0 once published (0: nothing published yet)
 *   bits 31..16  t_ms  time since gauge start, ms, modulo 65536
 *   bits 15..0   telltale bits (TT_*; bits outside TT_ALL are 0)
 */
#define TELLTALE_STATE_SHM_NAME "/vcockpit-telltale-state"
#define TELLTALE_STATE_SHM_SIZE 4096u

/* The word packed from seq, t_ms (taken modulo 65536) and bits (only
 * TT_ALL bits are kept). Cannot fail. */
static inline uint64_t telltale_state_pack(uint32_t seq, uint32_t t_ms, unsigned bits)
{
	return ((uint64_t)seq << 32) | ((uint64_t)(t_ms & 0xffffu) << 16) |
	       (uint64_t)(bits & TT_ALL);
}

/* The seq field of word w. Cannot fail. */
static inline uint32_t telltale_state_seq(uint64_t w)
{
	return (uint32_t)(w >> 32);
}

/* The t_ms field of word w. Cannot fail. */
static inline uint32_t telltale_state_t_ms(uint64_t w)
{
	return (uint32_t)((w >> 16) & 0xffffu);
}

/* The telltale bits field of word w, all 16 bits (a reader checks that
 * none outside TT_ALL is set). Cannot fail. */
static inline unsigned telltale_state_bits(uint64_t w)
{
	return (unsigned)(w & 0xffffu);
}

#endif
