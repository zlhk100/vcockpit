/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * telltale-monitor: checks, several times a second, that each telltale the
 * composed display shows matches the state cluster-gauge commands, and
 * reacts when they differ for longer than a set time.
 *
 * Each cycle it loads the commanded state the gauge publishes in shared
 * memory (telltale_layout.h), captures the whole composed display with
 * screen_read_display(), classifies each telltale's sample points as lit,
 * dim or unknown, and compares. A mismatch on three consecutive compared
 * cycles is a fault: one log line, then the monitor creates its own overlay
 * window (a red band drawn with screen_fill, no GL) and shows it. The first
 * fault latches the fault state until the monitor restarts: no further
 * check runs, so neither the overlay nor a gauge the window manager pauses
 * can add faults; status lines continue. The overlay window is not created
 * before a fault because this guest's window manager pauses the other
 * programs while any other window exists. The monitor never writes the
 * shared object: its mapping is read-only.
 *
 * State and threads: one thread runs all of the program; it creates none.
 * All state is automatic in main and reached through pointers main passes.
 * SIGINT and SIGTERM are blocked before Screen starts and polled once per
 * cycle with sigtimedwait(); there is no signal handler. The cycle is
 * paced by clock_nanosleep(TIMER_ABSTIME) on CLOCK_MONOTONIC.
 *
 * Build with -DTELLTALE_MONITOR_HOST_TEST and a host compiler for the host
 * test of the classification and comparison ("make host-test").
 */
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "telltale_layout.h"

#define TOLERANCE 8		/* per channel, 8-bit units */
#define DEBOUNCE_CYCLES 3u	/* consecutive mismatching compared cycles */

/* ==== classification and comparison: no platform dependency ==== */

enum displayed { DISP_LIT, DISP_DIM, DISP_UNKNOWN };

/* Where the 1280x720 layout lies in a captured image of width x height
 * pixels, 4 bytes per pixel in memory order B, G, R, A. */
struct capture_geom {
	int width;
	int height;
	size_t stride;		/* bytes per row */
	float scale;
	float off_x;
	float off_y;
};

/* Comparison state: per telltale, the count of consecutive mismatching
 * compared cycles; and the TT_* bits of the telltales already faulted. */
struct comparison {
	unsigned mismatch[TELLTALE_COUNT];
	unsigned latched;
};

/* The layout's place in an image of size[0] x size[1] pixels, stride
 * bytes per row: scaled uniformly by min(w/1280, h/720) and centred. */
static struct capture_geom geom_for(const int size[2], size_t stride)
{
	struct capture_geom g;
	float sx = (float)size[0] / TELLTALE_LAYOUT_W;
	float sy = (float)size[1] / TELLTALE_LAYOUT_H;

	g.width = size[0];
	g.height = size[1];
	g.stride = stride;
	g.scale = sx < sy ? sx : sy;
	g.off_x = ((float)size[0] - TELLTALE_LAYOUT_W * g.scale) * 0.5f;
	g.off_y = ((float)size[1] - TELLTALE_LAYOUT_H * g.scale) * 0.5f;
	return g;
}

static int within_tolerance(int diff)
{
	return diff >= -TOLERANCE && diff <= TOLERANCE;
}

/* Pixel p (B, G, R, A) within tolerance of colour c. */
static int pixel_near(const unsigned char *p, struct telltale_rgb c)
{
	return within_tolerance((int)p[2] - (int)c.r) && within_tolerance((int)p[1] - (int)c.g) &&
	       within_tolerance((int)p[0] - (int)c.b);
}

/* What image img shows at telltale tt's sample points: lit if every one is
 * near the ON colour, dim if every one is near the OFF colour, otherwise
 * unknown. img NULL (no capture) or a point outside the image: unknown. */
static enum displayed classify(const unsigned char *img, const struct capture_geom *g,
			       const struct telltale_def *tt)
{
	int all_on = 1, all_off = 1;
	size_t i;

	if (img == NULL || tt->n_samples == 0 || tt->n_samples > TELLTALE_MAX_SAMPLES)
		return DISP_UNKNOWN;
	for (i = 0; i < tt->n_samples; i++) {
		float fx = g->off_x + tt->samples[i].x * g->scale;
		float fy = g->off_y + tt->samples[i].y * g->scale;
		const unsigned char *p;
		size_t x, y;

		if (!(fx >= 0.0f && fx < (float)g->width && fy >= 0.0f && fy < (float)g->height))
			return DISP_UNKNOWN;
		x = (size_t)fx;
		y = (size_t)fy;
		p = img + y * g->stride + x * 4u;
		if (!pixel_near(p, tt->on))
			all_on = 0;
		if (!pixel_near(p, telltale_off))
			all_off = 0;
	}
	if (all_on)
		return DISP_LIT;
	if (all_off)
		return DISP_DIM;
	return DISP_UNKNOWN;
}

/* One compared cycle: commanded TT_* bits against what is displayed for
 * each telltale of telltale_table. Returns the TT_* bits of the telltales
 * whose fault is detected in this cycle (each telltale at most once until
 * c is reset). */
static unsigned compare_cycle(struct comparison *c, unsigned commanded,
			      const enum displayed disp[TELLTALE_COUNT])
{
	unsigned newly = 0;
	size_t i;

	for (i = 0; i < TELLTALE_COUNT; i++) {
		unsigned bit = telltale_table[i].bit;
		int on = (commanded & bit) != 0;
		int match = (on && disp[i] == DISP_LIT) || (!on && disp[i] == DISP_DIM);

		if (match) {
			c->mismatch[i] = 0;
			continue;
		}
		if (c->mismatch[i] < DEBOUNCE_CYCLES)
			c->mismatch[i]++;
		if (c->mismatch[i] >= DEBOUNCE_CYCLES && (c->latched & bit) == 0) {
			c->latched |= bit;
			newly |= bit;
		}
	}
	return newly;
}

static const char *displayed_name(enum displayed d)
{
	switch (d) {
	case DISP_LIT:
		return "lit";
	case DISP_DIM:
		return "dim";
	default:
		return "unknown";
	}
}

#ifndef TELLTALE_MONITOR_HOST_TEST
/* ==== the program: Screen, shared memory, the cycle ==== */

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <screen/screen.h>

#ifndef TELLTALE_MONITOR_VERSION
#define TELLTALE_MONITOR_VERSION "unknown"
#endif

#define PERIOD_MS_DEFAULT 100
#define PERIOD_MS_MIN 50
#define PERIOD_MS_MAX 1000
#define STALE_NS 500000000LL		/* seq unchanged this long: gauge not updating */
#define OPEN_RETRY_NS 100000000LL	/* shared object: retry every 100 ms ... */
#define OPEN_RETRIES 100		/* ... for up to 10 s */
#define STATUS_INTERVAL_CYCLES 50u
#define MAX_DISPLAYS 16
#define OVERLAY_HEIGHT 80
#define OVERLAY_ZORDER 1000		/* above any application window's default of 0 */
#define BAND_COLOR 0xffc81414u		/* A R G B */
#define SIGN_COLOR 0xffffffffu
#define EXIT_USAGE 2

/* QNX defines TIMER_ABSTIME as an unsigned bit (0x80000000); the flags
 * argument of clock_nanosleep() is an int with that bit pattern. */
#define ABSTIME_FLAGS ((int)TIMER_ABSTIME)

/* The published word is shared between processes: its atomic operations
 * must not depend on a lock inside this process. */
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
	       "64-bit atomics are not lock-free");

struct options {
	int period_ms;
	int priority;
	int set_priority;	/* 1: --priority given */
};

/* The read-only mapping of the gauge's shared object. */
struct state_view {
	void *map;			/* NULL: not mapped; mapped PROT_READ */
	const _Atomic uint64_t *word;
};

/* Screen objects: one display-manager context; the capture pixmap and its
 * buffer; the overlay window and its buffer. */
struct screen_objs {
	screen_context_t ctx;
	screen_display_t disp;
	screen_pixmap_t pix;
	screen_buffer_t cap_buf;
	const unsigned char *cap_ptr;
	struct capture_geom geom;
	screen_window_t ovl;
	screen_buffer_t ovl_buf;
	int ovl_shown;
};

/* Counters reported in the status line. */
struct cycle_stats {
	unsigned long cycles;
	unsigned long skipped;
	unsigned long overruns;
	unsigned long faults;
	int64_t longest_ns;
	unsigned long capture_failures;
};

static void log_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Informational output to stdout, flushed per line. */
static void log_out(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	/* stdout carries status and fault lines; a failed write changes
	 * nothing the program does, so the results are not acted on */
	(void)vprintf(fmt, ap);
	va_end(ap);
	(void)fflush(stdout);
}

/* Error output to stderr. */
static void log_err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	/* stderr is the only error channel; if it fails, nothing else can report */
	(void)vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void fail_errno(const char *call)
{
	log_err("telltale-monitor: %s failed: %s\n", call, strerror(errno));
}

static void fail_code(const char *call, int code)
{
	log_err("telltale-monitor: %s failed: %s\n", call, strerror(code));
}

static int64_t now_ns(void)
{
	struct timespec ts;

	/* CLOCK_MONOTONIC is always present and ts is valid: cannot fail */
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static struct timespec ts_of(int64_t ns)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ns / 1000000000LL);
	ts.tv_nsec = (long)(ns % 1000000000LL);
	return ts;
}

/* ---- command line ---- */

static void usage(FILE *f, const char *argv0)
{
	/* usage text; nothing to do if the write fails */
	(void)fprintf(f,
		      "usage: %s [--period-ms N] [--priority N] [--help] [--version]\n"
		      "  --period-ms N  check every N ms, %d..%d (default %d)\n"
		      "  --priority N   run at SCHED_RR priority N (default: inherited);\n"
		      "                 recommended: above cluster-gauge's priority\n",
		      argv0, PERIOD_MS_MIN, PERIOD_MS_MAX, PERIOD_MS_DEFAULT);
}

/* A decimal int. */
static int parse_int(const char *s, int *out_i)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (end == s || *end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX)
		return -1;
	*out_i = (int)v;
	return 0;
}

enum args_result { ARGS_RUN, ARGS_DONE, ARGS_BAD };

static enum args_result parse_args(int argc, char **argv, struct options *o)
{
	int i;

	o->period_ms = PERIOD_MS_DEFAULT;
	o->priority = 0;
	o->set_priority = 0;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			usage(stdout, argv[0]);
			return ARGS_DONE;
		} else if (strcmp(argv[i], "--version") == 0) {
			log_out("telltale-monitor %s\n", TELLTALE_MONITOR_VERSION);
			return ARGS_DONE;
		} else if (strcmp(argv[i], "--period-ms") == 0 && i + 1 < argc) {
			if (parse_int(argv[++i], &o->period_ms) != 0 ||
			    o->period_ms < PERIOD_MS_MIN || o->period_ms > PERIOD_MS_MAX)
				return ARGS_BAD;
		} else if (strcmp(argv[i], "--priority") == 0 && i + 1 < argc) {
			/* whether the priority is valid is decided by the OS */
			if (parse_int(argv[++i], &o->priority) != 0)
				return ARGS_BAD;
			o->set_priority = 1;
		} else {
			return ARGS_BAD;
		}
	}
	return ARGS_RUN;
}

/* ---- signals and scheduling ---- */

/* Block SIGINT and SIGTERM in this thread; threads created later inherit
 * the mask. They are then received only through poll_signals(). */
static int block_signals(sigset_t *set)
{
	int rc;

	if (sigemptyset(set) != 0 || sigaddset(set, SIGINT) != 0 ||
	    sigaddset(set, SIGTERM) != 0) {
		fail_errno("sigemptyset/sigaddset");
		return -1;
	}
	rc = pthread_sigmask(SIG_BLOCK, set, NULL);
	if (rc != 0) {
		fail_code("pthread_sigmask", rc);
		return -1;
	}
	return 0;
}

/* The pending signal of set, 0 if none, -1 on error. Never blocks. */
static int poll_signals(const sigset_t *set)
{
	const struct timespec zero = { 0, 0 };
	int sig = sigtimedwait(set, NULL, &zero);

	if (sig > 0)
		return sig;
	if (errno == EAGAIN || errno == EINTR)
		return 0;
	fail_errno("sigtimedwait");
	return -1;
}

static const char *policy_name(int policy)
{
	switch (policy) {
	case SCHED_FIFO:
		return "SCHED_FIFO";
	case SCHED_RR:
		return "SCHED_RR";
	case SCHED_OTHER:
		return "SCHED_OTHER";
	default:
		return "policy";
	}
}

struct sched_info {
	int policy;
	int priority;
};

/* Set SCHED_RR at the given priority if requested; report the policy and
 * priority in effect into si. */
static int sched_setup(const struct options *o, struct sched_info *si)
{
	struct sched_param sp;
	int rc;

	if (o->set_priority) {
		memset(&sp, 0, sizeof(sp));
		sp.sched_priority = o->priority;
		rc = pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
		if (rc != 0) {
			log_err("telltale-monitor: pthread_setschedparam(SCHED_RR, %d) failed: %s\n",
				o->priority, strerror(rc));
			return -1;
		}
	}
	rc = pthread_getschedparam(pthread_self(), &si->policy, &sp);
	if (rc != 0) {
		fail_code("pthread_getschedparam", rc);
		return -1;
	}
	si->priority = sp.sched_priority;
	return 0;
}

/* ---- the gauge's commanded state ---- */

static void state_close(struct state_view *sv)
{
	if (sv->map == NULL)
		return;
	/* shutdown: nothing further to release if this fails */
	(void)munmap(sv->map, TELLTALE_STATE_SHM_SIZE);
	sv->map = NULL;
	sv->word = NULL;
}

/* Try once to open and map the shared object read-only. Returns 0 when
 * mapped, 1 when it does not exist yet or is not yet sized (try again),
 * -1 after printing the failing call. */
static int state_try_open(struct state_view *sv)
{
	struct stat st;
	void *m;
	int fd;

	fd = shm_open(TELLTALE_STATE_SHM_NAME, O_RDONLY, 0);
	if (fd < 0) {
		if (errno == ENOENT)
			return 1;
		fail_errno("shm_open(" TELLTALE_STATE_SHM_NAME ", O_RDONLY)");
		return -1;
	}
	if (fstat(fd, &st) != 0) {
		fail_errno("fstat(" TELLTALE_STATE_SHM_NAME ")");
		/* failure path: nothing further to release if close fails */
		(void)close(fd);
		return -1;
	}
	if (st.st_size < (off_t)TELLTALE_STATE_SHM_SIZE) {
		/* the gauge has created but not yet sized it; nothing further to
		 * release if close fails */
		(void)close(fd);
		return 1;
	}
	m = mmap(NULL, TELLTALE_STATE_SHM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		fail_errno("mmap(" TELLTALE_STATE_SHM_NAME ", PROT_READ)");
		/* failure path, as above */
		(void)close(fd);
		return -1;
	}
	/* the mapping stays valid without the descriptor; a failed close
	 * leaks one descriptor and changes nothing else */
	(void)close(fd);
	sv->map = m;
	sv->word = m;
	return 0;
}

/* Open the shared object read-only, retrying every 100 ms for up to 10 s
 * while the gauge starts. Returns 0 when mapped, 1 if a signal in sigs
 * arrived first, -1 after printing why (not found in time, or an error). */
static int state_open(struct state_view *sv, const sigset_t *sigs)
{
	int64_t next = now_ns();
	int i;

	sv->map = NULL;
	sv->word = NULL;
	for (i = 0; i < OPEN_RETRIES; i++) {
		struct timespec ts;
		int rc = state_try_open(sv);
		int sig;

		if (rc <= 0)
			return rc;
		sig = poll_signals(sigs);
		if (sig != 0)
			return sig > 0 ? 1 : -1;
		next += OPEN_RETRY_NS;
		ts = ts_of(next);
		rc = clock_nanosleep(CLOCK_MONOTONIC, ABSTIME_FLAGS, &ts, NULL);
		if (rc != 0 && rc != EINTR) {
			fail_code("clock_nanosleep", rc);
			return -1;
		}
	}
	log_err("telltale-monitor: shm_open(" TELLTALE_STATE_SHM_NAME
		") failed: not created by cluster-gauge within 10 s\n");
	return -1;
}

static uint64_t state_load(const struct state_view *sv)
{
	return atomic_load_explicit(sv->word, memory_order_acquire);
}

/* ---- Screen ---- */

static void screen_shutdown(struct screen_objs *so)
{
	/* shutdown: a failure here leaves nothing further to release */
	if (so->ovl)
		(void)screen_destroy_window(so->ovl);
	if (so->pix)
		(void)screen_destroy_pixmap(so->pix);
	if (so->ctx)
		(void)screen_destroy_context(so->ctx);
	memset(so, 0, sizeof(*so));
}

/* The capture pixmap: RGBA8888, the display's size, usable by
 * screen_read_display(). */
static int capture_setup(struct screen_objs *so, const int size[2])
{
	const int format = SCREEN_FORMAT_RGBA8888;
	const int usage_bits = SCREEN_USAGE_NATIVE | SCREEN_USAGE_READ;
	void *ptr = NULL;
	int stride = 0;

	if (screen_create_pixmap(&so->pix, so->ctx) != 0) {
		so->pix = NULL;
		fail_errno("screen_create_pixmap");
		return -1;
	}
	if (screen_set_pixmap_property_iv(so->pix, SCREEN_PROPERTY_FORMAT, &format) != 0) {
		fail_errno("screen_set_pixmap_property_iv(SCREEN_PROPERTY_FORMAT)");
		return -1;
	}
	if (screen_set_pixmap_property_iv(so->pix, SCREEN_PROPERTY_USAGE, &usage_bits) != 0) {
		fail_errno("screen_set_pixmap_property_iv(SCREEN_PROPERTY_USAGE)");
		return -1;
	}
	if (screen_set_pixmap_property_iv(so->pix, SCREEN_PROPERTY_BUFFER_SIZE, size) != 0) {
		fail_errno("screen_set_pixmap_property_iv(SCREEN_PROPERTY_BUFFER_SIZE)");
		return -1;
	}
	if (screen_create_pixmap_buffer(so->pix) != 0) {
		fail_errno("screen_create_pixmap_buffer");
		return -1;
	}
	if (screen_get_pixmap_property_pv(so->pix, SCREEN_PROPERTY_RENDER_BUFFERS,
					  (void **)&so->cap_buf) != 0) {
		fail_errno("screen_get_pixmap_property_pv(SCREEN_PROPERTY_RENDER_BUFFERS)");
		return -1;
	}
	if (screen_get_buffer_property_pv(so->cap_buf, SCREEN_PROPERTY_POINTER, &ptr) != 0 ||
	    ptr == NULL) {
		fail_errno("screen_get_buffer_property_pv(SCREEN_PROPERTY_POINTER)");
		return -1;
	}
	if (screen_get_buffer_property_iv(so->cap_buf, SCREEN_PROPERTY_STRIDE, &stride) != 0) {
		fail_errno("screen_get_buffer_property_iv(SCREEN_PROPERTY_STRIDE)");
		return -1;
	}
	if (stride < size[0] * 4) {
		log_err("telltale-monitor: capture buffer stride %d is less than %d\n",
			stride, size[0] * 4);
		return -1;
	}
	so->cap_ptr = ptr;
	so->geom = geom_for(size, (size_t)stride);
	return 0;
}

/* The overlay window: full display width, OVERLAY_HEIGHT high at the top,
 * above the gauge, hidden until overlay_show() posts it. */
static int overlay_setup(struct screen_objs *so, int width)
{
	const int format = SCREEN_FORMAT_RGBA8888;
	const int usage_bits = SCREEN_USAGE_WRITE | SCREEN_USAGE_NATIVE;
	const int size[2] = { width, OVERLAY_HEIGHT };
	const int pos[2] = { 0, 0 };
	const int zorder = OVERLAY_ZORDER;
	const int hidden = 0;
	static const char id_string[] = "telltale-monitor";

	if (screen_create_window(&so->ovl, so->ctx) != 0) {
		so->ovl = NULL;
		fail_errno("screen_create_window");
		return -1;
	}
	if (screen_set_window_property_iv(so->ovl, SCREEN_PROPERTY_FORMAT, &format) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_FORMAT)");
		return -1;
	}
	if (screen_set_window_property_iv(so->ovl, SCREEN_PROPERTY_USAGE, &usage_bits) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_USAGE)");
		return -1;
	}
	if (screen_set_window_property_cv(so->ovl, SCREEN_PROPERTY_ID_STRING,
					  (int)(sizeof(id_string) - 1u), id_string) != 0) {
		fail_errno("screen_set_window_property_cv(SCREEN_PROPERTY_ID_STRING)");
		return -1;
	}
	if (screen_set_window_property_iv(so->ovl, SCREEN_PROPERTY_SIZE, size) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_SIZE)");
		return -1;
	}
	if (screen_set_window_property_iv(so->ovl, SCREEN_PROPERTY_POSITION, pos) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_POSITION)");
		return -1;
	}
	if (screen_set_window_property_iv(so->ovl, SCREEN_PROPERTY_BUFFER_SIZE, size) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_BUFFER_SIZE)");
		return -1;
	}
	if (screen_set_window_property_iv(so->ovl, SCREEN_PROPERTY_ZORDER, &zorder) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_ZORDER)");
		return -1;
	}
	if (screen_set_window_property_iv(so->ovl, SCREEN_PROPERTY_VISIBLE, &hidden) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_VISIBLE)");
		return -1;
	}
	if (screen_create_window_buffers(so->ovl, 1) != 0) {
		fail_errno("screen_create_window_buffers");
		return -1;
	}
	if (screen_get_window_property_pv(so->ovl, SCREEN_PROPERTY_RENDER_BUFFERS,
					  (void **)&so->ovl_buf) != 0) {
		fail_errno("screen_get_window_property_pv(SCREEN_PROPERTY_RENDER_BUFFERS)");
		return -1;
	}
	if (screen_flush_context(so->ctx, 0) != 0) {
		fail_errno("screen_flush_context");
		return -1;
	}
	return 0;
}

/* Context, first display and its size, capture pixmap (the overlay window
 * is created only on a fault). On failure, print the call, release what was
 * created, return -1. */
static int screen_setup(struct screen_objs *so)
{
	screen_display_t disps[MAX_DISPLAYS];
	int ndisp = 0;
	int size[2] = { 0, 0 };

	memset(so, 0, sizeof(*so));
	if (screen_create_context(&so->ctx, SCREEN_DISPLAY_MANAGER_CONTEXT) != 0) {
		so->ctx = NULL;
		fail_errno("screen_create_context(SCREEN_DISPLAY_MANAGER_CONTEXT)");
		goto fail;
	}
	if (screen_get_context_property_iv(so->ctx, SCREEN_PROPERTY_DISPLAY_COUNT, &ndisp) != 0) {
		fail_errno("screen_get_context_property_iv(SCREEN_PROPERTY_DISPLAY_COUNT)");
		goto fail;
	}
	if (ndisp < 1 || ndisp > MAX_DISPLAYS) {
		log_err("telltale-monitor: %d displays; expected 1..%d\n", ndisp, MAX_DISPLAYS);
		goto fail;
	}
	if (screen_get_context_property_pv(so->ctx, SCREEN_PROPERTY_DISPLAYS,
					   (void **)disps) != 0) {
		fail_errno("screen_get_context_property_pv(SCREEN_PROPERTY_DISPLAYS)");
		goto fail;
	}
	so->disp = disps[0];
	if (screen_get_display_property_iv(so->disp, SCREEN_PROPERTY_SIZE, size) != 0) {
		fail_errno("screen_get_display_property_iv(SCREEN_PROPERTY_SIZE)");
		goto fail;
	}
	if (size[0] < 1 || size[1] < 1 || size[0] > 16384 || size[1] > 16384) {
		log_err("telltale-monitor: display size %dx%d out of range\n", size[0], size[1]);
		goto fail;
	}
	if (capture_setup(so, size) != 0)
		goto fail;
	return 0;

fail:
	screen_shutdown(so);
	return -1;
}

/* Fill rectangle (x, y, w, h) of the overlay buffer with colour argb. */
static int overlay_fill(struct screen_objs *so, const int rect[4], uint32_t argb)
{
	const int attribs[] = {
		SCREEN_BLIT_DESTINATION_X, rect[0],
		SCREEN_BLIT_DESTINATION_Y, rect[1],
		SCREEN_BLIT_DESTINATION_WIDTH, rect[2],
		SCREEN_BLIT_DESTINATION_HEIGHT, rect[3],
		SCREEN_BLIT_COLOR, (int)argb,
		SCREEN_BLIT_END
	};

	if (screen_fill(so->ctx, so->ovl_buf, attribs) != 0) {
		fail_errno("screen_fill");
		return -1;
	}
	return 0;
}

/* Create the overlay window, draw the warning (red band, white triangle of
 * stacked rectangles with a red exclamation mark), post it and make it
 * visible. Done once; later calls return at once. Returns 0, or -1 after
 * printing the failing call. */
static int overlay_show(struct screen_objs *so)
{
	const int w = so->geom.width;
	const int cx = w / 2;
	const int band[4] = { 0, 0, w, OVERLAY_HEIGHT };
	const int bar[4] = { cx - 3, 32, 6, 22 };
	const int dot[4] = { cx - 3, 58, 6, 6 };
	const int visible = 1;
	int row;

	if (so->ovl_shown)
		return 0;
	if (so->ovl == NULL && overlay_setup(so, w) != 0)
		return -1;
	if (overlay_fill(so, band, BAND_COLOR) != 0)
		return -1;
	/* Triangle: 16 rows of 4 px from y = 8, widening by 8 px a row. */
	for (row = 0; row < 16; row++) {
		const int half = 3 + row * 4;
		const int r[4] = { cx - half, 8 + row * 4, 2 * half, 4 };

		if (overlay_fill(so, r, SIGN_COLOR) != 0)
			return -1;
	}
	if (overlay_fill(so, bar, BAND_COLOR) != 0 || overlay_fill(so, dot, BAND_COLOR) != 0)
		return -1;
	if (screen_post_window(so->ovl, so->ovl_buf, 1, band, 0) != 0) {
		fail_errno("screen_post_window");
		return -1;
	}
	if (screen_set_window_property_iv(so->ovl, SCREEN_PROPERTY_VISIBLE, &visible) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_VISIBLE)");
		return -1;
	}
	if (screen_flush_context(so->ctx, 0) != 0) {
		fail_errno("screen_flush_context");
		return -1;
	}
	so->ovl_shown = 1;
	return 0;
}

/* ---- the cycle ---- */

enum stop_reason { STOP_SIGINT, STOP_SIGTERM, STOP_ERROR };

static const char *stop_name(enum stop_reason why)
{
	switch (why) {
	case STOP_SIGINT:
		return "SIGINT";
	case STOP_SIGTERM:
		return "SIGTERM";
	default:
		return "error";
	}
}

/* What the checks remember from cycle to cycle. */
struct monitor_state {
	struct comparison cmp;
	uint32_t last_seq;	/* seq seen last */
	int64_t t_seq_change;	/* when seq last changed, CLOCK_MONOTONIC ns */
	int fault_state;	/* latched by the first fault: checks stopped */
};

/* Enter the fault state: stop checking, show the overlay. A failure to
 * show the overlay is reported; the fault log lines already stand, so the
 * monitor stays in the fault state rather than exiting. */
static void fault_enter(struct screen_objs *so, struct monitor_state *ms)
{
	ms->fault_state = 1;
	if (overlay_show(so) == 0)
		log_out("telltale-monitor: fault state: checks stopped, overlay shown\n");
	else
		log_out("telltale-monitor: fault state: checks stopped, overlay NOT shown\n");
}

/* Report the telltale faults newly detected this cycle (TT_* bits in
 * newly) for the commanded word w, and enter the fault state if any. */
static void react(struct screen_objs *so, struct monitor_state *ms, unsigned newly,
		  const enum displayed disp[TELLTALE_COUNT], uint64_t w, struct cycle_stats *cs)
{
	unsigned commanded = telltale_state_bits(w);
	uint32_t t_ms = telltale_state_t_ms(w);
	size_t i;

	for (i = 0; i < TELLTALE_COUNT; i++) {
		const struct telltale_def *tt = &telltale_table[i];

		if ((newly & tt->bit) == 0)
			continue;
		log_out("telltale-monitor: FAULT %s commanded=%s displayed=%s t=%u\n",
			tt->name, (commanded & tt->bit) ? "on" : "off", displayed_name(disp[i]),
			(unsigned)t_ms);
		cs->faults++;
	}
	if (newly)
		fault_enter(so, ms);
}

/* The checks of one cycle; none in the fault state. Cannot fail: a failed
 * capture counts as unknown for every telltale. */
static void check_once(struct screen_objs *so, const struct state_view *sv,
		       struct monitor_state *ms, int64_t t_now, struct cycle_stats *cs)
{
	enum displayed disp[TELLTALE_COUNT];
	uint64_t w = state_load(sv), w2;
	uint32_t seq = telltale_state_seq(w);
	uint32_t t_ms = telltale_state_t_ms(w);
	unsigned bits = telltale_state_bits(w);
	const unsigned char *img = so->cap_ptr;
	size_t i;

	if (ms->fault_state)
		return;

	/* 1. Is the gauge still publishing? */
	if (seq != ms->last_seq) {
		ms->last_seq = seq;
		ms->t_seq_change = t_now;
	} else if (t_now - ms->t_seq_change >= STALE_NS) {
		cs->faults++;
		log_out("telltale-monitor: FAULT gauge not updating seq=%u t=%u\n",
			(unsigned)seq, (unsigned)t_ms);
		fault_enter(so, ms);
		return;
	}
	if ((bits & ~TT_ALL) != 0u) {
		cs->faults++;
		log_out("telltale-monitor: FAULT commanded state invalid bits=0x%04x t=%u\n",
			bits, (unsigned)t_ms);
		fault_enter(so, ms);
		return;
	}

	/* 2. Capture the composed display. A failed capture counts as
	 * unknown for every telltale. */
	if (screen_read_display(so->disp, so->cap_buf, 0, NULL, 0) != 0) {
		if (cs->capture_failures++ == 0)
			fail_errno("screen_read_display (further failures are counted)");
		img = NULL;
	}

	/* 3. Skip the comparison if nothing is published yet, or if the
	 * commanded bits changed during the capture. */
	w2 = state_load(sv);
	if (seq == 0u || telltale_state_bits(w2) != bits) {
		cs->skipped++;
		return;
	}

	/* 4.-6. Classify, compare, react. */
	for (i = 0; i < TELLTALE_COUNT; i++)
		disp[i] = classify(img, &so->geom, &telltale_table[i]);
	react(so, ms, compare_cycle(&ms->cmp, bits, disp), disp, w, cs);
}

static void status_line(const struct cycle_stats *cs)
{
	log_out("telltale-monitor: cycles %lu, skipped %lu, overruns %lu, longest %.1f ms, "
		"faults %lu\n", cs->cycles, cs->skipped, cs->overruns,
		(double)cs->longest_ns / 1e6, cs->faults);
}

static enum stop_reason run(struct screen_objs *so, const struct state_view *sv,
			    const struct options *o, const sigset_t *sigs, struct cycle_stats *cs)
{
	const int64_t period = (int64_t)o->period_ms * 1000000LL;
	struct monitor_state ms;
	int64_t next = now_ns();

	memset(&ms, 0, sizeof(ms));
	ms.last_seq = telltale_state_seq(state_load(sv));
	ms.t_seq_change = next;
	for (;;) {
		struct timespec ts = ts_of(next);
		int64_t t_start, t_end;
		int rc, sig;

		rc = clock_nanosleep(CLOCK_MONOTONIC, ABSTIME_FLAGS, &ts, NULL);
		if (rc != 0 && rc != EINTR) {
			fail_code("clock_nanosleep", rc);
			return STOP_ERROR;
		}
		t_start = now_ns();
		sig = poll_signals(sigs);
		if (sig < 0)
			return STOP_ERROR;
		if (sig == SIGINT)
			return STOP_SIGINT;
		if (sig == SIGTERM)
			return STOP_SIGTERM;

		check_once(so, sv, &ms, t_start, cs);

		t_end = now_ns();
		if (t_end - t_start > cs->longest_ns)
			cs->longest_ns = t_end - t_start;
		if (t_end - t_start > period)
			cs->overruns++;
		if (++cs->cycles % STATUS_INTERVAL_CYCLES == 0)
			status_line(cs);

		next += period;
		if (next <= t_end)
			next = t_end + period;	/* behind: resume on time, do not catch up */
	}
}

int main(int argc, char **argv)
{
	struct options opt;
	struct sched_info si = { 0, 0 };
	struct state_view sv;
	struct screen_objs so;
	struct cycle_stats cs;
	sigset_t sigs;
	enum stop_reason why;
	int mlock_rc, mlock_errno = 0, rc;

	switch (parse_args(argc, argv, &opt)) {
	case ARGS_DONE:
		return 0;
	case ARGS_BAD:
		usage(stderr, argv[0]);
		return EXIT_USAGE;
	default:
		break;
	}

	if (block_signals(&sigs) != 0)
		return 1;
	if (sched_setup(&opt, &si) != 0)
		return 1;
	rc = state_open(&sv, &sigs);
	if (rc != 0)
		return rc > 0 ? 0 : 1;
	if (screen_setup(&so) != 0) {
		state_close(&sv);
		return 1;
	}

	mlock_rc = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (mlock_rc != 0)
		mlock_errno = errno;

	log_out("telltale-monitor %s: display %dx%d, period %d ms, %s priority %d (%s), "
		"mlockall %s%s, reading %s\n",
		TELLTALE_MONITOR_VERSION, so.geom.width, so.geom.height, opt.period_ms,
		policy_name(si.policy), si.priority,
		opt.set_priority ? "set by --priority" : "inherited",
		mlock_rc == 0 ? "ok" : "failed: ", mlock_rc == 0 ? "" : strerror(mlock_errno),
		TELLTALE_STATE_SHM_NAME);

	memset(&cs, 0, sizeof(cs));
	why = run(&so, &sv, &opt, &sigs, &cs);

	screen_shutdown(&so);
	state_close(&sv);
	log_out("telltale-monitor: stopped (%s): cycles %lu, skipped %lu, overruns %lu, "
		"longest %.1f ms, faults %lu\n", stop_name(why), cs.cycles, cs.skipped,
		cs.overruns, (double)cs.longest_ns / 1e6, cs.faults);
	return why == STOP_ERROR ? 1 : 0;
}

#else
/* ==== host test of the classification and comparison ==== */

#define TEST_W 640
#define TEST_H 400	/* 1280x720 layout scaled by 0.5, centred: 20 px bands */

static void expect(int *failures, int cond, const char *what)
{
	if (!cond) {
		printf("FAIL: %s\n", what);
		(*failures)++;
	}
}

static void put(unsigned char *img, const struct capture_geom *g, struct telltale_point p,
		struct telltale_rgb c)
{
	size_t x = (size_t)(g->off_x + p.x * g->scale);
	size_t y = (size_t)(g->off_y + p.y * g->scale);
	unsigned char *px = img + y * g->stride + x * 4u;

	px[0] = c.b;
	px[1] = c.g;
	px[2] = c.r;
	px[3] = 0xff;
}

/* Paint every telltale's sample points as commanded by bits. */
static void paint(unsigned char *img, const struct capture_geom *g, unsigned bits)
{
	size_t i, j;

	memset(img, 0, g->stride * (size_t)g->height);
	for (i = 0; i < TELLTALE_COUNT; i++) {
		const struct telltale_def *tt = &telltale_table[i];

		for (j = 0; j < tt->n_samples; j++)
			put(img, g, tt->samples[j], (bits & tt->bit) ? tt->on : telltale_off);
	}
}

static unsigned cycle(struct comparison *c, const unsigned char *img,
		      const struct capture_geom *g, unsigned commanded)
{
	enum displayed d[TELLTALE_COUNT];
	size_t i;

	for (i = 0; i < TELLTALE_COUNT; i++)
		d[i] = classify(img, g, &telltale_table[i]);
	return compare_cycle(c, commanded, d);
}

int main(void)
{
	unsigned char img[TEST_W * TEST_H * 4];
	int failures = 0;
	const int size[2] = { TEST_W, TEST_H };
	const struct capture_geom g = geom_for(size, TEST_W * 4u);
	struct comparison c;
	struct telltale_rgb near_on;
	size_t i;
	int k;

	expect(&failures, g.scale == 0.5f && g.off_x == 0.0f && g.off_y == 20.0f, "geometry 640x400");

	/* Classification */
	paint(img, &g, TT_ALL);
	for (i = 0; i < TELLTALE_COUNT; i++)
		expect(&failures, classify(img, &g, &telltale_table[i]) == DISP_LIT, "all on: lit");
	paint(img, &g, 0);
	for (i = 0; i < TELLTALE_COUNT; i++)
		expect(&failures, classify(img, &g, &telltale_table[i]) == DISP_DIM, "all off: dim");
	expect(&failures, classify(NULL, &g, &telltale_table[0]) == DISP_UNKNOWN, "no capture: unknown");
	/* One sample point of one telltale lit, the others dim: unknown */
	put(img, &g, telltale_table[2].samples[0], telltale_table[2].on);
	expect(&failures, classify(img, &g, &telltale_table[2]) == DISP_UNKNOWN, "mixed: unknown");
	/* Within tolerance and just outside */
	near_on = telltale_table[1].on;
	near_on.g = (unsigned char)(near_on.g + TOLERANCE);
	paint(img, &g, TT_ALL);
	put(img, &g, telltale_table[1].samples[0], near_on);
	expect(&failures, classify(img, &g, &telltale_table[1]) == DISP_LIT, "delta = tolerance: lit");
	near_on.g = (unsigned char)(near_on.g + 1);
	put(img, &g, telltale_table[1].samples[0], near_on);
	expect(&failures, classify(img, &g, &telltale_table[1]) == DISP_UNKNOWN, "delta = tolerance + 1: unknown");

	/* Black (blank display) and the clear colour are neither on nor off */
	memset(img, 0, sizeof(img));
	for (i = 0; i < TELLTALE_COUNT; i++)
		expect(&failures, classify(img, &g, &telltale_table[i]) == DISP_UNKNOWN,
		       "black: unknown");
	for (i = 0; i < TELLTALE_COUNT; i++) {
		size_t j;

		for (j = 0; j < telltale_table[i].n_samples; j++)
			put(img, &g, telltale_table[i].samples[j], (struct telltale_rgb){ 5, 8, 13 });
		expect(&failures, classify(img, &g, &telltale_table[i]) == DISP_UNKNOWN,
		       "clear colour: unknown");
	}
	/* Unknown mismatches both commanded states */
	memset(&c, 0, sizeof(c));
	memset(img, 0, sizeof(img));
	for (k = 0; k < 2; k++)
		expect(&failures, cycle(&c, img, &g, 0) == 0, "blank, all off: debouncing");
	expect(&failures, cycle(&c, img, &g, 0) == TT_ALL, "blank, all off: all fault");

	/* Comparison: healthy */
	memset(&c, 0, sizeof(c));
	paint(img, &g, TT_SEATBELT);
	for (k = 0; k < 10; k++)
		expect(&failures, cycle(&c, img, &g, TT_SEATBELT) == 0, "healthy: no fault");

	/* Two mismatching cycles, then a match: no fault (debounce resets) */
	paint(img, &g, TT_SEATBELT | TT_HIGH_BEAM);
	expect(&failures, cycle(&c, img, &g, TT_SEATBELT) == 0, "mismatch 1: no fault");
	expect(&failures, cycle(&c, img, &g, TT_SEATBELT) == 0, "mismatch 2: no fault");
	paint(img, &g, TT_SEATBELT);
	expect(&failures, cycle(&c, img, &g, TT_SEATBELT) == 0, "match: reset");
	paint(img, &g, TT_SEATBELT | TT_HIGH_BEAM);
	expect(&failures, cycle(&c, img, &g, TT_SEATBELT) == 0, "mismatch 1 again: no fault");
	expect(&failures, cycle(&c, img, &g, TT_SEATBELT) == 0, "mismatch 2 again: no fault");

	/* Third consecutive mismatch: fault for high beam only, reported once */
	expect(&failures, cycle(&c, img, &g, TT_SEATBELT) == TT_HIGH_BEAM, "mismatch 3: high beam fault");
	expect(&failures, cycle(&c, img, &g, TT_SEATBELT) == 0, "latched: reported once");

	/* Commanded on, drawn dim: fault */
	memset(&c, 0, sizeof(c));
	paint(img, &g, 0);
	for (k = 0; k < 2; k++)
		expect(&failures, cycle(&c, img, &g, TT_SEATBELT) == 0, "seat belt dim: debouncing");
	expect(&failures, cycle(&c, img, &g, TT_SEATBELT) == TT_SEATBELT, "seat belt dim: fault");

	/* Shared word */
	expect(&failures, telltale_state_seq(telltale_state_pack(7u, 70000u, TT_ALL | 0x100u)) == 7u,
	       "pack: seq");
	expect(&failures, telltale_state_t_ms(telltale_state_pack(7u, 70000u, TT_ALL)) == 70000u % 65536u,
	       "pack: t_ms modulo 65536");
	expect(&failures, telltale_state_bits(telltale_state_pack(7u, 0u, TT_ALL | 0x100u)) == TT_ALL,
	       "pack: only TT_ALL bits");
	expect(&failures, strcmp(displayed_name(DISP_DIM), "dim") == 0, "names");

	printf("%s (%d failures)\n", failures ? "monitor host test FAIL" : "monitor host test PASS",
	       failures);
	return failures ? 1 : 0;
}
#endif
