/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
#define _GNU_SOURCE
#include "wayland.h"
#include "dbus.h"
#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

#define MIN_VER(a, b) ((a) < (b) ? (a) : (b))

typedef struct {
    GSource      source;
    WaylandState *ws;
    gpointer     fd_tag;
} WaylandSource;

static gboolean wl_source_prepare(GSource *base, gint *timeout) {
    WaylandSource *source = (WaylandSource *)base;
    *timeout = -1;
    while (wl_display_prepare_read(source->ws->display) != 0)
        wl_display_dispatch_pending(source->ws->display);
    wl_display_flush(source->ws->display);
    return FALSE;
}

static gboolean wl_source_check(GSource *base) {
    WaylandSource *source = (WaylandSource *)base;
    GIOCondition cond = g_source_query_unix_fd(base, source->fd_tag);
    if (cond & G_IO_IN) {
        wl_display_read_events(source->ws->display);
        return TRUE;
    } else {
        wl_display_cancel_read(source->ws->display);
        return FALSE;
    }
}

static gboolean wl_source_dispatch(GSource *base, GSourceFunc callback, gpointer user_data) {
    (void)callback; (void)user_data;
    WaylandSource *source = (WaylandSource *)base;
    wl_display_dispatch_pending(source->ws->display);
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs wl_source_funcs = {
    .prepare = wl_source_prepare, .check = wl_source_check, .dispatch = wl_source_dispatch,
};

void wayland_attach_to_mainloop(WaylandState *ws, GMainLoop *loop) {
    GSource *base = g_source_new(&wl_source_funcs, sizeof(WaylandSource));
    WaylandSource *source = (WaylandSource *)base;
    source->ws = ws;
    source->fd_tag = g_source_add_unix_fd(base, wl_display_get_fd(ws->display), G_IO_IN | G_IO_ERR | G_IO_HUP);
    g_source_attach(base, g_main_loop_get_context(loop));
    g_source_unref(base);
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    WaylandState *ws = data;
    xdg_surface_ack_configure(xdg_surface, serial);
    for (int i = 0; i < ws->n_scanouts; i++) {
        if (ws->scanouts[i].xdg_surface == xdg_surface) {
            ws->scanouts[i].configured = true;
            wl_surface_commit(ws->scanouts[i].surface);
            break;
        }
    }
}

static const struct xdg_surface_listener xdg_surface_listener = { .configure = xdg_surface_configure };
static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h, struct wl_array *s) { (void)data; (void)toplevel; (void)w; (void)h; (void)s; }
static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) { (void)data; (void)toplevel; }
static const struct xdg_toplevel_listener xdg_toplevel_listener = { .configure = xdg_toplevel_configure, .close = xdg_toplevel_close };

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    (void)data; xdg_wm_base_pong(xdg_wm_base, serial);
}
static const struct xdg_wm_base_listener xdg_wm_base_listener = { .ping = xdg_wm_base_ping };

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    WaylandState *ws = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        ws->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, MIN_VER(version, 4));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        ws->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, MIN_VER(version, 1));
        xdg_wm_base_add_listener(ws->wm_base, &xdg_wm_base_listener, ws);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        if (version >= 3) ws->linux_dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, MIN_VER(version, 3));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        ws->shm = wl_registry_bind(registry, name, &wl_shm_interface, MIN_VER(version, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        input_bind_seat(ws, registry, name, version);
    }
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = { .global = registry_global, .global_remove = registry_global_remove };

WaylandState *wayland_init(const char *wayland_socket, const char *vm_name, int n_scanouts) {
    WaylandState *ws = calloc(1, sizeof(WaylandState));
    if (!ws) return NULL;

    for (int i = 0; i < MAX_SCANOUTS; i++) {
        ws->scanouts[i].dummy_fd = -1;
        ws->scanouts[i].dmabuf_fd = -1;
        ws->scanouts[i].p2p_fd = -1;
    }
    ws->drm_fd = -1;
    ws->cursor_fd = -1;
    ws->pointer_focus = -1;
    ws->keyboard_focus = -1;
    strncpy(ws->vm_name, vm_name, sizeof(ws->vm_name) - 1);
    ws->n_scanouts = (n_scanouts > MAX_SCANOUTS) ? MAX_SCANOUTS : n_scanouts;

    const char *socket_name = strrchr(wayland_socket, '/');
    socket_name = socket_name ? socket_name + 1 : wayland_socket;

    ws->display = wl_display_connect(socket_name);
    if (!ws->display) { free(ws); return NULL; }
    
    ws->registry = wl_display_get_registry(ws->display);
    wl_registry_add_listener(ws->registry, &registry_listener, ws);
    wl_display_roundtrip(ws->display);
    wl_display_roundtrip(ws->display);

    if (!ws->compositor || !ws->wm_base || !ws->shm) { wayland_destroy(ws); return NULL; }
    if (input_cursor_init(ws) != 0)
        fprintf(stderr, "Input: no pointer cursor\n");
    return ws;
}

static struct wl_buffer *create_dummy_shm_buffer(WaylandState *ws, int scanout_index) {
    const int width = 1, height = 1, stride = 4, size = 4;
    int fd = (int)syscall(319, "wayland-dummy", 0);
    if (fd < 0) return NULL;
    if (ftruncate(fd, size) < 0) { close(fd); return NULL; }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return NULL; }
    memset(data, 0, size);
    munmap(data, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(ws->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    ws->scanouts[scanout_index].dummy_pool = pool;
    ws->scanouts[scanout_index].dummy_fd = fd;
    return buffer;
}

void wayland_create_surface(WaylandState *ws, int scanout_index) {
    ws->scanouts[scanout_index].surface = wl_compositor_create_surface(ws->compositor);
    ws->scanouts[scanout_index].xdg_surface = xdg_wm_base_get_xdg_surface(ws->wm_base, ws->scanouts[scanout_index].surface);
    xdg_surface_add_listener(ws->scanouts[scanout_index].xdg_surface, &xdg_surface_listener, ws);
    ws->scanouts[scanout_index].toplevel = xdg_surface_get_toplevel(ws->scanouts[scanout_index].xdg_surface);
    xdg_toplevel_add_listener(ws->scanouts[scanout_index].toplevel, &xdg_toplevel_listener, ws);

    snprintf(ws->scanouts[scanout_index].title, sizeof(ws->scanouts[scanout_index].title), "QEMU (qemu-%s-%d)", ws->vm_name, scanout_index);
    xdg_toplevel_set_title(ws->scanouts[scanout_index].toplevel, ws->scanouts[scanout_index].title);
    xdg_toplevel_set_app_id(ws->scanouts[scanout_index].toplevel, "qemu-system-x86_64");

    /* A guest display has no transparency: the whole surface is opaque, so the
     * compositor never blends it with what was on screen before. The region is
     * clipped to the surface size by the compositor. */
    struct wl_region *opaque = wl_compositor_create_region(ws->compositor);
    if (opaque) {
        wl_region_add(opaque, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_set_opaque_region(ws->scanouts[scanout_index].surface, opaque);
        wl_region_destroy(opaque);
    } else {
        fprintf(stderr, "WARNING: wl_compositor_create_region failed; surface not marked opaque\n");
    }

    wl_surface_commit(ws->scanouts[scanout_index].surface);
    wl_display_flush(ws->display);
    while (!ws->scanouts[scanout_index].configured) { wl_display_dispatch(ws->display); }

    ws->scanouts[scanout_index].dummy_buffer = create_dummy_shm_buffer(ws, scanout_index);
    if (ws->scanouts[scanout_index].dummy_buffer) {
        wl_surface_attach(ws->scanouts[scanout_index].surface, ws->scanouts[scanout_index].dummy_buffer, 0, 0);
        wl_surface_damage(ws->scanouts[scanout_index].surface, 0, 0, 1, 1);
        wl_surface_commit(ws->scanouts[scanout_index].surface);
        wl_display_roundtrip(ws->display);
    }
}

void wayland_destroy(WaylandState *ws) {
    if (!ws) return;
    dbus_destroy(ws);
    wayland_gbm_destroy(ws);
    for (int i = 0; i < ws->n_scanouts; i++) {
        if (ws->scanouts[i].dummy_buffer) wl_buffer_destroy(ws->scanouts[i].dummy_buffer);
        if (ws->scanouts[i].dummy_pool) wl_shm_pool_destroy(ws->scanouts[i].dummy_pool);
        if (ws->scanouts[i].dummy_fd >= 0) close(ws->scanouts[i].dummy_fd);
        if (ws->scanouts[i].toplevel) xdg_toplevel_destroy(ws->scanouts[i].toplevel);
        if (ws->scanouts[i].xdg_surface) xdg_surface_destroy(ws->scanouts[i].xdg_surface);
        if (ws->scanouts[i].surface) wl_surface_destroy(ws->scanouts[i].surface);
    }
    if (ws->cursor_surface) wl_surface_destroy(ws->cursor_surface);
    if (ws->cursor_buffer) wl_buffer_destroy(ws->cursor_buffer);
    if (ws->cursor_pool) wl_shm_pool_destroy(ws->cursor_pool);
    if (ws->cursor_fd >= 0) close(ws->cursor_fd);
    if (ws->keyboard) wl_keyboard_destroy(ws->keyboard);
    if (ws->pointer) wl_pointer_destroy(ws->pointer);
    if (ws->seat) wl_seat_destroy(ws->seat);
    if (ws->linux_dmabuf) zwp_linux_dmabuf_v1_destroy(ws->linux_dmabuf);
    if (ws->shm) wl_shm_destroy(ws->shm);
    if (ws->wm_base) xdg_wm_base_destroy(ws->wm_base);
    if (ws->compositor) wl_compositor_destroy(ws->compositor);
    if (ws->registry) wl_registry_destroy(ws->registry);
    if (ws->display) wl_display_disconnect(ws->display);
    free(ws);
}

/* dmabuf_params callbacks — used only by wayland_attach_dmabuf (start-up test buffer).
 * NOT used by wayland_import_qemu_dmabuf (live path uses create_immed instead). */
static void dmabuf_params_created(void *data, struct zwp_linux_buffer_params_v1 *params, struct wl_buffer *buffer) {
    WaylandState *ws = data;
    for (int i = 0; i < ws->n_scanouts; i++) {
        if (ws->scanouts[i].params == params) {
            if (ws->scanouts[i].dmabuf_buffer) wl_buffer_destroy(ws->scanouts[i].dmabuf_buffer);
            ws->scanouts[i].dmabuf_buffer = buffer;
            ws->scanouts[i].dmabuf_ready = true;
            zwp_linux_buffer_params_v1_destroy(params);
            ws->scanouts[i].params = NULL;
            break;
        }
    }
}

static void dmabuf_params_failed(void *data, struct zwp_linux_buffer_params_v1 *params) {
    WaylandState *ws = data;
    for (int i = 0; i < ws->n_scanouts; i++) {
        if (ws->scanouts[i].params == params) {
            ws->scanouts[i].dmabuf_ready = true;
            zwp_linux_buffer_params_v1_destroy(params);
            ws->scanouts[i].params = NULL;
            break;
        }
    }
}

static const struct zwp_linux_buffer_params_v1_listener dmabuf_params_listener = { .created = dmabuf_params_created, .failed = dmabuf_params_failed };

int wayland_gbm_init(WaylandState *ws, const char *drm_node) {
    ws->drm_fd = open(drm_node, O_RDWR | O_CLOEXEC);
    if (ws->drm_fd < 0) return -1;
    ws->gbm_dev = gbm_create_device(ws->drm_fd);
    if (!ws->gbm_dev) { close(ws->drm_fd); ws->drm_fd = -1; return -1; }
    return 0;
}

int wayland_attach_dmabuf(WaylandState *ws, int scanout_index, int width, int height) {
    if (!ws->gbm_dev || !ws->linux_dmabuf) return -1;
    ws->scanouts[scanout_index].bo = gbm_bo_create(ws->gbm_dev, width, height, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!ws->scanouts[scanout_index].bo) return -1;
    ws->scanouts[scanout_index].dmabuf_fd = gbm_bo_get_fd(ws->scanouts[scanout_index].bo);
    uint32_t stride = gbm_bo_get_stride(ws->scanouts[scanout_index].bo);
    
    ws->scanouts[scanout_index].params = zwp_linux_dmabuf_v1_create_params(ws->linux_dmabuf);
    zwp_linux_buffer_params_v1_add_listener(ws->scanouts[scanout_index].params, &dmabuf_params_listener, ws);
    zwp_linux_buffer_params_v1_add(ws->scanouts[scanout_index].params, ws->scanouts[scanout_index].dmabuf_fd, 0, 0, stride, 0, 0);
    zwp_linux_buffer_params_v1_create(ws->scanouts[scanout_index].params, width, height, DRM_FORMAT_XRGB8888, 0);

    ws->scanouts[scanout_index].dmabuf_ready = false;
    while (!ws->scanouts[scanout_index].dmabuf_ready) { wl_display_dispatch(ws->display); }
    if (!ws->scanouts[scanout_index].dmabuf_buffer) return -1;

    wl_surface_attach(ws->scanouts[scanout_index].surface, ws->scanouts[scanout_index].dmabuf_buffer, 0, 0);
    wl_surface_damage(ws->scanouts[scanout_index].surface, 0, 0, width, height);
    wl_surface_commit(ws->scanouts[scanout_index].surface);
    wl_display_roundtrip(ws->display);
    return 0;
}

void wayland_gbm_destroy(WaylandState *ws) {
    if (!ws) return;
    for (int i = 0; i < ws->n_scanouts; i++) {
        if (ws->scanouts[i].dmabuf_buffer) { wl_buffer_destroy(ws->scanouts[i].dmabuf_buffer); ws->scanouts[i].dmabuf_buffer = NULL; }
        if (ws->scanouts[i].params) { zwp_linux_buffer_params_v1_destroy(ws->scanouts[i].params); ws->scanouts[i].params = NULL; }
        if (ws->scanouts[i].dmabuf_fd >= 0) { close(ws->scanouts[i].dmabuf_fd); ws->scanouts[i].dmabuf_fd = -1; }
        if (ws->scanouts[i].bo) { gbm_bo_destroy(ws->scanouts[i].bo); ws->scanouts[i].bo = NULL; }
    }
    if (ws->gbm_dev) { gbm_device_destroy(ws->gbm_dev); ws->gbm_dev = NULL; }
    if (ws->drm_fd >= 0) { close(ws->drm_fd); ws->drm_fd = -1; }
}

/*
 * STEP 5: Import QEMU's live dma-buf into a Wayland wl_buffer.
 *
 * Called from inside the GDBus dispatch callback (handle_method_call) for
 * ScanoutDMABUF. This means the GLib main loop is already running — we must
 * NOT call wl_display_dispatch() or wl_display_roundtrip() here, as both
 * attempt to read the Wayland fd which is already owned by the GSource
 * registered in wayland_attach_to_mainloop(). Doing so causes a nested
 * dispatch deadlock: roundtrip blocks waiting for a sync reply that can
 * never arrive because the GSource is suspended while we're inside its
 * own dispatch callback.
 *
 * Fix: use zwp_linux_buffer_params_v1_create_immed() instead of
 * zwp_linux_buffer_params_v1_create(). create_immed() is a client-side
 * protocol request that returns a wl_buffer object synchronously without
 * any server round-trip. The compositor validates the buffer lazily on
 * first attach. No callback, no waiting, no deadlock.
 */
/* Alpha formats and their opaque twins (same layout, alpha byte ignored).
 * The guest's alpha byte is not meaningful (an X server's depth-24 framebuffer
 * leaves it undefined), so frames are imported as opaque. */
static const uint32_t opaque_twin[][2] = {
    { DRM_FORMAT_ABGR8888,    DRM_FORMAT_XBGR8888 },
    { DRM_FORMAT_ARGB8888,    DRM_FORMAT_XRGB8888 },
    { DRM_FORMAT_ABGR2101010, DRM_FORMAT_XBGR2101010 },
    { DRM_FORMAT_ARGB2101010, DRM_FORMAT_XRGB2101010 },
};

/* Return the opaque twin of an alpha format, or the format itself; log each
 * substitution once per process (bit n of ws->opaque_logged). */
static uint32_t opaque_fourcc(WaylandState *ws, uint32_t fourcc)
{
    for (size_t n = 0; n < sizeof(opaque_twin) / sizeof(opaque_twin[0]); n++) {
        if (opaque_twin[n][0] != fourcc)
            continue;
        if (!(ws->opaque_logged & (1u << n))) {
            ws->opaque_logged |= 1u << n;
            printf("ScanoutDMABUF: importing fourcc 0x%08x as opaque 0x%08x\n",
                   fourcc, opaque_twin[n][1]);
        }
        return opaque_twin[n][1];
    }
    return fourcc;
}

void wayland_import_qemu_dmabuf(WaylandState *ws, int scanout_index, int fd,
                                uint32_t w, uint32_t h, uint32_t stride,
                                uint32_t fourcc, uint64_t modifier)
{
    if (!ws->linux_dmabuf || scanout_index >= ws->n_scanouts) return;

    fourcc = opaque_fourcc(ws, fourcc);

    /* Destroy any previous pending params (shouldn't happen, but be safe) */
    if (ws->scanouts[scanout_index].params) {
        zwp_linux_buffer_params_v1_destroy(ws->scanouts[scanout_index].params);
        ws->scanouts[scanout_index].params = NULL;
    }

    /* Destroy the previous wl_buffer — compositor holds its own reference,
     * so this is safe to call even while the buffer is attached to a surface. */
    if (ws->scanouts[scanout_index].dmabuf_buffer) {
        wl_buffer_destroy(ws->scanouts[scanout_index].dmabuf_buffer);
        ws->scanouts[scanout_index].dmabuf_buffer = NULL;
    }

    struct zwp_linux_buffer_params_v1 *params =
        zwp_linux_dmabuf_v1_create_params(ws->linux_dmabuf);
    if (!params) {
        fprintf(stderr, "ERROR: zwp_linux_dmabuf_v1_create_params failed\n");
        return;
    }

    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride,
                                   modifier >> 32, modifier & 0xFFFFFFFF);

    /*
     * create_immed: returns wl_buffer directly, no server round-trip.
     * The server may send a 'failed' event later if the buffer is invalid,
     * but for QEMU's virglrenderer-allocated dma-bufs this will always
     * succeed — the bo was already validated by the host GPU driver.
     */
    struct wl_buffer *buffer =
        zwp_linux_buffer_params_v1_create_immed(params, w, h, fourcc, 0);

    zwp_linux_buffer_params_v1_destroy(params);

    if (!buffer) {
        fprintf(stderr, "ERROR: zwp_linux_buffer_params_v1_create_immed failed "
                        "for %ux%u fourcc=0x%08x\n", w, h, fourcc);
        return;
    }

    ws->scanouts[scanout_index].dmabuf_buffer = buffer;
    ws->scanouts[scanout_index].dmabuf_ready = true;
    /* Guest scanout size for input positions (the console size changes during boot). */
    ws->scanouts[scanout_index].width = (int)w;
    ws->scanouts[scanout_index].height = (int)h;

    printf("ScanoutDMABUF: import OK scanout=%d %ux%u fourcc=0x%08x modifier=0x%016lx\n",
           scanout_index, w, h, fourcc, (unsigned long)modifier);

    /* Flush the create_immed request to the compositor immediately.
     * Safe: flush only writes, does not read or block. */
    wl_display_flush(ws->display);
}
