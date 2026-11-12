/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * cluster-gauge: platform layer. Owns the Screen window, the EGL context
 * and the frame loop.
 *
 * State and threads: one thread runs all of the program. struct platform
 * lives on main's stack; struct renderer is a block-scope static in main
 * (64 KB kept off the stack) reached only through the pointer main passes.
 * SIGINT and SIGTERM are blocked before Screen or EGL start (threads they
 * create inherit the mask) and polled once per frame with sigtimedwait();
 * there is no signal handler. Screen events are drained once per frame
 * without blocking.
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <screen/screen.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "drive_cycle.h"
#include "render.h"

#ifndef CLUSTER_GAUGE_VERSION
#define CLUSTER_GAUGE_VERSION "unknown"
#endif

#define FPS_INTERVAL_FRAMES 300
#define FRAME_BUDGET_S (1.0 / 60.0)	/* one display period at 60 Hz */
#define OVERRUN_S (2.0 * FRAME_BUDGET_S)
#define MAX_EVENTS_PER_FRAME 64
#define MAX_EGL_CONFIGS 256

#define EXIT_USAGE 2

struct platform {
	screen_context_t ctx;
	screen_window_t win;
	screen_event_t ev;
	EGLDisplay dpy;
	EGLContext egl_ctx;
	EGLSurface surf;
	int width;
	int height;
};

struct options {
	double limit_s;		/* 0: run until stopped */
	int priority;
	int set_priority;	/* 1: --priority given */
};

enum stop_reason { STOP_SECONDS, STOP_SIGINT, STOP_SIGTERM, STOP_CLOSE, STOP_ERROR };

static void log_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Informational output to stdout, flushed per line. */
static void log_out(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	/* stdout carries status lines only; a failed write changes nothing the
	 * program does, so the results are not acted on */
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
	log_err("cluster-gauge: %s failed: %s\n", call, strerror(errno));
}

static void fail_code(const char *call, int code)
{
	log_err("cluster-gauge: %s failed: %s\n", call, strerror(code));
}

static void fail_egl(const char *call)
{
	log_err("cluster-gauge: %s failed: EGL error 0x%04x\n", call, (unsigned)eglGetError());
}

static double now_s(void)
{
	struct timespec ts;

	/* CLOCK_MONOTONIC is always present and ts is valid: cannot fail */
	(void)clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- command line ---- */

static void usage(FILE *f, const char *argv0)
{
	/* usage text; nothing to do if the write fails */
	(void)fprintf(f,
		      "usage: %s [--seconds N] [--priority N] [--help] [--version]\n"
		      "  --seconds N   exit with status 0 after N seconds (N finite, > 0)\n"
		      "  --priority N  run at SCHED_RR priority N (default: inherited)\n",
		      argv0);
}

/* A finite positive number of seconds, no range error. */
static int parse_seconds(const char *s, double *out_s)
{
	char *end;
	double v;

	errno = 0;
	v = strtod(s, &end);
	if (end == s || *end != '\0' || errno == ERANGE || !isfinite(v) || v <= 0.0)
		return -1;
	*out_s = v;
	return 0;
}

/* A decimal int. Whether the priority is valid is decided by the OS. */
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

	o->limit_s = 0.0;
	o->priority = 0;
	o->set_priority = 0;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0) {
			usage(stdout, argv[0]);
			return ARGS_DONE;
		} else if (strcmp(argv[i], "--version") == 0) {
			log_out("cluster-gauge %s\n", CLUSTER_GAUGE_VERSION);
			return ARGS_DONE;
		} else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
			if (parse_seconds(argv[++i], &o->limit_s) != 0)
				return ARGS_BAD;
		} else if (strcmp(argv[i], "--priority") == 0 && i + 1 < argc) {
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

/* Scheduling policy and priority in effect. */
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
			log_err("cluster-gauge: pthread_setschedparam(SCHED_RR, %d) failed: %s\n",
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

/* ---- Screen and EGL ---- */

static void platform_shutdown(struct platform *p)
{
	/* Shutdown: a failure here leaves nothing further to release, so the
	 * results are not acted on. */
	if (p->dpy != EGL_NO_DISPLAY) {
		(void)eglMakeCurrent(p->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (p->surf != EGL_NO_SURFACE)
			(void)eglDestroySurface(p->dpy, p->surf);
		if (p->egl_ctx != EGL_NO_CONTEXT)
			(void)eglDestroyContext(p->dpy, p->egl_ctx);
		(void)eglTerminate(p->dpy);
	}
	if (p->ev)
		(void)screen_destroy_event(p->ev);
	if (p->win)
		(void)screen_destroy_window(p->win);
	if (p->ctx)
		(void)screen_destroy_context(p->ctx);
	p->surf = EGL_NO_SURFACE;
	p->egl_ctx = EGL_NO_CONTEXT;
	p->dpy = EGL_NO_DISPLAY;
	p->ev = NULL;
	p->win = NULL;
	p->ctx = NULL;
}

/* Pick the first window-capable OpenGL ES 2 config whose native visual is
 * the window's Screen format. eglChooseConfig() is not used: for 8-bit
 * colour sizes it can return a 10-bit config first, and a window buffer in
 * that format cannot be allocated on the virtio GPU. */
static int choose_config(EGLDisplay dpy, int format, EGLConfig *cfg_out)
{
	EGLConfig cfgs[MAX_EGL_CONFIGS];
	EGLint n = 0;
	EGLint i;

	if (!eglGetConfigs(dpy, cfgs, MAX_EGL_CONFIGS, &n) || n < 1) {
		fail_egl("eglGetConfigs");
		return -1;
	}
	if (n > MAX_EGL_CONFIGS)
		n = MAX_EGL_CONFIGS;
	for (i = 0; i < n; i++) {
		EGLint surface = 0, renderable = 0, visual = 0;

		if (!eglGetConfigAttrib(dpy, cfgs[i], EGL_SURFACE_TYPE, &surface) ||
		    !eglGetConfigAttrib(dpy, cfgs[i], EGL_RENDERABLE_TYPE, &renderable) ||
		    !eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &visual)) {
			fail_egl("eglGetConfigAttrib");
			return -1;
		}
		if ((surface & EGL_WINDOW_BIT) && (renderable & EGL_OPENGL_ES2_BIT) &&
		    visual == format) {
			*cfg_out = cfgs[i];
			return 0;
		}
	}
	log_err("cluster-gauge: eglGetConfigs: no OpenGL ES 2 window config "
		"with native visual %d\n", format);
	return -1;
}

/* Initialisation in the order the design fixes. On any failure, print the
 * call, release what was created, and return -1. */
static int platform_init(struct platform *p)
{
	const int usage_bits = SCREEN_USAGE_OPENGL_ES2;
	const int format = SCREEN_FORMAT_RGBX8888;
	const int swap_interval = 1;
	static const char id_string[] = "cluster-gauge";
	static const EGLint ctx_attrs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLConfig cfg;
	int size[2];

	memset(p, 0, sizeof(*p));
	p->dpy = EGL_NO_DISPLAY;
	p->egl_ctx = EGL_NO_CONTEXT;
	p->surf = EGL_NO_SURFACE;

	/* 1. Screen context */
	if (screen_create_context(&p->ctx, SCREEN_APPLICATION_CONTEXT) != 0) {
		p->ctx = NULL;
		fail_errno("screen_create_context");
		goto fail;
	}

	/* 2. Screen window and its properties */
	if (screen_create_window(&p->win, p->ctx) != 0) {
		p->win = NULL;
		fail_errno("screen_create_window");
		goto fail;
	}
	if (screen_set_window_property_iv(p->win, SCREEN_PROPERTY_USAGE, &usage_bits) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_USAGE)");
		goto fail;
	}
	if (screen_set_window_property_iv(p->win, SCREEN_PROPERTY_FORMAT, &format) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_FORMAT)");
		goto fail;
	}
	if (screen_set_window_property_iv(p->win, SCREEN_PROPERTY_SWAP_INTERVAL,
					  &swap_interval) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_SWAP_INTERVAL)");
		goto fail;
	}
	if (screen_set_window_property_cv(p->win, SCREEN_PROPERTY_ID_STRING,
					  (int)(sizeof(id_string) - 1u), id_string) != 0) {
		fail_errno("screen_set_window_property_cv(SCREEN_PROPERTY_ID_STRING)");
		goto fail;
	}
	if (screen_create_window_buffers(p->win, 2) != 0) {
		fail_errno("screen_create_window_buffers");
		goto fail;
	}

	/* 3. EGL */
	p->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (p->dpy == EGL_NO_DISPLAY) {
		fail_egl("eglGetDisplay");
		goto fail;
	}
	if (!eglInitialize(p->dpy, NULL, NULL)) {
		fail_egl("eglInitialize");
		p->dpy = EGL_NO_DISPLAY;
		goto fail;
	}
	if (choose_config(p->dpy, format, &cfg) != 0)
		goto fail;
	p->egl_ctx = eglCreateContext(p->dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
	if (p->egl_ctx == EGL_NO_CONTEXT) {
		fail_egl("eglCreateContext");
		goto fail;
	}
	p->surf = eglCreateWindowSurface(p->dpy, cfg, (EGLNativeWindowType)p->win, NULL);
	if (p->surf == EGL_NO_SURFACE) {
		fail_egl("eglCreateWindowSurface");
		goto fail;
	}
	if (!eglMakeCurrent(p->dpy, p->surf, p->surf, p->egl_ctx)) {
		fail_egl("eglMakeCurrent");
		goto fail;
	}

	/* 4. Viewport from the buffer size */
	if (screen_get_window_property_iv(p->win, SCREEN_PROPERTY_BUFFER_SIZE, size) != 0) {
		fail_errno("screen_get_window_property_iv(SCREEN_PROPERTY_BUFFER_SIZE)");
		goto fail;
	}
	p->width = size[0];
	p->height = size[1];
	glViewport(0, 0, p->width, p->height);

	/* The event object the frame loop drains into */
	if (screen_create_event(&p->ev) != 0) {
		p->ev = NULL;
		fail_errno("screen_create_event");
		goto fail;
	}
	return 0;

fail:
	platform_shutdown(p);
	return -1;
}

/* Drain pending Screen events without blocking, at most
 * MAX_EVENTS_PER_FRAME per call. Returns 1 if a close event was seen, 0 if
 * not, -1 on error. Other events are discarded. */
static int platform_drain_events(struct platform *p)
{
	int close_seen = 0;
	int n;

	for (n = 0; n < MAX_EVENTS_PER_FRAME; n++) {
		int type = SCREEN_EVENT_NONE;

		if (screen_get_event(p->ctx, p->ev, 0) != 0) {
			fail_errno("screen_get_event");
			return -1;
		}
		if (screen_get_event_property_iv(p->ev, SCREEN_PROPERTY_TYPE, &type) != 0) {
			fail_errno("screen_get_event_property_iv(SCREEN_PROPERTY_TYPE)");
			return -1;
		}
		if (type == SCREEN_EVENT_NONE)
			break;
		if (type == SCREEN_EVENT_CLOSE)
			close_seen = 1;
	}
	return close_seen;
}

/* ---- frame loop ---- */

struct frame_stats {
	unsigned long frames;
	unsigned long overruns;
	double longest_s;	/* longest frame in the current FPS interval */
	double longest_total_s;
};

static const char *stop_name(enum stop_reason why)
{
	switch (why) {
	case STOP_SECONDS:
		return "--seconds elapsed";
	case STOP_SIGINT:
		return "SIGINT";
	case STOP_SIGTERM:
		return "SIGTERM";
	case STOP_CLOSE:
		return "Screen close event";
	default:
		return "error";
	}
}

static enum stop_reason run(struct platform *p, struct renderer *r, const struct options *o,
			    const sigset_t *sigs, struct frame_stats *fs)
{
	double t0 = now_s();
	double t_prev = t0, t_fps = t0;

	for (;;) {
		double t = now_s();
		double dt = t - t_prev;
		struct gauge_state st;
		size_t dropped;
		int sig, ev;

		t_prev = t;
		if (dt > fs->longest_s)
			fs->longest_s = dt;
		if (dt > fs->longest_total_s)
			fs->longest_total_s = dt;
		if (dt > OVERRUN_S)
			fs->overruns++;

		sig = poll_signals(sigs);
		if (sig < 0)
			return STOP_ERROR;
		if (sig == SIGINT)
			return STOP_SIGINT;
		if (sig == SIGTERM)
			return STOP_SIGTERM;
		ev = platform_drain_events(p);
		if (ev < 0)
			return STOP_ERROR;
		if (ev > 0)
			return STOP_CLOSE;
		if (o->limit_s > 0.0 && t - t0 >= o->limit_s)
			return STOP_SECONDS;

		drive_cycle_state(t - t0, &st);
		dropped = render_frame(r, &st);
		if (dropped > 0)
			log_err("cluster-gauge: frame %lu: vertex buffer full, "
				"%zu vertices dropped\n", fs->frames + 1u, dropped);

		if (!eglSwapBuffers(p->dpy, p->surf)) {
			fail_egl("eglSwapBuffers");
			return STOP_ERROR;
		}

		if (++fs->frames % FPS_INTERVAL_FRAMES == 0) {
			double tn = now_s();

			log_out("cluster-gauge: frames %lu, %.1f fps, overruns %lu, "
				"longest frame %.1f ms\n", fs->frames,
				FPS_INTERVAL_FRAMES / (tn - t_fps), fs->overruns,
				fs->longest_s * 1e3);
			t_fps = tn;
			fs->longest_s = 0.0;
		}
	}
}

int main(int argc, char **argv)
{
	/* The renderer: 64 KB of vertex buffer kept off the main thread's
	 * stack. Written only through the pointer passed below. */
	static struct renderer renderer;
	struct platform p;
	struct options opt;
	struct frame_stats fs = { 0, 0, 0.0, 0.0 };
	struct render_viewport vp;
	sigset_t sigs;
	enum stop_reason why;
	struct sched_info si = { 0, 0 };
	int mlock_rc, mlock_errno = 0;

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

	if (platform_init(&p) != 0)
		return 1;
	vp.width = p.width;
	vp.height = p.height;
	if (render_init(&renderer, vp) != 0) {
		platform_shutdown(&p);
		return 1;
	}

	mlock_rc = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (mlock_rc != 0)
		mlock_errno = errno;

	log_out("cluster-gauge %s: %dx%d, %s priority %d (%s), mlockall %s%s\n",
	    CLUSTER_GAUGE_VERSION, p.width, p.height, policy_name(si.policy), si.priority,
	    opt.set_priority ? "set by --priority" : "inherited",
	    mlock_rc == 0 ? "ok" : "failed: ", mlock_rc == 0 ? "" : strerror(mlock_errno));

	why = run(&p, &renderer, &opt, &sigs, &fs);

	render_shutdown(&renderer);
	platform_shutdown(&p);
	log_out("cluster-gauge: stopped (%s): frames %lu, overruns %lu, longest frame %.1f ms\n",
	    stop_name(why), fs.frames, fs.overruns, fs.longest_total_s * 1e3);
	return why == STOP_ERROR ? 1 : 0;
}
