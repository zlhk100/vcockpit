/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * cluster-gauge: scripted drive cycle. The state shown at time t is a pure
 * function of t; no platform dependency.
 */
#ifndef CLUSTER_GAUGE_DRIVE_CYCLE_H
#define CLUSTER_GAUGE_DRIVE_CYCLE_H

/* Telltale bits */
#define TT_TURN_LEFT     (1u << 0)
#define TT_TURN_RIGHT    (1u << 1)
#define TT_SEATBELT      (1u << 2)
#define TT_HIGH_BEAM     (1u << 3)
#define TT_CHECK_ENGINE  (1u << 4)
#define TT_ALL           (TT_TURN_LEFT | TT_TURN_RIGHT | TT_SEATBELT | \
			  TT_HIGH_BEAM | TT_CHECK_ENGINE)

#define DRIVE_CYCLE_PERIOD_S  60.0
#define SPEED_MAX_KMH        200.0f
#define RPM_MAX             7000.0f

struct gauge_state {
	float speed_kmh;
	float rpm;
	unsigned telltales;
};

/* State at t seconds after start (t >= 0). The same t always gives the
 * same state. */
void drive_cycle_state(double t, struct gauge_state *s);

#endif
