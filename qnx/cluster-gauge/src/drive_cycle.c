/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * cluster-gauge: scripted drive cycle.
 *
 * A 60 s loop of piecewise-linear speed segments:
 *    0- 5 s  idle                      0 km/h
 *    5-15 s  accelerate                0 -> 100 km/h
 *   15-25 s  cruise                    100 km/h
 *   25-32 s  left turn, left blinker   100 -> 60 km/h
 *   32-38 s  brake                     60 -> 20 km/h
 *   38-45 s  right turn, right blinker 20 -> 40 km/h
 *   45-60 s  night, high beam to 57 s  40 -> 80 km/h (to 52 s), 80 km/h,
 *                                      80 -> 0 km/h (57-60 s)
 * Bulb check: every telltale on for the first 2 s after start. The seat
 * belt telltale stays on until 5 s. Blinkers toggle at 1.5 Hz while their
 * segment is active. RPM follows speed within a gear band (cosmetic).
 *
 * Build with -DDRIVE_CYCLE_HOST_TEST and a host compiler for the host test
 * ("make host-test").
 */
#include <math.h>

#include "drive_cycle.h"

#define BULB_CHECK_S   2.0
#define SEATBELT_S     5.0
#define BLINK_HZ       1.5

struct segment {
	double t_start;
	double t_end;
	float v_start;
	float v_end;
	unsigned telltales;	/* TT_TURN_* blink; others steady */
};

static const struct segment segments[] = {
	{  0.0,  5.0,   0.0f,   0.0f, 0 },
	{  5.0, 15.0,   0.0f, 100.0f, 0 },
	{ 15.0, 25.0, 100.0f, 100.0f, 0 },
	{ 25.0, 32.0, 100.0f,  60.0f, TT_TURN_LEFT },
	{ 32.0, 38.0,  60.0f,  20.0f, 0 },
	{ 38.0, 45.0,  20.0f,  40.0f, TT_TURN_RIGHT },
	{ 45.0, 52.0,  40.0f,  80.0f, TT_HIGH_BEAM },
	{ 52.0, 57.0,  80.0f,  80.0f, TT_HIGH_BEAM },
	{ 57.0, 60.0,  80.0f,   0.0f, 0 },
};

#define N_SEGMENTS (sizeof(segments) / sizeof(segments[0]))

/* Gear bands: lower and upper speed of each gear, km/h. */
static const float gear_lo[] = { 0.0f, 20.0f, 40.0f, 60.0f, 80.0f };
static const float gear_hi[] = { 20.0f, 40.0f, 60.0f, 80.0f, 200.0f };

#define N_GEARS (sizeof(gear_lo) / sizeof(gear_lo[0]))

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float rpm_for_speed(float v)
{
	unsigned g;

	if (v <= 0.0f)
		return 800.0f;	/* idle */
	for (g = 0; g + 1 < N_GEARS && v >= gear_hi[g]; g++)
		;
	return 1200.0f + 2800.0f * (v - gear_lo[g]) / (gear_hi[g] - gear_lo[g]);
}

void drive_cycle_state(double t, struct gauge_state *s)
{
	double tl;
	const struct segment *seg = &segments[N_SEGMENTS - 1];
	unsigned i;
	float f;

	if (t < 0.0)
		t = 0.0;
	tl = fmod(t, DRIVE_CYCLE_PERIOD_S);
	for (i = 0; i < N_SEGMENTS; i++) {
		if (tl < segments[i].t_end) {
			seg = &segments[i];
			break;
		}
	}

	f = (float)((tl - seg->t_start) / (seg->t_end - seg->t_start));
	s->speed_kmh = clampf(seg->v_start + (seg->v_end - seg->v_start) * f,
			      0.0f, SPEED_MAX_KMH);
	s->rpm = clampf(rpm_for_speed(s->speed_kmh), 0.0f, RPM_MAX);

	s->telltales = seg->telltales & TT_HIGH_BEAM;
	if (seg->telltales & (TT_TURN_LEFT | TT_TURN_RIGHT)) {
		double phase = fmod((tl - seg->t_start) * BLINK_HZ, 1.0);

		if (phase < 0.5)
			s->telltales |= seg->telltales & (TT_TURN_LEFT | TT_TURN_RIGHT);
	}
	if (t < SEATBELT_S)
		s->telltales |= TT_SEATBELT;
	if (t < BULB_CHECK_S)
		s->telltales |= TT_ALL;
}

void drive_cycle_pause_init(struct drive_cycle_pause *p)
{
	p->paused = 0u;
	p->pause_started_s = 0.0;
	p->paused_total_s = 0.0;
}

int drive_cycle_pause_apply(struct drive_cycle_pause *p, struct drive_cycle_toggles req)
{
	if (req.count % 2u == 0u)
		return 0;
	if (p->paused) {
		p->paused_total_s += req.at_s - p->pause_started_s;
		p->paused = 0u;
	} else {
		p->pause_started_s = req.at_s;
		p->paused = 1u;
	}
	return 1;
}

double drive_cycle_pause_time(const struct drive_cycle_pause *p, double e)
{
	double t = (p->paused ? p->pause_started_s : e) - p->paused_total_s;

	return t < 0.0 ? 0.0 : t;
}

#ifdef DRIVE_CYCLE_HOST_TEST
#include <stdio.h>

/* Expected state at one time: speed band and telltale bits. */
struct expected {
	double t;
	float v_lo, v_hi;
	unsigned on, off;
};

static void expect(int *failures, int cond, const char *what, double t)
{
	if (!cond) {
		printf("FAIL t=%.2f: %s\n", t, what);
		(*failures)++;
	}
}

static void check_point(int *failures, struct expected e)
{
	struct gauge_state s;

	drive_cycle_state(e.t, &s);
	expect(failures, s.speed_kmh >= e.v_lo && s.speed_kmh <= e.v_hi, "speed band", e.t);
	expect(failures, (s.telltales & e.on) == e.on, "telltales expected on", e.t);
	expect(failures, (s.telltales & e.off) == 0, "telltales expected off", e.t);
	printf("t=%6.2f speed=%6.1f rpm=%6.0f telltales=0x%02x\n",
	       e.t, (double)s.speed_kmh, (double)s.rpm, s.telltales);
}

static const struct expected points[] = {
	/* Bulb check, seat belt, idle */
	{  0.0,   0.0f,   0.0f, TT_ALL, 0 },
	{  1.9,   0.0f,   0.0f, TT_ALL, 0 },
	{  3.0,   0.0f,   0.0f, TT_SEATBELT,
	   TT_CHECK_ENGINE | TT_TURN_LEFT | TT_TURN_RIGHT | TT_HIGH_BEAM },
	{  6.0,   0.0f,  20.0f, 0, TT_ALL },
	/* Accelerate, cruise */
	{ 10.0,  40.0f,  60.0f, 0, TT_ALL },
	{ 20.0, 100.0f, 100.0f, 0, TT_ALL },
	/* Left turn: blinker on in the first half period, off in the second */
	{ 25.1,  60.0f, 100.0f, TT_TURN_LEFT, TT_TURN_RIGHT | TT_HIGH_BEAM },
	{ 25.5,  60.0f, 100.0f, 0, TT_TURN_LEFT | TT_TURN_RIGHT },
	/* Brake */
	{ 35.0,  20.0f,  60.0f, 0, TT_ALL },
	/* Right turn */
	{ 38.1,  20.0f,  40.0f, TT_TURN_RIGHT, TT_TURN_LEFT },
	/* Night: high beam */
	{ 50.0,  40.0f,  80.0f, TT_HIGH_BEAM, TT_TURN_LEFT | TT_TURN_RIGHT },
	{ 55.0,  80.0f,  80.0f, TT_HIGH_BEAM, 0 },
	{ 58.5,   0.0f,  80.0f, 0, TT_ALL },
	/* Second loop: no bulb check, no seat belt */
	{ 61.0,   0.0f,   0.0f, 0, TT_ALL },
	{ 80.0, 100.0f, 100.0f, 0, TT_ALL },
};

int main(void)
{
	struct gauge_state a, b;
	int failures = 0;
	size_t i;
	double t;

	for (i = 0; i < sizeof(points) / sizeof(points[0]); i++)
		check_point(&failures, points[i]);

	/* Determinism and invariants over ten loops */
	for (t = 0.0; t < 600.0; t += 0.01) {
		drive_cycle_state(t, &a);
		drive_cycle_state(t, &b);
		expect(&failures, a.speed_kmh == b.speed_kmh && a.rpm == b.rpm &&
		       a.telltales == b.telltales, "deterministic", t);
		expect(&failures, a.speed_kmh >= 0.0f && a.speed_kmh <= SPEED_MAX_KMH,
		       "0 <= speed <= 200", t);
		expect(&failures, a.rpm >= 0.0f && a.rpm <= RPM_MAX, "0 <= rpm <= 7000", t);
		expect(&failures, (a.telltales & ~TT_ALL) == 0, "only defined bits", t);
		if (failures > 20)
			break;
	}

	/* Pause-time arithmetic */
	{
		struct drive_cycle_pause p;
		struct drive_cycle_toggles req;
		struct gauge_state held, now;
		int changed;

		/* Pause at 10 s: cycle time holds at 10 s, so the state holds */
		drive_cycle_pause_init(&p);
		expect(&failures, drive_cycle_pause_time(&p, 7.0) == 7.0, "running: t = e", 7.0);
		req = (struct drive_cycle_toggles){ 1u, 10.0 };
		changed = drive_cycle_pause_apply(&p, req);
		expect(&failures, changed == 1 && p.paused, "pause: one toggle pauses", 10.0);
		expect(&failures, drive_cycle_pause_time(&p, 10.0) == 10.0 &&
		       drive_cycle_pause_time(&p, 14.5) == 10.0, "pause: cycle time holds", 14.5);
		drive_cycle_state(drive_cycle_pause_time(&p, 10.0), &held);
		drive_cycle_state(drive_cycle_pause_time(&p, 14.5), &now);
		expect(&failures, held.speed_kmh == now.speed_kmh && held.rpm == now.rpm &&
		       held.telltales == now.telltales, "pause: state holds", 14.5);

		/* Resume at 15 s: continues from 10 s, no jump */
		req = (struct drive_cycle_toggles){ 1u, 15.0 };
		changed = drive_cycle_pause_apply(&p, req);
		expect(&failures, changed == 1 && !p.paused, "resume: one toggle resumes", 15.0);
		expect(&failures, drive_cycle_pause_time(&p, 15.0) == 10.0, "resume: no jump", 15.0);
		expect(&failures, drive_cycle_pause_time(&p, 16.0) == 11.0,
		       "resume: cycle advances after resume", 16.0);

		/* Two toggles in one frame: nothing changes; none: nothing; three: one */
		req = (struct drive_cycle_toggles){ 2u, 20.0 };
		changed = drive_cycle_pause_apply(&p, req);
		expect(&failures, changed == 0 && !p.paused &&
		       drive_cycle_pause_time(&p, 20.0) == 15.0, "double toggle: no change", 20.0);
		req = (struct drive_cycle_toggles){ 0u, 21.0 };
		changed = drive_cycle_pause_apply(&p, req);
		expect(&failures, changed == 0 && !p.paused, "no toggle: no change", 21.0);
		req = (struct drive_cycle_toggles){ 3u, 22.0 };
		changed = drive_cycle_pause_apply(&p, req);
		expect(&failures, changed == 1 && p.paused &&
		       drive_cycle_pause_time(&p, 30.0) == 17.0, "three toggles: one toggle", 30.0);

		/* Pause at t = 0: the bulb check holds; resume continues from 0 */
		drive_cycle_pause_init(&p);
		req = (struct drive_cycle_toggles){ 1u, 0.0 };
		changed = drive_cycle_pause_apply(&p, req);
		expect(&failures, changed == 1 && drive_cycle_pause_time(&p, 3.0) == 0.0,
		       "pause at 0: holds at 0", 3.0);
		drive_cycle_state(drive_cycle_pause_time(&p, 3.0), &now);
		expect(&failures, (now.telltales & TT_ALL) == TT_ALL, "pause at 0: bulb check held",
		       3.0);
		req = (struct drive_cycle_toggles){ 1u, 3.0 };
		changed = drive_cycle_pause_apply(&p, req);
		expect(&failures, changed == 1 && drive_cycle_pause_time(&p, 4.0) == 1.0,
		       "pause at 0: resume from 0", 4.0);
		printf("pause: pause, resume, double toggle, pause at t=0 checked\n");
	}

	printf("%s (%d failures)\n", failures ? "T1 FAIL" : "T1 PASS", failures);
	return failures ? 1 : 0;
}
#endif
