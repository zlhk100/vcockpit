/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * cluster-gauge: renderer.
 *
 * One shader program: per-vertex position (normalised device coordinates),
 * uniform colour. All geometry is built on the CPU each frame as triangle
 * lists in a 1280x720 layout, scaled to the viewport. Digits are seven-
 * segment quads, so no font is needed. All shapes are drawn here.
 */
#include <math.h>
#include <stdio.h>

#include <GLES2/gl2.h>

#include "render.h"

#define LAYOUT_W 1280.0f
#define LAYOUT_H 720.0f
#define MAX_VERTS 8192
#define PI_F 3.14159265f

/* Speedometer */
#define SPD_CX        640.0f
#define SPD_CY        380.0f
#define SPD_R_OUT     270.0f
#define SPD_R_IN      250.0f
#define SPD_A_START   225.0f	/* degrees, 0 km/h */
#define SPD_SWEEP     270.0f	/* degrees, clockwise to SPEED_MAX_KMH */

/* RPM bar */
#define RPM_X         240.0f
#define RPM_Y         660.0f
#define RPM_W         800.0f
#define RPM_H          28.0f
#define RPM_REDLINE  6000.0f

/* Telltale row */
#define TT_Y           70.0f

struct rgba { float r, g, b, a; };

static const struct rgba col_track   = { 0.16f, 0.18f, 0.22f, 1.0f };
static const struct rgba col_arc     = { 0.10f, 0.75f, 0.95f, 1.0f };
static const struct rgba col_tick    = { 0.80f, 0.84f, 0.90f, 1.0f };
static const struct rgba col_needle  = { 1.00f, 0.35f, 0.10f, 1.0f };
static const struct rgba col_hub     = { 0.30f, 0.32f, 0.36f, 1.0f };
static const struct rgba col_seg_on  = { 0.92f, 0.95f, 1.00f, 1.0f };
static const struct rgba col_seg_off = { 0.08f, 0.09f, 0.11f, 1.0f };
static const struct rgba col_rpm     = { 0.95f, 0.80f, 0.20f, 1.0f };
static const struct rgba col_red     = { 0.45f, 0.06f, 0.06f, 1.0f };
static const struct rgba col_tt_off  = { 0.10f, 0.11f, 0.13f, 1.0f };
static const struct rgba col_green   = { 0.15f, 0.85f, 0.25f, 1.0f };
static const struct rgba col_warn    = { 0.95f, 0.15f, 0.12f, 1.0f };
static const struct rgba col_blue    = { 0.20f, 0.45f, 1.00f, 1.0f };
static const struct rgba col_amber   = { 1.00f, 0.65f, 0.05f, 1.0f };

static GLuint program;
static GLint loc_pos = -1;
static GLint loc_color = -1;
static float scale_x, scale_y;
static GLfloat verts[MAX_VERTS * 2];
static int nverts;

/* ---- geometry helpers (layout pixels, y down) ---- */

static void vtx(float x, float y)
{
	if (nverts >= MAX_VERTS)
		return;
	verts[nverts * 2] = x * scale_x - 1.0f;
	verts[nverts * 2 + 1] = 1.0f - y * scale_y;
	nverts++;
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2)
{
	vtx(x0, y0);
	vtx(x1, y1);
	vtx(x2, y2);
}

static void quad(float x0, float y0, float x1, float y1,
		 float x2, float y2, float x3, float y3)
{
	tri(x0, y0, x1, y1, x2, y2);
	tri(x0, y0, x2, y2, x3, y3);
}

static void rect(float x, float y, float w, float h)
{
	quad(x, y, x + w, y, x + w, y + h, x, y + h);
}

/* Ring sector between radii r_in and r_out from angle a0 to a1 (degrees,
 * counter-clockwise, 0 = +x). r_in = 0 gives a disc sector. */
static void ring(float cx, float cy, float r_in, float r_out, float a0, float a1)
{
	int n = (int)(fabsf(a1 - a0) / 3.0f) + 1;
	int i;

	for (i = 0; i < n; i++) {
		float b0 = (a0 + (a1 - a0) * (float)i / (float)n) * PI_F / 180.0f;
		float b1 = (a0 + (a1 - a0) * (float)(i + 1) / (float)n) * PI_F / 180.0f;
		float c0 = cosf(b0), s0 = sinf(b0), c1 = cosf(b1), s1 = sinf(b1);

		quad(cx + r_in * c0, cy - r_in * s0, cx + r_out * c0, cy - r_out * s0,
		     cx + r_out * c1, cy - r_out * s1, cx + r_in * c1, cy - r_in * s1);
	}
}

static void disc(float cx, float cy, float r)
{
	ring(cx, cy, 0.0f, r, 0.0f, 360.0f);
}

/* Line of width w from (x0, y0) to (x1, y1). */
static void line(float x0, float y0, float x1, float y1, float w)
{
	float dx = x1 - x0, dy = y1 - y0;
	float len = sqrtf(dx * dx + dy * dy);
	float nx, ny;

	if (len <= 0.0f)
		return;
	nx = -dy / len * w * 0.5f;
	ny = dx / len * w * 0.5f;
	quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny);
}

static void draw(const struct rgba *c)
{
	if (nverts == 0)
		return;
	glUniform4f(loc_color, c->r, c->g, c->b, c->a);
	glVertexAttribPointer((GLuint)loc_pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
	glDrawArrays(GL_TRIANGLES, 0, nverts);
	nverts = 0;
}

/* ---- seven-segment digits ---- */

/* Bits a..g = 0..6 */
static const unsigned char seg_digits[10] = {
	0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};

static void segments(float x, float y, float w, float h, float t, unsigned mask)
{
	float h2 = h * 0.5f;
	float vl = h2 - 1.5f * t;

	if (mask & 0x01) rect(x + t, y, w - 2.0f * t, t);			/* a */
	if (mask & 0x02) rect(x + w - t, y + t, t, vl);				/* b */
	if (mask & 0x04) rect(x + w - t, y + h2 + 0.5f * t, t, vl);		/* c */
	if (mask & 0x08) rect(x + t, y + h - t, w - 2.0f * t, t);		/* d */
	if (mask & 0x10) rect(x, y + h2 + 0.5f * t, t, vl);			/* e */
	if (mask & 0x20) rect(x, y + t, t, vl);					/* f */
	if (mask & 0x40) rect(x + t, y + h2 - 0.5f * t, w - 2.0f * t, t);	/* g */
}

/* Three-digit number centred at cx, top at y; leading zeros blank. */
static void number3(float cx, float y, int value)
{
	const float w = 52.0f, h = 92.0f, t = 11.0f, gap = 14.0f;
	float x0 = cx - (3.0f * w + 2.0f * gap) * 0.5f;
	unsigned masks[3];
	int d[3], i;

	if (value < 0)
		value = 0;
	if (value > 999)
		value = 999;
	d[0] = value / 100;
	d[1] = (value / 10) % 10;
	d[2] = value % 10;
	masks[0] = value >= 100 ? seg_digits[d[0]] : 0;
	masks[1] = value >= 10 ? seg_digits[d[1]] : 0;
	masks[2] = seg_digits[d[2]];

	for (i = 0; i < 3; i++)
		segments(x0 + (float)i * (w + gap), y, w, h, t, 0x7f);
	draw(&col_seg_off);
	for (i = 0; i < 3; i++)
		segments(x0 + (float)i * (w + gap), y, w, h, t, masks[i]);
	draw(&col_seg_on);
}

/* ---- telltale shapes ---- */

static void arrow(float cx, float cy, int dir)
{
	float s = (float)dir;	/* -1 left, +1 right */

	tri(cx + s * 34.0f, cy, cx + s * 4.0f, cy - 28.0f, cx + s * 4.0f, cy + 28.0f);
	rect(dir < 0 ? cx + 4.0f : cx - 30.0f, cy - 10.0f, 26.0f, 20.0f);
}

static void seatbelt(float cx, float cy)
{
	disc(cx, cy - 22.0f, 9.0f);
	quad(cx - 14.0f, cy - 8.0f, cx + 14.0f, cy - 8.0f,
	     cx + 18.0f, cy + 30.0f, cx - 18.0f, cy + 30.0f);
}

static void seatbelt_strap(float cx, float cy)
{
	line(cx - 16.0f, cy - 10.0f, cx + 16.0f, cy + 26.0f, 6.0f);
}

static void high_beam(float cx, float cy)
{
	int i;

	ring(cx + 4.0f, cy, 0.0f, 22.0f, -90.0f, 90.0f);
	rect(cx + 1.0f, cy - 22.0f, 4.0f, 44.0f);
	for (i = 0; i < 4; i++)
		rect(cx - 34.0f, cy - 18.0f + (float)i * 11.0f, 28.0f, 5.0f);
}

static void engine(float cx, float cy)
{
	rect(cx - 22.0f, cy - 14.0f, 40.0f, 30.0f);	/* block */
	rect(cx - 12.0f, cy - 24.0f, 20.0f, 6.0f);	/* filler cap */
	rect(cx - 4.0f, cy - 20.0f, 4.0f, 6.0f);
	rect(cx + 18.0f, cy - 6.0f, 10.0f, 14.0f);	/* right side */
	rect(cx - 32.0f, cy - 4.0f, 10.0f, 10.0f);	/* left side */
}

static void telltales(unsigned bits)
{
	struct {
		unsigned bit;
		float x;
		const struct rgba *col;
	} tt[] = {
		{ TT_TURN_LEFT, 100.0f, &col_green },
		{ TT_SEATBELT, 470.0f, &col_warn },
		{ TT_HIGH_BEAM, 640.0f, &col_blue },
		{ TT_CHECK_ENGINE, 810.0f, &col_amber },
		{ TT_TURN_RIGHT, 1180.0f, &col_green },
	};
	unsigned i;

	for (i = 0; i < sizeof(tt) / sizeof(tt[0]); i++) {
		switch (tt[i].bit) {
		case TT_TURN_LEFT:
			arrow(tt[i].x, TT_Y, -1);
			break;
		case TT_TURN_RIGHT:
			arrow(tt[i].x, TT_Y, 1);
			break;
		case TT_SEATBELT:
			seatbelt(tt[i].x, TT_Y);
			break;
		case TT_HIGH_BEAM:
			high_beam(tt[i].x, TT_Y);
			break;
		default:
			engine(tt[i].x, TT_Y);
			break;
		}
		draw((bits & tt[i].bit) ? tt[i].col : &col_tt_off);
		if (tt[i].bit == TT_SEATBELT) {
			seatbelt_strap(tt[i].x, TT_Y);
			draw(&col_seg_off);
		}
	}
}

/* ---- gauges ---- */

static float speed_angle(float v)
{
	return SPD_A_START - SPD_SWEEP * (v / SPEED_MAX_KMH);
}

static void speedometer(float v)
{
	float a = speed_angle(v) * PI_F / 180.0f;
	int k;

	ring(SPD_CX, SPD_CY, SPD_R_IN, SPD_R_OUT, SPD_A_START - SPD_SWEEP, SPD_A_START);
	draw(&col_track);
	if (v > 0.0f) {
		ring(SPD_CX, SPD_CY, SPD_R_IN, SPD_R_OUT, speed_angle(v), SPD_A_START);
		draw(&col_arc);
	}

	for (k = 0; k <= 20; k++) {
		float b = speed_angle((float)k * 10.0f) * PI_F / 180.0f;
		float r0 = (k % 2 == 0) ? SPD_R_IN - 34.0f : SPD_R_IN - 18.0f;
		float r1 = SPD_R_IN - 6.0f;

		line(SPD_CX + r0 * cosf(b), SPD_CY - r0 * sinf(b),
		     SPD_CX + r1 * cosf(b), SPD_CY - r1 * sinf(b),
		     (k % 2 == 0) ? 5.0f : 3.0f);
	}
	draw(&col_tick);

	{
		float r = SPD_R_IN - 10.0f;
		float px = -sinf(a) * 9.0f, py = -cosf(a) * 9.0f;

		tri(SPD_CX + r * cosf(a), SPD_CY - r * sinf(a),
		    SPD_CX + px, SPD_CY + py, SPD_CX - px, SPD_CY - py);
		draw(&col_needle);
	}
	disc(SPD_CX, SPD_CY, 22.0f);
	draw(&col_hub);
}

static void rpm_bar(float rpm)
{
	float f = rpm / RPM_MAX;
	float red_x = RPM_X + RPM_W * (RPM_REDLINE / RPM_MAX);
	int k;

	if (f < 0.0f)
		f = 0.0f;
	if (f > 1.0f)
		f = 1.0f;
	rect(RPM_X, RPM_Y, red_x - RPM_X, RPM_H);
	draw(&col_track);
	rect(red_x, RPM_Y, RPM_X + RPM_W - red_x, RPM_H);
	draw(&col_red);
	rect(RPM_X, RPM_Y, RPM_W * f, RPM_H);
	draw(&col_rpm);
	for (k = 0; k <= 7; k++)
		rect(RPM_X + RPM_W * (float)k / 7.0f - 1.5f, RPM_Y - 12.0f, 3.0f, 10.0f);
	draw(&col_tick);
}

/* ---- public ---- */

static GLuint compile(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	GLint ok = 0;

	if (sh == 0) {
		fprintf(stderr, "cluster-gauge: glCreateShader failed: GL error 0x%04x\n",
			(unsigned)glGetError());
		return 0;
	}
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		fprintf(stderr, "cluster-gauge: glCompileShader failed: %s\n", log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

int render_init(int width, int height)
{
	static const char vs_src[] =
		"attribute vec2 a_pos;\n"
		"void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
	static const char fs_src[] =
		"precision mediump float;\n"
		"uniform vec4 u_color;\n"
		"void main() { gl_FragColor = u_color; }\n";
	GLuint vs, fs;
	GLint ok = 0;

	(void)width;
	(void)height;
	/* Layout pixels to normalised device coordinates; the layout fills the
	 * viewport whatever its size. */
	scale_x = 2.0f / LAYOUT_W;
	scale_y = 2.0f / LAYOUT_H;

	vs = compile(GL_VERTEX_SHADER, vs_src);
	if (vs == 0)
		return -1;
	fs = compile(GL_FRAGMENT_SHADER, fs_src);
	if (fs == 0) {
		glDeleteShader(vs);
		return -1;
	}
	program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		fprintf(stderr, "cluster-gauge: glLinkProgram failed: %s\n", log);
		glDeleteProgram(program);
		program = 0;
		return -1;
	}
	loc_pos = glGetAttribLocation(program, "a_pos");
	loc_color = glGetUniformLocation(program, "u_color");
	if (loc_pos < 0 || loc_color < 0) {
		fprintf(stderr, "cluster-gauge: glGetAttribLocation/glGetUniformLocation failed\n");
		glDeleteProgram(program);
		program = 0;
		return -1;
	}
	glUseProgram(program);
	glEnableVertexAttribArray((GLuint)loc_pos);
	return 0;
}

void render_frame(const struct gauge_state *s)
{
	glClearColor(0.02f, 0.03f, 0.05f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	speedometer(s->speed_kmh);
	number3(SPD_CX, SPD_CY + 70.0f, (int)(s->speed_kmh + 0.5f));
	rpm_bar(s->rpm);
	telltales(s->telltales);
}

void render_shutdown(void)
{
	if (program) {
		glDeleteProgram(program);
		program = 0;
	}
}
