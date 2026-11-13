/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
/*
 * ivi-background: an opaque black surface for the background of the IVI
 * compositor.
 *
 * ivi-shell has no background, and Weston paints only views: output areas
 * that no opaque view covers are never redrawn and keep whatever was drawn
 * there before, for example old cursor images. This client shows one xdg
 * toplevel titled "vcockpit-background" with a 16x16 opaque black buffer.
 * The compositor maps that title to an IVI surface, which is placed under
 * the guest surfaces and scaled to the whole output.
 *
 * Runs until SIGINT or SIGTERM. The two signals are blocked and read from a
 * signalfd; there are no signal handlers.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#define BG_TITLE	"vcockpit-background"
#define BG_SIZE		16				/* pixels, square */
#define BG_STRIDE	(BG_SIZE * 4)			/* XRGB8888 */
#define BG_BYTES	((size_t)BG_STRIDE * BG_SIZE)
#define BG_BLACK	0xff000000u

struct bg {
	struct wl_display	*display;
	struct wl_registry	*registry;
	struct wl_compositor	*compositor;
	struct wl_shm		*shm;
	struct xdg_wm_base	*wm_base;
	struct wl_surface	*surface;
	struct xdg_surface	*xdg_surface;
	struct xdg_toplevel	*toplevel;
	struct wl_buffer	*buffer;
	bool			configured;
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	struct bg *bg = data;

	(void)version;	/* version 1 of each interface is enough */
	if (strcmp(interface, wl_compositor_interface.name) == 0 && !bg->compositor) {
		bg->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0 && !bg->shm) {
		bg->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0 && !bg->wm_base) {
		bg->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		if (bg->wm_base && xdg_wm_base_add_listener(bg->wm_base, &wm_base_listener, bg) != 0)
			fprintf(stderr, "ivi-background: xdg_wm_base listener already set\n");
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;	/* the globals used here are never removed by Weston */
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
	struct bg *bg = data;

	xdg_surface_ack_configure(xdg_surface, serial);
	if (!bg->configured) {
		wl_surface_attach(bg->surface, bg->buffer, 0, 0);
		wl_surface_damage(bg->surface, 0, 0, BG_SIZE, BG_SIZE);
		bg->configured = true;
		printf("ivi-background: surface \"%s\" shown (%dx%d, opaque black)\n",
		       BG_TITLE, BG_SIZE, BG_SIZE);
		(void)fflush(stdout);	/* progress line only; nothing to do if it fails */
	}
	wl_surface_commit(bg->surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

/* Create the 16x16 opaque black buffer. Returns NULL on error (reported). */
static struct wl_buffer *create_buffer(struct wl_shm *shm)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint32_t *px;
	int fd;

	fd = memfd_create("ivi-background", MFD_CLOEXEC);
	if (fd < 0) {
		perror("ivi-background: memfd_create");
		return NULL;
	}
	if (ftruncate(fd, (off_t)BG_BYTES) != 0) {
		perror("ivi-background: ftruncate");
		(void)close(fd);	/* already failing; the descriptor is not reused */
		return NULL;
	}
	px = mmap(NULL, BG_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		perror("ivi-background: mmap");
		(void)close(fd);	/* already failing */
		return NULL;
	}
	for (size_t i = 0; i < (size_t)BG_SIZE * BG_SIZE; i++)
		px[i] = BG_BLACK;
	if (munmap(px, BG_BYTES) != 0)
		perror("ivi-background: munmap");	/* mapping leaks; the buffer is still valid */

	pool = wl_shm_create_pool(shm, fd, (int32_t)BG_BYTES);
	buffer = pool ? wl_shm_pool_create_buffer(pool, 0, BG_SIZE, BG_SIZE, BG_STRIDE,
						  WL_SHM_FORMAT_XRGB8888) : NULL;
	if (pool)
		wl_shm_pool_destroy(pool);	/* the buffer keeps the memory */
	if (close(fd) != 0)
		perror("ivi-background: close");	/* the pool holds its own reference */
	if (!buffer)
		fprintf(stderr, "ivi-background: wl_shm buffer creation failed\n");
	return buffer;
}

/* Create the toplevel, its opaque region and buffer. Returns 0, or -1 (reported). */
static int create_surface(struct bg *bg)
{
	struct wl_region *opaque;

	bg->buffer = create_buffer(bg->shm);
	if (!bg->buffer)
		return -1;
	bg->surface = wl_compositor_create_surface(bg->compositor);
	if (!bg->surface) {
		fprintf(stderr, "ivi-background: wl_compositor_create_surface failed\n");
		return -1;
	}
	opaque = wl_compositor_create_region(bg->compositor);
	if (!opaque) {
		fprintf(stderr, "ivi-background: wl_compositor_create_region failed\n");
		return -1;
	}
	wl_region_add(opaque, 0, 0, BG_SIZE, BG_SIZE);
	wl_surface_set_opaque_region(bg->surface, opaque);
	wl_region_destroy(opaque);

	bg->xdg_surface = xdg_wm_base_get_xdg_surface(bg->wm_base, bg->surface);
	if (!bg->xdg_surface) {
		fprintf(stderr, "ivi-background: xdg_wm_base_get_xdg_surface failed\n");
		return -1;
	}
	if (xdg_surface_add_listener(bg->xdg_surface, &xdg_surface_listener, bg) != 0) {
		fprintf(stderr, "ivi-background: xdg_surface listener already set\n");
		return -1;
	}
	bg->toplevel = xdg_surface_get_toplevel(bg->xdg_surface);
	if (!bg->toplevel) {
		fprintf(stderr, "ivi-background: xdg_surface_get_toplevel failed\n");
		return -1;
	}
	xdg_toplevel_set_title(bg->toplevel, BG_TITLE);
	xdg_toplevel_set_app_id(bg->toplevel, BG_TITLE);
	wl_surface_commit(bg->surface);	/* no buffer yet: asks for the first configure */
	return 0;
}

/* Dispatch Wayland events until a signal arrives on sfd. Returns 0, or -1 on a
 * connection error (reported). */
static int run(struct bg *bg, int sfd)
{
	struct wl_display *d = bg->display;

	for (;;) {
		struct pollfd pfd[2] = {
			{ .fd = wl_display_get_fd(d), .events = POLLIN },
			{ .fd = sfd, .events = POLLIN },
		};

		while (wl_display_prepare_read(d) != 0) {
			if (wl_display_dispatch_pending(d) < 0) {
				perror("ivi-background: wl_display_dispatch_pending");
				return -1;
			}
		}
		if (wl_display_flush(d) < 0 && errno != EAGAIN) {
			perror("ivi-background: wl_display_flush");
			wl_display_cancel_read(d);
			return -1;
		}
		if (poll(pfd, 2, -1) < 0) {
			wl_display_cancel_read(d);
			if (errno == EINTR)
				continue;
			perror("ivi-background: poll");
			return -1;
		}
		if (pfd[0].revents & POLLIN) {
			if (wl_display_read_events(d) < 0) {
				perror("ivi-background: wl_display_read_events");
				return -1;
			}
		} else {
			wl_display_cancel_read(d);
		}
		if (pfd[0].revents & (POLLERR | POLLHUP)) {
			fprintf(stderr, "ivi-background: compositor connection closed\n");
			return -1;
		}
		if (wl_display_dispatch_pending(d) < 0) {
			perror("ivi-background: wl_display_dispatch_pending");
			return -1;
		}
		if (pfd[1].revents & POLLIN) {
			struct signalfd_siginfo si;
			ssize_t n = read(sfd, &si, sizeof(si));

			if (n == (ssize_t)sizeof(si)) {
				printf("ivi-background: signal %u, exiting\n", si.ssi_signo);
				return 0;
			}
			perror("ivi-background: read(signalfd)");
			return -1;
		}
	}
}

static void destroy(struct bg *bg)
{
	if (bg->toplevel)
		xdg_toplevel_destroy(bg->toplevel);
	if (bg->xdg_surface)
		xdg_surface_destroy(bg->xdg_surface);
	if (bg->surface)
		wl_surface_destroy(bg->surface);
	if (bg->buffer)
		wl_buffer_destroy(bg->buffer);
	if (bg->wm_base)
		xdg_wm_base_destroy(bg->wm_base);
	if (bg->shm)
		wl_shm_destroy(bg->shm);
	if (bg->compositor)
		wl_compositor_destroy(bg->compositor);
	if (bg->registry)
		wl_registry_destroy(bg->registry);
	if (bg->display) {
		if (wl_display_flush(bg->display) < 0)
			perror("ivi-background: wl_display_flush at exit");
		wl_display_disconnect(bg->display);
	}
}

int main(void)
{
	struct bg bg = { 0 };
	sigset_t sigs;
	int sfd;
	int status = 1;

	/* Block SIGINT and SIGTERM before anything else; read them from a signalfd. */
	if (sigemptyset(&sigs) != 0 || sigaddset(&sigs, SIGINT) != 0 ||
	    sigaddset(&sigs, SIGTERM) != 0) {
		perror("ivi-background: sigset");
		return 1;
	}
	if (sigprocmask(SIG_BLOCK, &sigs, NULL) != 0) {
		perror("ivi-background: sigprocmask");
		return 1;
	}
	sfd = signalfd(-1, &sigs, SFD_CLOEXEC);
	if (sfd < 0) {
		perror("ivi-background: signalfd");
		return 1;
	}

	bg.display = wl_display_connect(NULL);
	if (!bg.display) {
		perror("ivi-background: wl_display_connect");
		goto out;
	}
	bg.registry = wl_display_get_registry(bg.display);
	if (!bg.registry ||
	    wl_registry_add_listener(bg.registry, &registry_listener, &bg) != 0) {
		fprintf(stderr, "ivi-background: wl_display_get_registry failed\n");
		goto out;
	}
	if (wl_display_roundtrip(bg.display) < 0) {
		perror("ivi-background: wl_display_roundtrip");
		goto out;
	}
	if (!bg.compositor || !bg.shm || !bg.wm_base) {
		fprintf(stderr, "ivi-background: missing global:%s%s%s\n",
			bg.compositor ? "" : " wl_compositor", bg.shm ? "" : " wl_shm",
			bg.wm_base ? "" : " xdg_wm_base");
		goto out;
	}
	if (create_surface(&bg) != 0)
		goto out;
	if (run(&bg, sfd) == 0)
		status = 0;
out:
	destroy(&bg);
	if (close(sfd) != 0)
		perror("ivi-background: close(signalfd)");
	return status;
}
