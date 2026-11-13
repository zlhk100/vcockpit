/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
/*
 * Input forwarding: Wayland pointer and keyboard events on the proxy's own
 * surfaces go to QEMU's org.qemu.Display1.Mouse and .Keyboard interfaces on
 * the Console_<i> that feeds the surface, over the session bus.
 *
 * All callbacks here run on the one GMainLoop thread (Wayland source and
 * GDBus dispatch), so the state in WaylandState needs no locking. This file
 * defines no mutable object with static storage duration.
 */
#define _GNU_SOURCE
#include "input.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define QEMU_DEST		"org.qemu"
#define QEMU_MOUSE_IFACE	"org.qemu.Display1.Mouse"
#define QEMU_KEYBOARD_IFACE	"org.qemu.Display1.Keyboard"
#define PROPS_IFACE		"org.freedesktop.DBus.Properties"
#define SEAT_MAX_VERSION	5u
#define QEMU_WHEEL_UP		3u
#define QEMU_WHEEL_DOWN		4u
#define N_KEY_WORDS		8u	/* held_keys: 8 x 32 bits = QEMU key numbers 0..255 */
#define ERROR_LOG_FIRST		10ul
#define ERROR_LOG_EVERY		100ul

/*
 * evdev key code -> QEMU key number ("qnum", AT set 1; extended keys are
 * 0x80 | the code after the E0 prefix). Written by hand from the evdev and
 * AT set 1 columns of keycodemapdb's keymaps.csv, as listed in the design:
 * evdev 1-84 and 86-88 map to the same number; the extended keys below are
 * listed one by one. 0 means unmapped: such keys are dropped and counted.
 */
static const uint8_t evdev_to_qnum[128] = {
	[1] = 0x01, [2] = 0x02, [3] = 0x03, [4] = 0x04, [5] = 0x05, [6] = 0x06,
	[7] = 0x07, [8] = 0x08, [9] = 0x09, [10] = 0x0a, [11] = 0x0b, [12] = 0x0c,
	[13] = 0x0d, [14] = 0x0e, [15] = 0x0f, [16] = 0x10, [17] = 0x11, [18] = 0x12,
	[19] = 0x13, [20] = 0x14, [21] = 0x15, [22] = 0x16, [23] = 0x17, [24] = 0x18,
	[25] = 0x19, [26] = 0x1a, [27] = 0x1b, [28] = 0x1c, [29] = 0x1d, [30] = 0x1e,
	[31] = 0x1f, [32] = 0x20, [33] = 0x21, [34] = 0x22, [35] = 0x23, [36] = 0x24,
	[37] = 0x25, [38] = 0x26, [39] = 0x27, [40] = 0x28, [41] = 0x29, [42] = 0x2a,
	[43] = 0x2b, [44] = 0x2c, [45] = 0x2d, [46] = 0x2e, [47] = 0x2f, [48] = 0x30,
	[49] = 0x31, [50] = 0x32, [51] = 0x33, [52] = 0x34, [53] = 0x35, [54] = 0x36,
	[55] = 0x37, [56] = 0x38, [57] = 0x39, [58] = 0x3a, [59] = 0x3b, [60] = 0x3c,
	[61] = 0x3d, [62] = 0x3e, [63] = 0x3f, [64] = 0x40, [65] = 0x41, [66] = 0x42,
	[67] = 0x43, [68] = 0x44, [69] = 0x45, [70] = 0x46, [71] = 0x47, [72] = 0x48,
	[73] = 0x49, [74] = 0x4a, [75] = 0x4b, [76] = 0x4c, [77] = 0x4d, [78] = 0x4e,
	[79] = 0x4f, [80] = 0x50, [81] = 0x51, [82] = 0x52, [83] = 0x53, [84] = 0x54,
	[86] = 0x56, [87] = 0x57, [88] = 0x58,
	[96] = 0x9c,	/* KEY_KPENTER */
	[97] = 0x9d,	/* KEY_RIGHTCTRL */
	[100] = 0xb8,	/* KEY_RIGHTALT */
	[102] = 0xc7,	/* KEY_HOME */
	[103] = 0xc8,	/* KEY_UP */
	[104] = 0xc9,	/* KEY_PAGEUP */
	[105] = 0xcb,	/* KEY_LEFT */
	[106] = 0xcd,	/* KEY_RIGHT */
	[107] = 0xcf,	/* KEY_END */
	[108] = 0xd0,	/* KEY_DOWN */
	[109] = 0xd1,	/* KEY_PAGEDOWN */
	[110] = 0xd2,	/* KEY_INSERT */
	[111] = 0xd3,	/* KEY_DELETE */
	[125] = 0xdb,	/* KEY_LEFTMETA */
};

/* evdev button code -> QEMU button number (ui/dbus-display1.xml, Mouse.Press). */
static const struct {
	uint32_t evdev;
	uint32_t qemu;
} button_map[] = {
	{ 0x110, 0 },	/* BTN_LEFT */
	{ 0x111, 2 },	/* BTN_RIGHT */
	{ 0x112, 1 },	/* BTN_MIDDLE */
	{ 0x113, 5 },	/* BTN_SIDE */
	{ 0x114, 6 },	/* BTN_EXTRA */
};

/*
 * Pointer cursor, drawn by the proxy: neither guest's cursor reaches the host.
 * 'X' black outline, 'o' white fill, '.' transparent. Hotspot (0,0).
 */
#define CURSOR_W	16
#define CURSOR_H	24
#define CURSOR_BLACK	0xff000000u	/* ARGB8888, premultiplied */
#define CURSOR_WHITE	0xffffffffu

static const char cursor_rows[CURSOR_H][CURSOR_W + 1] = {
	"X...............",
	"XX..............",
	"XoX.............",
	"XooX............",
	"XoooX...........",
	"XooooX..........",
	"XoooooX.........",
	"XooooooX........",
	"XoooooooX.......",
	"XooooooooX......",
	"XoooooooooX.....",
	"XooooooooooX....",
	"XoooooooooooX...",
	"XoooooooXXXXX...",
	"XoooXoooX.......",
	"XooXXoooX.......",
	"XoX..XoooX......",
	"XX...XoooX......",
	"X.....XoooX.....",
	"......XoooX.....",
	".......XoooX....",
	".......XoooX....",
	"........XXX.....",
	"................",
};

int input_cursor_init(WaylandState *ws)
{
	const size_t stride = CURSOR_W * sizeof(uint32_t);
	const size_t size = stride * CURSOR_H;
	int fd = memfd_create("proxy-cursor", MFD_CLOEXEC);

	if (fd < 0) {
		perror("Input: memfd_create(cursor)");
		return -1;
	}
	ws->cursor_fd = fd;	/* closed by wayland_destroy */
	if (ftruncate(fd, (off_t)size) != 0) {
		perror("Input: ftruncate(cursor)");
		return -1;
	}

	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (px == MAP_FAILED) {
		perror("Input: mmap(cursor)");
		return -1;
	}
	for (size_t y = 0; y < CURSOR_H; y++) {
		for (size_t x = 0; x < CURSOR_W; x++) {
			char c = cursor_rows[y][x];

			px[y * CURSOR_W + x] = c == 'X' ? CURSOR_BLACK :
					       c == 'o' ? CURSOR_WHITE : 0u;
		}
	}
	if (munmap(px, size) != 0)
		perror("Input: munmap(cursor)");	/* mapping leaks; buffer still valid */

	ws->cursor_pool = wl_shm_create_pool(ws->shm, fd, (int32_t)size);
	ws->cursor_buffer = ws->cursor_pool ?
		wl_shm_pool_create_buffer(ws->cursor_pool, 0, CURSOR_W, CURSOR_H,
					  (int32_t)stride, WL_SHM_FORMAT_ARGB8888) : NULL;
	ws->cursor_surface = wl_compositor_create_surface(ws->compositor);
	if (!ws->cursor_pool || !ws->cursor_buffer || !ws->cursor_surface) {
		fprintf(stderr, "Input: cursor buffer or surface not created\n");
		return -1;
	}
	wl_surface_attach(ws->cursor_surface, ws->cursor_buffer, 0, 0);
	wl_surface_damage(ws->cursor_surface, 0, 0, CURSOR_W, CURSOR_H);
	wl_surface_commit(ws->cursor_surface);
	return 0;
}

static int scanout_for_surface(const WaylandState *ws, const struct wl_surface *surface)
{
	if (!surface)
		return -1;
	for (int i = 0; i < ws->n_scanouts; i++) {
		if (ws->scanouts[i].surface == surface)
			return i;
	}
	return -1;
}

/* Reply handler for calls whose errors are counted (P6: never fatal). */
static void call_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
	WaylandState *ws = user_data;
	GError *error = NULL;
	GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

	if (reply) {
		g_variant_unref(reply);
		return;
	}
	ws->call_errors++;
	if (ws->call_errors <= ERROR_LOG_FIRST || ws->call_errors % ERROR_LOG_EVERY == 0)
		fprintf(stderr, "Input: call error %lu: %s\n", ws->call_errors,
			error ? error->message : "unknown");
	g_clear_error(&error);
}

/*
 * Send one input call to Console_<i>. With counted == false no reply is
 * requested (GDBus sets NO_REPLY_EXPECTED when the callback is NULL).
 */
static void qemu_call(WaylandState *ws, int i, const char *iface, const char *method,
		      GVariant *args, bool counted)
{
	if (!ws->session_bus) {
		g_variant_unref(g_variant_ref_sink(args));
		return;
	}
	g_dbus_connection_call(ws->session_bus, QEMU_DEST, ws->scanouts[i].console_path, iface,
			       method, args, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
			       counted ? call_done : NULL, counted ? ws : NULL);
}

/* floor(v) clamped to [0, size-1]; truncation equals floor for v >= 0. */
static int clamp_coord(wl_fixed_t v, int size)
{
	int c = wl_fixed_to_int(v);

	if (v < 0 || c < 0)
		return 0;
	if (c > size - 1)
		return size - 1;
	return c;
}

/* Send the pending motion, if any, to the focused scanout (P5: clamped). */
static void flush_motion(WaylandState *ws)
{
	int i = ws->pointer_focus;

	if (!ws->pending_motion || i < 0)
		return;
	ws->pending_motion = false;
	if (ws->scanouts[i].width <= 0 || ws->scanouts[i].height <= 0)
		return;	/* no frame yet: guest size unknown */

	int gx = clamp_coord(ws->pending_x, ws->scanouts[i].width);
	int gy = clamp_coord(ws->pending_y, ws->scanouts[i].height);

	if (ws->scanouts[i].is_absolute) {
		qemu_call(ws, i, QEMU_MOUSE_IFACE, "SetAbsPosition",
			  g_variant_new("(uu)", (guint32)gx, (guint32)gy), false);
		ws->sent_motion++;
	} else {
		int dx = gx - ws->scanouts[i].last_x;
		int dy = gy - ws->scanouts[i].last_y;

		if (dx == 0 && dy == 0)
			return;
		qemu_call(ws, i, QEMU_MOUSE_IFACE, "RelMotion", g_variant_new("(ii)", dx, dy),
			  false);
		ws->sent_motion++;
	}
	ws->scanouts[i].last_x = gx;
	ws->scanouts[i].last_y = gy;
	if (gx < ws->range_min_x)
		ws->range_min_x = gx;
	if (gx > ws->range_max_x)
		ws->range_max_x = gx;
	if (gy < ws->range_min_y)
		ws->range_min_y = gy;
	if (gy > ws->range_max_y)
		ws->range_max_y = gy;
}

static void send_button(WaylandState *ws, int i, uint32_t qbutton, bool press)
{
	qemu_call(ws, i, QEMU_MOUSE_IFACE, press ? "Press" : "Release",
		  g_variant_new("(u)", qbutton), true);
	ws->sent_button++;
}

/* P3: release every button held in the guest. */
static void release_buttons(WaylandState *ws, int i)
{
	for (uint32_t b = 0; b < 32u; b++) {
		if (ws->held_buttons & (1u << b))
			send_button(ws, i, b, false);
	}
	ws->held_buttons = 0;
}

static bool key_held(const WaylandState *ws, uint32_t q)
{
	return (ws->held_keys[q / 32u] & (1u << (q % 32u))) != 0;
}

static void set_key_held(WaylandState *ws, uint32_t q, bool held)
{
	if (held)
		ws->held_keys[q / 32u] |= 1u << (q % 32u);
	else
		ws->held_keys[q / 32u] &= ~(1u << (q % 32u));
}

/* P3: release every key held in the guest. Returns the number of keys released. */
static unsigned int release_keys(WaylandState *ws, int i)
{
	unsigned int released = 0;

	for (uint32_t q = 0; q < N_KEY_WORDS * 32u; q++) {
		if (key_held(ws, q)) {
			qemu_call(ws, i, QEMU_KEYBOARD_IFACE, "Release", g_variant_new("(u)", q),
				  true);
			ws->sent_key++;
			set_key_held(ws, q, false);
			released++;
		}
	}
	return released;
}

/*
 * Wayland events arrive through one dispatcher per interface
 * (wl_proxy_add_dispatcher) instead of listener tables: a listener table of
 * function pointers is const, but lands in a relocated data section, and
 * this file keeps no data section objects at all. Event opcodes are the
 * order of the events in the core protocol (wayland.xml), the same order
 * as the members of wl_seat_listener, wl_pointer_listener and
 * wl_keyboard_listener.
 */
enum seat_event { SEAT_CAPABILITIES = 0, SEAT_NAME = 1 };
enum pointer_event {
	POINTER_ENTER = 0, POINTER_LEAVE = 1, POINTER_MOTION = 2, POINTER_BUTTON = 3,
	POINTER_AXIS = 4, POINTER_FRAME = 5	/* 6-8: axis_source, axis_stop, axis_discrete */
};
enum keyboard_event {
	KEYBOARD_KEYMAP = 0, KEYBOARD_ENTER = 1, KEYBOARD_LEAVE = 2, KEYBOARD_KEY = 3
	/* 4: modifiers (the guest derives them from key events);
	 * 5: repeat_info (the guest does its own repeat) */
};

/* ---- wl_pointer ---- */

static void pointer_enter(WaylandState *ws, uint32_t serial, struct wl_surface *surface,
			  wl_fixed_t sx, wl_fixed_t sy)
{
	ws->pointer_focus = scanout_for_surface(ws, surface);
	if (ws->pointer_focus < 0)
		return;	/* P2: not ours */
	if (ws->pointer && ws->cursor_buffer && ws->cursor_surface)
		wl_pointer_set_cursor(ws->pointer, serial, ws->cursor_surface, 0, 0);
	ws->range_min_x = ws->range_min_y = 1 << 30;
	ws->range_max_x = ws->range_max_y = -1;
	ws->pending_x = sx;
	ws->pending_y = sy;
	ws->pending_motion = true;
	printf("Input: pointer enter scanout %d at (%.1f,%.1f), mouse %s\n", ws->pointer_focus,
	       wl_fixed_to_double(sx), wl_fixed_to_double(sy),
	       ws->scanouts[ws->pointer_focus].is_absolute ? "absolute" : "relative");
	if (ws->seat_version < 5u)
		flush_motion(ws);
}

static void pointer_leave(WaylandState *ws)
{
	int i = ws->pointer_focus;

	if (i < 0)
		return;
	flush_motion(ws);
	release_buttons(ws, i);
	printf("Input: pointer leave scanout %d at (%d,%d); range x %d..%d, y %d..%d\n", i,
	       ws->scanouts[i].last_x, ws->scanouts[i].last_y, ws->range_min_x,
	       ws->range_max_x, ws->range_min_y, ws->range_max_y);
	ws->pointer_focus = -1;

	/* Keys follow the pointer: keys pressed while it was here are released now. */
	if (ws->keyboard_focus >= 0) {
		unsigned int released = release_keys(ws, ws->keyboard_focus);

		if (released > 0)
			printf("Input: pointer leave: released %u key(s) on Console_%d\n", released,
			       ws->keyboard_focus);
	}
}

static void pointer_motion(WaylandState *ws, wl_fixed_t sx, wl_fixed_t sy)
{
	if (ws->pointer_focus < 0)
		return;
	ws->pending_x = sx;
	ws->pending_y = sy;
	ws->pending_motion = true;
	if (ws->seat_version < 5u)
		flush_motion(ws);
}

static void pointer_button(WaylandState *ws, uint32_t button, uint32_t state)
{
	int i = ws->pointer_focus;

	if (i < 0)
		return;
	for (size_t n = 0; n < G_N_ELEMENTS(button_map); n++) {
		if (button_map[n].evdev != button)
			continue;

		uint32_t q = button_map[n].qemu;
		uint32_t bit = 1u << q;

		flush_motion(ws);	/* the click lands where the pointer is */
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			send_button(ws, i, q, true);
			ws->held_buttons |= bit;
			printf("Input: Console_%d button %u press at (%d,%d)\n", i, q,
			       ws->scanouts[i].last_x, ws->scanouts[i].last_y);
		} else if (ws->held_buttons & bit) {
			send_button(ws, i, q, false);
			ws->held_buttons &= ~bit;
		}
		return;
	}
	/* Unmapped button: dropped. */
}

static void pointer_axis(WaylandState *ws, uint32_t axis, wl_fixed_t value)
{
	int i = ws->pointer_focus;

	if (i < 0 || axis != WL_POINTER_AXIS_VERTICAL_SCROLL || value == 0)
		return;

	uint32_t q = value < 0 ? QEMU_WHEEL_UP : QEMU_WHEEL_DOWN;

	flush_motion(ws);
	send_button(ws, i, q, true);
	send_button(ws, i, q, false);
}

static int pointer_dispatch(const void *impl, void *target, uint32_t opcode,
			    const struct wl_message *msg, union wl_argument *args)
{
	WaylandState *ws = wl_proxy_get_user_data(target);
	(void)impl; (void)msg;

	switch (opcode) {
	case POINTER_ENTER:	/* serial, surface, surface_x, surface_y */
		pointer_enter(ws, args[0].u, (struct wl_surface *)args[1].o, args[2].f,
			      args[3].f);
		break;
	case POINTER_LEAVE:	/* serial, surface */
		pointer_leave(ws);
		break;
	case POINTER_MOTION:	/* time, surface_x, surface_y */
		pointer_motion(ws, args[1].f, args[2].f);
		break;
	case POINTER_BUTTON:	/* serial, time, button, state */
		pointer_button(ws, args[2].u, args[3].u);
		break;
	case POINTER_AXIS:	/* time, axis, value */
		pointer_axis(ws, args[1].u, args[2].f);
		break;
	case POINTER_FRAME:
		flush_motion(ws);
		break;
	default:		/* axis_source, axis_stop, axis_discrete: not used */
		break;
	}
	return 0;
}

/* ---- wl_keyboard ---- */

static void keyboard_enter(WaylandState *ws, struct wl_surface *surface)
{
	/* Keys already down at enter are not replayed. */
	ws->keyboard_focus = scanout_for_surface(ws, surface);
	if (ws->keyboard_focus >= 0)
		printf("Input: keyboard enter scanout %d\n", ws->keyboard_focus);
}

static void keyboard_leave(WaylandState *ws)
{
	if (ws->keyboard_focus < 0)
		return;
	release_keys(ws, ws->keyboard_focus);
	printf("Input: keyboard leave scanout %d\n", ws->keyboard_focus);
	ws->keyboard_focus = -1;
}

static void keyboard_key(WaylandState *ws, uint32_t key, uint32_t state)
{
	int i = ws->keyboard_focus;

	if (i < 0)
		return;

	uint32_t q = key < G_N_ELEMENTS(evdev_to_qnum) ? evdev_to_qnum[key] : 0u;

	if (q == 0u) {
		ws->dropped_key_unmapped++;	/* P4 */
		return;
	}
	/*
	 * The compositor gives keyboard focus to every guest surface; a key is
	 * sent only while the pointer is on this proxy's surface. A press while
	 * the pointer is elsewhere is counted; a release then has nothing to
	 * release (keys are released on pointer leave) and is dropped below.
	 */
	if (ws->pointer_focus < 0 && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		ws->gated_keys++;
		return;
	}
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		qemu_call(ws, i, QEMU_KEYBOARD_IFACE, "Press", g_variant_new("(u)", q), true);
		ws->sent_key++;
		set_key_held(ws, q, true);
	} else if (key_held(ws, q)) {
		qemu_call(ws, i, QEMU_KEYBOARD_IFACE, "Release", g_variant_new("(u)", q), true);
		ws->sent_key++;
		set_key_held(ws, q, false);
	}
}

static int keyboard_dispatch(const void *impl, void *target, uint32_t opcode,
			     const struct wl_message *msg, union wl_argument *args)
{
	WaylandState *ws = wl_proxy_get_user_data(target);
	(void)impl; (void)msg;

	switch (opcode) {
	case KEYBOARD_KEYMAP:	/* format, fd, size */
		/* Key numbers are forwarded, not keysyms: the keymap is not needed. */
		if (close(args[1].h) != 0)
			perror("Input: close(keymap fd)");
		break;
	case KEYBOARD_ENTER:	/* serial, surface, keys */
		keyboard_enter(ws, (struct wl_surface *)args[1].o);
		break;
	case KEYBOARD_LEAVE:	/* serial, surface */
		keyboard_leave(ws);
		break;
	case KEYBOARD_KEY:	/* serial, time, key, state */
		keyboard_key(ws, args[2].u, args[3].u);
		break;
	default:		/* modifiers, repeat_info: not used */
		break;
	}
	return 0;
}

/* ---- wl_seat ---- */

static void destroy_pointer(WaylandState *ws)
{
	if (!ws->pointer)
		return;
	if (wl_proxy_get_version((struct wl_proxy *)ws->pointer) >=
	    WL_POINTER_RELEASE_SINCE_VERSION)
		wl_pointer_release(ws->pointer);
	else
		wl_pointer_destroy(ws->pointer);
	ws->pointer = NULL;
	ws->pointer_focus = -1;
}

static void destroy_keyboard(WaylandState *ws)
{
	if (!ws->keyboard)
		return;
	if (wl_proxy_get_version((struct wl_proxy *)ws->keyboard) >=
	    WL_KEYBOARD_RELEASE_SINCE_VERSION)
		wl_keyboard_release(ws->keyboard);
	else
		wl_keyboard_destroy(ws->keyboard);
	ws->keyboard = NULL;
	ws->keyboard_focus = -1;
}

static void seat_capabilities(WaylandState *ws, struct wl_seat *seat, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ws->pointer) {
		ws->pointer = wl_seat_get_pointer(seat);
		if (!ws->pointer)
			fprintf(stderr, "Input: wl_seat_get_pointer failed\n");
		else if (wl_proxy_add_dispatcher((struct wl_proxy *)ws->pointer, pointer_dispatch,
						 NULL, ws) != 0)
			fprintf(stderr, "Input: pointer dispatcher not set\n");
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ws->pointer) {
		if (ws->pointer_focus >= 0)
			release_buttons(ws, ws->pointer_focus);
		destroy_pointer(ws);
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !ws->keyboard) {
		ws->keyboard = wl_seat_get_keyboard(seat);
		if (!ws->keyboard)
			fprintf(stderr, "Input: wl_seat_get_keyboard failed\n");
		else if (wl_proxy_add_dispatcher((struct wl_proxy *)ws->keyboard,
						 keyboard_dispatch, NULL, ws) != 0)
			fprintf(stderr, "Input: keyboard dispatcher not set\n");
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && ws->keyboard) {
		if (ws->keyboard_focus >= 0)
			release_keys(ws, ws->keyboard_focus);
		destroy_keyboard(ws);
	}
	printf("Input: seat capabilities: pointer %s, keyboard %s\n",
	       ws->pointer ? "yes" : "no", ws->keyboard ? "yes" : "no");
}

static int seat_dispatch(const void *impl, void *target, uint32_t opcode,
			 const struct wl_message *msg, union wl_argument *args)
{
	WaylandState *ws = wl_proxy_get_user_data(target);
	(void)impl; (void)msg;

	if (opcode == SEAT_CAPABILITIES)	/* capabilities */
		seat_capabilities(ws, target, args[0].u);
	/* SEAT_NAME: not used */
	return 0;
}

void input_bind_seat(WaylandState *ws, struct wl_registry *registry, uint32_t name,
		     uint32_t version)
{
	if (ws->seat)
		return;	/* one seat */
	ws->seat_version = version < SEAT_MAX_VERSION ? version : SEAT_MAX_VERSION;
	ws->seat = wl_registry_bind(registry, name, &wl_seat_interface, ws->seat_version);
	if (!ws->seat) {
		fprintf(stderr, "Input: wl_seat bind failed; no input forwarded\n");
		return;
	}
	if (wl_proxy_add_dispatcher((struct wl_proxy *)ws->seat, seat_dispatch, NULL, ws) != 0)
		fprintf(stderr, "Input: seat dispatcher not set; no input forwarded\n");
}

/* ---- QEMU mouse mode ---- */

static void props_changed(GDBusConnection *conn, const gchar *sender, const gchar *path,
			  const gchar *iface, const gchar *signal, GVariant *params,
			  gpointer user_data)
{
	WaylandState *ws = user_data;
	const gchar *changed_iface = NULL;
	GVariant *changed = NULL;
	gboolean abs_mode = FALSE;
	(void)conn; (void)sender; (void)iface; (void)signal;

	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(sa{sv}as)")))
		return;
	g_variant_get(params, "(&s@a{sv}as)", &changed_iface, &changed, NULL);
	if (g_strcmp0(changed_iface, QEMU_MOUSE_IFACE) == 0 &&
	    g_variant_lookup(changed, "IsAbsolute", "b", &abs_mode)) {
		for (int i = 0; i < ws->n_scanouts; i++) {
			if (g_strcmp0(ws->scanouts[i].console_path, path) != 0)
				continue;
			ws->scanouts[i].is_absolute = abs_mode != FALSE;
			printf("Console_%d: mouse is %s\n", i, abs_mode ? "absolute" : "relative");
		}
	}
	g_variant_unref(changed);
}

void input_dbus_init(WaylandState *ws)
{
	for (int i = 0; i < ws->n_scanouts; i++) {
		int n = snprintf(ws->scanouts[i].console_path,
				 sizeof(ws->scanouts[i].console_path),
				 "/org/qemu/Display1/Console_%d", i);

		if (n < 0 || (size_t)n >= sizeof(ws->scanouts[i].console_path)) {
			fprintf(stderr, "Input: Console_%d: path does not fit\n", i);
			ws->scanouts[i].console_path[0] = '\0';
			continue;
		}

		/* Subscribe first, so that no change between the read and the
		 * subscription is missed. */
		ws->scanouts[i].props_sub_id = g_dbus_connection_signal_subscribe(
			ws->session_bus, QEMU_DEST, PROPS_IFACE, "PropertiesChanged",
			ws->scanouts[i].console_path, QEMU_MOUSE_IFACE,
			G_DBUS_SIGNAL_FLAGS_NONE, props_changed, ws, NULL);

		GError *error = NULL;
		GVariant *res = g_dbus_connection_call_sync(
			ws->session_bus, QEMU_DEST, ws->scanouts[i].console_path, PROPS_IFACE,
			"Get", g_variant_new("(ss)", QEMU_MOUSE_IFACE, "IsAbsolute"),
			G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

		if (!res) {
			fprintf(stderr,
				"Console_%d: IsAbsolute read failed (%s); assuming relative\n",
				i, error ? error->message : "unknown");
			g_clear_error(&error);
			ws->scanouts[i].is_absolute = false;
		} else {
			GVariant *v = NULL;

			g_variant_get(res, "(v)", &v);
			ws->scanouts[i].is_absolute =
				g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
				g_variant_get_boolean(v);
			g_variant_unref(v);
			g_variant_unref(res);
		}
		printf("Console_%d: mouse is %s\n", i,
		       ws->scanouts[i].is_absolute ? "absolute" : "relative");
	}
}

void input_shutdown(WaylandState *ws)
{
	if (ws->pointer_focus >= 0)
		release_buttons(ws, ws->pointer_focus);
	if (ws->keyboard_focus >= 0)
		(void)release_keys(ws, ws->keyboard_focus);	/* count not needed at shutdown */

	for (int i = 0; i < ws->n_scanouts; i++) {
		if (ws->scanouts[i].props_sub_id && ws->session_bus)
			g_dbus_connection_signal_unsubscribe(ws->session_bus,
							     ws->scanouts[i].props_sub_id);
		ws->scanouts[i].props_sub_id = 0;
	}

	destroy_keyboard(ws);
	destroy_pointer(ws);
	if (ws->seat) {
		if (ws->seat_version >= WL_SEAT_RELEASE_SINCE_VERSION)
			wl_seat_release(ws->seat);
		else
			wl_seat_destroy(ws->seat);
		ws->seat = NULL;
	}

	/* Send the releases before the bus is closed. */
	if (ws->session_bus) {
		GError *error = NULL;

		if (!g_dbus_connection_flush_sync(ws->session_bus, NULL, &error)) {
			fprintf(stderr, "Input: session bus flush failed: %s\n",
				error ? error->message : "unknown");
			g_clear_error(&error);
		}
	}

	printf("Input: %lu motion, %lu button, %lu key calls; %lu unmapped keys; %lu gated keys; "
	       "%lu call errors\n",
	       ws->sent_motion, ws->sent_button, ws->sent_key, ws->dropped_key_unmapped,
	       ws->gated_keys, ws->call_errors);
}
