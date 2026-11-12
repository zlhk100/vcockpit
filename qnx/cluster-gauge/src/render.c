/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * cluster-gauge: renderer.
 *
 * One shader program: per-vertex position (normalised device coordinates),
 * uniform colour. All geometry is built on the CPU each frame as triangle
 * lists in a 1280x720 layout, scaled uniformly to the viewport and centred.
 * Digits are seven-segment quads, so no font is needed. All shapes are
 * drawn here.
 *
 * All state is in the struct renderer the caller passes; the only objects
 * with static storage are the constant tables below.
 */
#include <math.h>
#include <stdio.h>

#include <GLES2/gl2.h>

#include "render.h"

#define LAYOUT_W 1280.0f
#define LAYOUT_H 720.0f
#define PI_F 3.14159265f
#define RING_STEP_DEG 3.0f
#define RING_MAX_STEPS 121	/* 360 degrees in 3-degree steps, plus one */

/* Speedometer */
#define SPD_CX        640.0f
#define SPD_CY        380.0f
#define SPD_R_OUT     270.0f
#define SPD_R_IN      250.0f
#define SPD_A_START   225.0f	/* degrees, 0 km/h */
#define SPD_SWEEP     270.0f	/* degrees, clockwise to SPEED_MAX_KMH */
#define SPD_TICKS     20	/* one tick per 10 km/h */

/* RPM bar */
#define RPM_X         240.0f
#define RPM_Y         660.0f
#define RPM_W         800.0f
#define RPM_H          28.0f
#define RPM_REDLINE  6000.0f
#define RPM_TICKS     7

/* Pause glyph: two vertical bars inside the layout box (610, 630, 60, 60),
 * clear of every telltale box (y 30-110) and kept above the RPM bar
 * (y >= 660): bars 14 x 24 px at y 632-656, 10 px apart, centred on x 640. */
#define PAUSE_BAR_W   14.0f
#define PAUSE_BAR_H   24.0f
#define PAUSE_GAP     10.0f
#define PAUSE_TOP    632.0f
#define PAUSE_CX     640.0f

struct rgba { float r, g, b, a; };
struct vec2 { float x, y; };
struct box { float x, y, w, h; };
struct seg { struct vec2 a, b; };

/* Ring sector between radii r_in and r_out from angle a0 to a1 (degrees,
 * counter-clockwise, 0 = +x). r_in = 0 gives a disc sector. */
struct arc { float r_in, r_out, a0, a1; };

/* A seven-segment cell: its box and the segment thickness. */
struct seven_seg { struct box box; float t; };

/* Colours of everything but the telltales; those are in telltale_layout.h. */
enum colour {
	COL_TRACK, COL_ARC, COL_TICK, COL_NEEDLE, COL_HUB, COL_SEG_ON, COL_SEG_OFF,
	COL_RPM, COL_RED, COL_PAUSE,
	COL_COUNT
};

static const struct rgba palette[COL_COUNT] = {
	[COL_TRACK]   = { 0.16f, 0.18f, 0.22f, 1.0f },
	[COL_ARC]     = { 0.10f, 0.75f, 0.95f, 1.0f },
	[COL_TICK]    = { 0.80f, 0.84f, 0.90f, 1.0f },
	[COL_NEEDLE]  = { 1.00f, 0.35f, 0.10f, 1.0f },
	[COL_HUB]     = { 0.30f, 0.32f, 0.36f, 1.0f },
	[COL_SEG_ON]  = { 0.92f, 0.95f, 1.00f, 1.0f },
	[COL_SEG_OFF] = { 0.08f, 0.09f, 0.11f, 1.0f },
	[COL_RPM]     = { 0.95f, 0.80f, 0.20f, 1.0f },
	[COL_RED]     = { 0.45f, 0.06f, 0.06f, 1.0f },
	[COL_PAUSE]   = { 0.902f, 0.902f, 0.902f, 1.0f },	/* (230,230,230) */
};

/* Seven-segment digits, bits a..g = 0..6 */
static const unsigned char seg_digits[10] = {
	0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};

/* ---- geometry helpers (layout pixels, y down) ---- */

/* Append one vertex. When the buffer is full the vertex is dropped and
 * counted; render_frame() returns the count. */
static void vtx(struct renderer *r, struct vec2 p)
{
	if (r->nverts >= RENDER_MAX_VERTS) {
		r->dropped++;
		return;
	}
	r->verts[r->nverts * 2u] = r->offset_x + p.x * r->scale_x;
	r->verts[r->nverts * 2u + 1u] = r->offset_y - p.y * r->scale_y;
	r->nverts++;
}

static void tri(struct renderer *r, const struct vec2 v[3])
{
	vtx(r, v[0]);
	vtx(r, v[1]);
	vtx(r, v[2]);
}

static void quad(struct renderer *r, const struct vec2 v[4])
{
	const struct vec2 t0[3] = { v[0], v[1], v[2] };
	const struct vec2 t1[3] = { v[0], v[2], v[3] };

	tri(r, t0);
	tri(r, t1);
}

static void rect(struct renderer *r, struct box b)
{
	const struct vec2 v[4] = {
		{ b.x, b.y }, { b.x + b.w, b.y }, { b.x + b.w, b.y + b.h }, { b.x, b.y + b.h }
	};

	quad(r, v);
}

static void ring(struct renderer *r, struct vec2 c, struct arc a)
{
	int n = (int)(fabsf(a.a1 - a.a0) / RING_STEP_DEG) + 1;
	int i;

	if (n > RING_MAX_STEPS)
		n = RING_MAX_STEPS;
	for (i = 0; i < n; i++) {
		float b0 = (a.a0 + (a.a1 - a.a0) * (float)i / (float)n) * PI_F / 180.0f;
		float b1 = (a.a0 + (a.a1 - a.a0) * (float)(i + 1) / (float)n) * PI_F / 180.0f;
		float c0 = cosf(b0), s0 = sinf(b0), c1 = cosf(b1), s1 = sinf(b1);
		const struct vec2 v[4] = {
			{ c.x + a.r_in * c0, c.y - a.r_in * s0 },
			{ c.x + a.r_out * c0, c.y - a.r_out * s0 },
			{ c.x + a.r_out * c1, c.y - a.r_out * s1 },
			{ c.x + a.r_in * c1, c.y - a.r_in * s1 },
		};

		quad(r, v);
	}
}

static void disc(struct renderer *r, struct vec2 c, float radius)
{
	const struct arc a = { 0.0f, radius, 0.0f, 360.0f };

	ring(r, c, a);
}

/* Line of width w along s. */
static void line(struct renderer *r, struct seg s, float w)
{
	float dx = s.b.x - s.a.x, dy = s.b.y - s.a.y;
	float len = sqrtf(dx * dx + dy * dy);
	float nx, ny;

	if (len <= 0.0f)
		return;
	nx = -dy / len * w * 0.5f;
	ny = dx / len * w * 0.5f;
	{
		const struct vec2 v[4] = {
			{ s.a.x + nx, s.a.y + ny }, { s.b.x + nx, s.b.y + ny },
			{ s.b.x - nx, s.b.y - ny }, { s.a.x - nx, s.a.y - ny },
		};

		quad(r, v);
	}
}

/* Draw the vertices collected since the last draw in colour p. */
static void draw_rgba(struct renderer *r, const struct rgba *p)
{
	if (r->nverts == 0)
		return;
	glUniform4f(r->loc_color, p->r, p->g, p->b, p->a);
	glVertexAttribPointer((GLuint)r->loc_pos, 2, GL_FLOAT, GL_FALSE, 0, r->verts);
	glDrawArrays(GL_TRIANGLES, 0, (GLsizei)r->nverts);
	r->nverts = 0;
}

/* Draw in palette colour c. */
static void draw(struct renderer *r, enum colour c)
{
	draw_rgba(r, &palette[c]);
}

/* Draw in a telltale colour (8 bits per channel, opaque). */
static void draw_telltale(struct renderer *r, struct telltale_rgb c)
{
	const struct rgba p = {
		(float)c.r / 255.0f, (float)c.g / 255.0f, (float)c.b / 255.0f, 1.0f
	};

	draw_rgba(r, &p);
}

/* ---- seven-segment digits ---- */

static void segments(struct renderer *r, const struct seven_seg *g, unsigned mask)
{
	float x = g->box.x, y = g->box.y, w = g->box.w, h = g->box.h, t = g->t;
	float h2 = h * 0.5f;
	float vl = h2 - 1.5f * t;

	if (mask & 0x01) rect(r, (struct box){ x + t, y, w - 2.0f * t, t });		/* a */
	if (mask & 0x02) rect(r, (struct box){ x + w - t, y + t, t, vl });		/* b */
	if (mask & 0x04) rect(r, (struct box){ x + w - t, y + h2 + 0.5f * t, t, vl }); /* c */
	if (mask & 0x08) rect(r, (struct box){ x + t, y + h - t, w - 2.0f * t, t });	/* d */
	if (mask & 0x10) rect(r, (struct box){ x, y + h2 + 0.5f * t, t, vl });	/* e */
	if (mask & 0x20) rect(r, (struct box){ x, y + t, t, vl });			/* f */
	if (mask & 0x40) rect(r, (struct box){ x + t, y + h2 - 0.5f * t, w - 2.0f * t, t }); /* g */
}

/* Three-digit number centred at pos.x, top at pos.y; leading zeros blank. */
static void number3(struct renderer *r, struct vec2 pos, int value)
{
	const float w = 52.0f, h = 92.0f, t = 11.0f, gap = 14.0f;
	float x0 = pos.x - (3.0f * w + 2.0f * gap) * 0.5f;
	unsigned masks[3];
	size_t i;

	if (value < 0)
		value = 0;
	if (value > 999)
		value = 999;
	masks[0] = value >= 100 ? seg_digits[value / 100] : 0u;
	masks[1] = value >= 10 ? seg_digits[(value / 10) % 10] : 0u;
	masks[2] = seg_digits[value % 10];

	for (i = 0; i < 3; i++) {
		const struct seven_seg g = { { x0 + (float)i * (w + gap), pos.y, w, h }, t };

		segments(r, &g, 0x7f);
	}
	draw(r, COL_SEG_OFF);
	for (i = 0; i < 3; i++) {
		const struct seven_seg g = { { x0 + (float)i * (w + gap), pos.y, w, h }, t };

		segments(r, &g, masks[i]);
	}
	draw(r, COL_SEG_ON);
}

/* ---- telltale shapes ---- */

/* Turn arrow centred at c; dir -1 points left, +1 right. */
static void arrow(struct renderer *r, struct vec2 c, int dir)
{
	float s = (float)dir;
	const struct vec2 head[3] = {
		{ c.x + s * 34.0f, c.y },
		{ c.x + s * 4.0f, c.y - 28.0f },
		{ c.x + s * 4.0f, c.y + 28.0f },
	};

	tri(r, head);
	rect(r, (struct box){ dir < 0 ? c.x + 4.0f : c.x - 30.0f, c.y - 10.0f, 26.0f, 20.0f });
}

static void seatbelt(struct renderer *r, struct vec2 c)
{
	const struct vec2 body[4] = {
		{ c.x - 14.0f, c.y - 8.0f }, { c.x + 14.0f, c.y - 8.0f },
		{ c.x + 18.0f, c.y + 30.0f }, { c.x - 18.0f, c.y + 30.0f },
	};

	disc(r, (struct vec2){ c.x, c.y - 22.0f }, 9.0f);
	quad(r, body);
}

static void seatbelt_strap(struct renderer *r, struct vec2 c)
{
	line(r, (struct seg){ { c.x - 16.0f, c.y - 10.0f }, { c.x + 16.0f, c.y + 26.0f } }, 6.0f);
}

static void high_beam(struct renderer *r, struct vec2 c)
{
	int i;

	ring(r, (struct vec2){ c.x + 4.0f, c.y }, (struct arc){ 0.0f, 22.0f, -90.0f, 90.0f });
	rect(r, (struct box){ c.x + 1.0f, c.y - 22.0f, 4.0f, 44.0f });
	for (i = 0; i < 4; i++)
		rect(r, (struct box){ c.x - 34.0f, c.y - 18.0f + (float)i * 11.0f, 28.0f, 5.0f });
}

static void engine(struct renderer *r, struct vec2 c)
{
	rect(r, (struct box){ c.x - 22.0f, c.y - 14.0f, 40.0f, 30.0f });	/* block */
	rect(r, (struct box){ c.x - 12.0f, c.y - 24.0f, 20.0f, 6.0f });	/* filler cap */
	rect(r, (struct box){ c.x - 4.0f, c.y - 20.0f, 4.0f, 6.0f });
	rect(r, (struct box){ c.x + 18.0f, c.y - 6.0f, 10.0f, 14.0f });	/* right side */
	rect(r, (struct box){ c.x - 32.0f, c.y - 4.0f, 10.0f, 10.0f });	/* left side */
}

static void telltales(struct renderer *r, unsigned bits)
{
	size_t i;

	for (i = 0; i < TELLTALE_COUNT; i++) {
		const struct telltale_def *tt = &telltale_table[i];
		const struct vec2 c = { tt->box.x + tt->box.w * 0.5f, tt->box.y + tt->box.h * 0.5f };

		switch (tt->bit) {
		case TT_TURN_LEFT:
			arrow(r, c, -1);
			break;
		case TT_TURN_RIGHT:
			arrow(r, c, 1);
			break;
		case TT_SEATBELT:
			seatbelt(r, c);
			break;
		case TT_HIGH_BEAM:
			high_beam(r, c);
			break;
		default:
			engine(r, c);
			break;
		}
		draw_telltale(r, (bits & tt->bit) ? tt->on : telltale_off);
		if (tt->bit == TT_SEATBELT) {
			seatbelt_strap(r, c);
			draw(r, COL_SEG_OFF);
		}
	}
}

/* ---- gauges ---- */

static float speed_angle(float v)
{
	return SPD_A_START - SPD_SWEEP * (v / SPEED_MAX_KMH);
}

static void speedometer(struct renderer *r, float v)
{
	const struct vec2 c = { SPD_CX, SPD_CY };
	float a = speed_angle(v) * PI_F / 180.0f;
	int k;

	ring(r, c, (struct arc){ SPD_R_IN, SPD_R_OUT, SPD_A_START - SPD_SWEEP, SPD_A_START });
	draw(r, COL_TRACK);
	if (v > 0.0f) {
		ring(r, c, (struct arc){ SPD_R_IN, SPD_R_OUT, speed_angle(v), SPD_A_START });
		draw(r, COL_ARC);
	}

	for (k = 0; k <= SPD_TICKS; k++) {
		float b = speed_angle((float)k * 10.0f) * PI_F / 180.0f;
		float r0 = (k % 2 == 0) ? SPD_R_IN - 34.0f : SPD_R_IN - 18.0f;
		float r1 = SPD_R_IN - 6.0f;
		const struct seg s = {
			{ SPD_CX + r0 * cosf(b), SPD_CY - r0 * sinf(b) },
			{ SPD_CX + r1 * cosf(b), SPD_CY - r1 * sinf(b) },
		};

		line(r, s, (k % 2 == 0) ? 5.0f : 3.0f);
	}
	draw(r, COL_TICK);

	{
		float rn = SPD_R_IN - 10.0f;
		float px = -sinf(a) * 9.0f, py = -cosf(a) * 9.0f;
		const struct vec2 needle[3] = {
			{ SPD_CX + rn * cosf(a), SPD_CY - rn * sinf(a) },
			{ SPD_CX + px, SPD_CY + py },
			{ SPD_CX - px, SPD_CY - py },
		};

		tri(r, needle);
		draw(r, COL_NEEDLE);
	}
	disc(r, c, 22.0f);
	draw(r, COL_HUB);
}

static void rpm_bar(struct renderer *r, float rpm)
{
	float f = rpm / RPM_MAX;
	float red_x = RPM_X + RPM_W * (RPM_REDLINE / RPM_MAX);
	int k;

	if (f < 0.0f)
		f = 0.0f;
	if (f > 1.0f)
		f = 1.0f;
	rect(r, (struct box){ RPM_X, RPM_Y, red_x - RPM_X, RPM_H });
	draw(r, COL_TRACK);
	rect(r, (struct box){ red_x, RPM_Y, RPM_X + RPM_W - red_x, RPM_H });
	draw(r, COL_RED);
	rect(r, (struct box){ RPM_X, RPM_Y, RPM_W * f, RPM_H });
	draw(r, COL_RPM);
	for (k = 0; k <= RPM_TICKS; k++)
		rect(r, (struct box){ RPM_X + RPM_W * (float)k / (float)RPM_TICKS - 1.5f,
				      RPM_Y - 12.0f, 3.0f, 10.0f });
	draw(r, COL_TICK);
}

/* ---- public ---- */

static GLuint compile(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	GLint ok = 0;

	if (sh == 0) {
		/* stderr is the only error channel; if it fails, nothing else can report */
		(void)fprintf(stderr, "cluster-gauge: glCreateShader failed: GL error 0x%04x\n",
			      (unsigned)glGetError());
		return 0;
	}
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
		/* stderr is the only error channel; if it fails, nothing else can report */
		(void)fprintf(stderr, "cluster-gauge: glCompileShader failed: %s\n", log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

int render_init(struct renderer *r, struct render_viewport vp)
{
	static const char vs_src[] =
		"attribute vec2 a_pos;\n"
		"void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
	static const char fs_src[] =
		"precision mediump float;\n"
		"uniform vec4 u_color;\n"
		"void main() { gl_FragColor = u_color; }\n";
	float w = (float)vp.width, h = (float)vp.height;
	float s = fminf(w / LAYOUT_W, h / LAYOUT_H);
	GLuint vs, fs;
	GLint ok = 0;

	r->program = 0;
	r->loc_pos = -1;
	r->loc_color = -1;
	r->nverts = 0;
	r->dropped = 0;

	/* Layout pixels to normalised device coordinates: scale uniformly by s
	 * and centre the scaled layout in the viewport. */
	r->scale_x = 2.0f * s / w;
	r->scale_y = 2.0f * s / h;
	r->offset_x = (w - LAYOUT_W * s) / w - 1.0f;
	r->offset_y = 1.0f - (h - LAYOUT_H * s) / h;

	vs = compile(GL_VERTEX_SHADER, vs_src);
	if (vs == 0)
		return -1;
	fs = compile(GL_FRAGMENT_SHADER, fs_src);
	if (fs == 0) {
		glDeleteShader(vs);
		return -1;
	}
	r->program = glCreateProgram();
	glAttachShader(r->program, vs);
	glAttachShader(r->program, fs);
	glLinkProgram(r->program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	glGetProgramiv(r->program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetProgramInfoLog(r->program, (GLsizei)sizeof(log), NULL, log);
		/* stderr is the only error channel; if it fails, nothing else can report */
		(void)fprintf(stderr, "cluster-gauge: glLinkProgram failed: %s\n", log);
		glDeleteProgram(r->program);
		r->program = 0;
		return -1;
	}
	r->loc_pos = glGetAttribLocation(r->program, "a_pos");
	r->loc_color = glGetUniformLocation(r->program, "u_color");
	if (r->loc_pos < 0 || r->loc_color < 0) {
		/* stderr is the only error channel; if it fails, nothing else can report */
		(void)fprintf(stderr,
			      "cluster-gauge: glGetAttribLocation/glGetUniformLocation failed\n");
		glDeleteProgram(r->program);
		r->program = 0;
		return -1;
	}
	glUseProgram(r->program);
	glEnableVertexAttribArray((GLuint)r->loc_pos);
	return 0;
}

static void pause_glyph(struct renderer *r)
{
	const float half_gap = PAUSE_GAP * 0.5f;

	rect(r, (struct box){ PAUSE_CX - half_gap - PAUSE_BAR_W, PAUSE_TOP,
			      PAUSE_BAR_W, PAUSE_BAR_H });
	rect(r, (struct box){ PAUSE_CX + half_gap, PAUSE_TOP, PAUSE_BAR_W, PAUSE_BAR_H });
	draw(r, COL_PAUSE);
}

size_t render_frame(struct renderer *r, const struct gauge_state *s)
{
	r->nverts = 0;
	r->dropped = 0;

	glClearColor(0.02f, 0.03f, 0.05f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	speedometer(r, s->speed_kmh);
	number3(r, (struct vec2){ SPD_CX, SPD_CY + 70.0f }, (int)(s->speed_kmh + 0.5f));
	rpm_bar(r, s->rpm);
	telltales(r, s->telltales);
	if (s->paused)
		pause_glyph(r);
	return r->dropped;
}

void render_shutdown(struct renderer *r)
{
	if (r->program) {
		glDeleteProgram(r->program);
		r->program = 0;
	}
}
