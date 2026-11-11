/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 *
 * cluster-gauge: platform layer. Owns the Screen window, the EGL context
 * and the frame loop.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <screen/screen.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define FPS_INTERVAL_FRAMES 300

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

struct platform {
	screen_context_t ctx;
	screen_window_t win;
	EGLDisplay dpy;
	EGLContext egl_ctx;
	EGLSurface surf;
	int width;
	int height;
};

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void fail_errno(const char *call)
{
	fprintf(stderr, "cluster-gauge: %s failed: %s\n", call, strerror(errno));
}

static void fail_egl(const char *call)
{
	fprintf(stderr, "cluster-gauge: %s failed: EGL error 0x%04x\n", call,
		(unsigned)eglGetError());
}

static void platform_shutdown(struct platform *p)
{
	if (p->dpy != EGL_NO_DISPLAY) {
		eglMakeCurrent(p->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (p->surf != EGL_NO_SURFACE)
			eglDestroySurface(p->dpy, p->surf);
		if (p->egl_ctx != EGL_NO_CONTEXT)
			eglDestroyContext(p->dpy, p->egl_ctx);
		eglTerminate(p->dpy);
	}
	if (p->win)
		screen_destroy_window(p->win);
	if (p->ctx)
		screen_destroy_context(p->ctx);
	p->surf = EGL_NO_SURFACE;
	p->egl_ctx = EGL_NO_CONTEXT;
	p->dpy = EGL_NO_DISPLAY;
	p->win = NULL;
	p->ctx = NULL;
}

#define MAX_EGL_CONFIGS 256

/* Pick the first window-capable OpenGL ES 2 config whose native visual is
 * the window's Screen format. eglChooseConfig() is not used: for 8-bit
 * colour sizes it can return a 10-bit config first, and a window buffer in
 * that format cannot be allocated on the virtio GPU. */
static int choose_config(EGLDisplay dpy, int format, EGLConfig *out)
{
	EGLConfig cfgs[MAX_EGL_CONFIGS];
	EGLint n = 0;
	EGLint i;

	if (!eglGetConfigs(dpy, cfgs, MAX_EGL_CONFIGS, &n) || n < 1) {
		fail_egl("eglGetConfigs");
		return -1;
	}
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
			*out = cfgs[i];
			return 0;
		}
	}
	fprintf(stderr, "cluster-gauge: eglGetConfigs: no OpenGL ES 2 window config "
		"with native visual %d\n", format);
	return -1;
}

/* Initialisation in the order the design fixes. On any failure, print the
 * call, release what was created, and return -1. */
static int platform_init(struct platform *p)
{
	const int usage = SCREEN_USAGE_OPENGL_ES2;
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
		goto err;
	}

	/* 2. Screen window and its properties */
	if (screen_create_window(&p->win, p->ctx) != 0) {
		p->win = NULL;
		fail_errno("screen_create_window");
		goto err;
	}
	if (screen_set_window_property_iv(p->win, SCREEN_PROPERTY_USAGE, &usage) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_USAGE)");
		goto err;
	}
	if (screen_set_window_property_iv(p->win, SCREEN_PROPERTY_FORMAT, &format) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_FORMAT)");
		goto err;
	}
	if (screen_set_window_property_iv(p->win, SCREEN_PROPERTY_SWAP_INTERVAL,
					  &swap_interval) != 0) {
		fail_errno("screen_set_window_property_iv(SCREEN_PROPERTY_SWAP_INTERVAL)");
		goto err;
	}
	if (screen_set_window_property_cv(p->win, SCREEN_PROPERTY_ID_STRING,
					  (int)strlen(id_string), id_string) != 0) {
		fail_errno("screen_set_window_property_cv(SCREEN_PROPERTY_ID_STRING)");
		goto err;
	}
	if (screen_create_window_buffers(p->win, 2) != 0) {
		fail_errno("screen_create_window_buffers");
		goto err;
	}

	/* 3. EGL */
	p->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (p->dpy == EGL_NO_DISPLAY) {
		fail_egl("eglGetDisplay");
		goto err;
	}
	if (!eglInitialize(p->dpy, NULL, NULL)) {
		fail_egl("eglInitialize");
		p->dpy = EGL_NO_DISPLAY;
		goto err;
	}
	if (choose_config(p->dpy, format, &cfg) != 0)
		goto err;
	p->egl_ctx = eglCreateContext(p->dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
	if (p->egl_ctx == EGL_NO_CONTEXT) {
		fail_egl("eglCreateContext");
		goto err;
	}
	p->surf = eglCreateWindowSurface(p->dpy, cfg, (EGLNativeWindowType)p->win, NULL);
	if (p->surf == EGL_NO_SURFACE) {
		fail_egl("eglCreateWindowSurface");
		goto err;
	}
	if (!eglMakeCurrent(p->dpy, p->surf, p->surf, p->egl_ctx)) {
		fail_egl("eglMakeCurrent");
		goto err;
	}

	/* 4. Viewport from the buffer size */
	if (screen_get_window_property_iv(p->win, SCREEN_PROPERTY_BUFFER_SIZE, size) != 0) {
		fail_errno("screen_get_window_property_iv(SCREEN_PROPERTY_BUFFER_SIZE)");
		goto err;
	}
	p->width = size[0];
	p->height = size[1];
	glViewport(0, 0, p->width, p->height);
	return 0;

err:
	platform_shutdown(p);
	return -1;
}

static void usage_exit(const char *argv0)
{
	fprintf(stderr, "usage: %s [--seconds N]\n", argv0);
	exit(2);
}

int main(int argc, char **argv)
{
	struct platform p;
	struct sigaction sa;
	double limit_s = 0.0;
	double t0, t_fps;
	unsigned long frame = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
			char *end;

			limit_s = strtod(argv[++i], &end);
			if (*end != '\0' || limit_s <= 0.0)
				usage_exit(argv[0]);
		} else {
			usage_exit(argv[0]);
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (platform_init(&p) != 0)
		return 1;

	printf("cluster-gauge: %dx%d\n", p.width, p.height);
	fflush(stdout);

	t0 = now_s();
	t_fps = t0;
	while (!stop_requested) {
		double t = now_s();

		if (limit_s > 0.0 && t - t0 >= limit_s)
			break;

		/* Skeleton: one solid colour per frame. */
		glClearColor(0.0f, 0.35f, 0.75f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		if (!eglSwapBuffers(p.dpy, p.surf)) {
			fail_egl("eglSwapBuffers");
			platform_shutdown(&p);
			return 1;
		}

		if (++frame % FPS_INTERVAL_FRAMES == 0) {
			double tn = now_s();

			printf("cluster-gauge: frames %lu, %.1f fps\n", frame,
			       FPS_INTERVAL_FRAMES / (tn - t_fps));
			fflush(stdout);
			t_fps = tn;
		}
	}

	platform_shutdown(&p);
	return 0;
}
