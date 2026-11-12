/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * cluster-gauge: scripted drive cycle. The state shown at time t is a pure
 * function of t; no platform dependency.
 */
#ifndef CLUSTER_GAUGE_DRIVE_CYCLE_H
#define CLUSTER_GAUGE_DRIVE_CYCLE_H

/* Telltale bits TT_* (shared with telltale-monitor) */
#include "telltale_layout.h"

#define DRIVE_CYCLE_PERIOD_S  60.0
#define SPEED_MAX_KMH        200.0f
#define RPM_MAX             7000.0f

struct gauge_state {
	float speed_kmh;
	float rpm;
	unsigned telltales;
	unsigned paused;	/* 1: drive cycle paused (pause glyph shown); set by the
				 * frame loop, not by drive_cycle_state() */
};

/* State at t seconds after start (t >= 0). The same t always gives the
 * same state. Sets speed_kmh, rpm and telltales; does not touch paused.
 * Cannot fail. */
void drive_cycle_state(double t, struct gauge_state *s);

/* Pause control of the drive-cycle clock. Its owner (the frame loop) keeps
 * it as an automatic object and passes it to the functions below; nothing
 * else writes it. Times are seconds since the gauge started. */
struct drive_cycle_pause {
	unsigned paused;	/* 0: running, 1: paused */
	double pause_started_s;	/* start of the current pause */
	double paused_total_s;	/* sum of the completed pauses */
};

/* Running, nothing paused yet. Cannot fail. */
void drive_cycle_pause_init(struct drive_cycle_pause *p);

/* Toggle requests seen in one frame. */
struct drive_cycle_toggles {
	unsigned count;		/* left-button presses and Space key-downs */
	double at_s;		/* elapsed time of the frame (>= 0, not decreasing) */
};

/* Apply one frame's toggle requests: an odd count toggles pause, an even
 * count changes nothing. On resume the completed pause is added to
 * paused_total_s, so the cycle continues from where it stopped. Returns 1
 * if the state changed, else 0. Cannot fail. */
int drive_cycle_pause_apply(struct drive_cycle_pause *p, struct drive_cycle_toggles req);

/* The drive-cycle time at elapsed time e: e minus the time spent paused;
 * held at the pause start while paused. Never below 0. Cannot fail. */
double drive_cycle_pause_time(const struct drive_cycle_pause *p, double e);

#endif
