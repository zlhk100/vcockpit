/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
#ifndef WAYLAND_H
#define WAYLAND_H

#include <wayland-client.h>
#include <stdbool.h>
#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gbm.h>
#include <drm/drm_fourcc.h>

#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#define MAX_SCANOUTS 8

typedef struct {
    struct wl_display                *display;
    struct wl_registry               *registry;
    struct wl_compositor             *compositor;
    struct xdg_wm_base               *wm_base;
    struct zwp_linux_dmabuf_v1       *linux_dmabuf;
    struct wl_shm                    *shm;

    int                               drm_fd;
    struct gbm_device                *gbm_dev;
    GDBusConnection                  *session_bus;

    int                               n_scanouts;
    struct {
        struct wl_surface            *surface;
        struct xdg_surface           *xdg_surface;
        struct xdg_toplevel          *toplevel;
        bool                          configured;
        char                          title[64];

        struct wl_buffer             *dummy_buffer;
        struct wl_shm_pool           *dummy_pool;
        int                           dummy_fd;

        struct gbm_bo                *bo;
        int                           dmabuf_fd;
        struct wl_buffer             *dmabuf_buffer; /* start-up test buffer, then guest frames */
        struct zwp_linux_buffer_params_v1 *params;
        bool                          dmabuf_ready;

        GDBusConnection              *p2p_conn;
        int                           p2p_fd;

        /* Input (input.c) */
        char                          console_path[48]; /* /org/qemu/Display1/Console_<i> */
        bool                          is_absolute;      /* QEMU Mouse.IsAbsolute */
        guint                         props_sub_id;     /* PropertiesChanged subscription */
        int                           last_x, last_y;   /* guest pixels, last sent */
        int                           width, height;    /* guest scanout size, from frames */
    } scanouts[MAX_SCANOUTS];

    /* Opaque import (wayland.c): bit n set once alpha->opaque substitution n has been logged */
    unsigned int                      opaque_logged;

    /* Input (input.c): seat, focus, pending motion, held state, counters */
    struct wl_seat                   *seat;
    struct wl_pointer                *pointer;
    struct wl_keyboard               *keyboard;
    uint32_t                          seat_version;
    int                               pointer_focus;    /* scanout index or -1 */
    int                               keyboard_focus;   /* scanout index or -1 */
    bool                              pending_motion;
    wl_fixed_t                        pending_x, pending_y;
    uint32_t                          held_buttons;     /* bit n: QEMU button n pressed */
    uint32_t                          held_keys[8];     /* bit q: QEMU key number q pressed */
    int                               range_min_x, range_max_x, range_min_y, range_max_y;
    unsigned long                     sent_motion, sent_button, sent_key;
    unsigned long                     dropped_key_unmapped, call_errors;
    unsigned long                     gated_keys;       /* key presses not sent: pointer elsewhere */

    /* Pointer cursor (input.c draws it, wayland_destroy releases it) */
    struct wl_surface                *cursor_surface;
    struct wl_buffer                 *cursor_buffer;
    struct wl_shm_pool               *cursor_pool;
    int                               cursor_fd;

    char                              vm_name[32];
} WaylandState;

WaylandState *wayland_init(const char *wayland_socket, const char *vm_name, int n_scanouts);
void wayland_create_surface(WaylandState *ws, int scanout_index);
void wayland_destroy(WaylandState *ws);
void wayland_attach_to_mainloop(WaylandState *ws, GMainLoop *loop);

int wayland_gbm_init(WaylandState *ws, const char *drm_node);
int wayland_attach_dmabuf(WaylandState *ws, int scanout_index, int width, int height);
void wayland_gbm_destroy(WaylandState *ws);

/* Import a guest frame (dma-buf fd from QEMU) as the scanout's wl_buffer */
void wayland_import_qemu_dmabuf(WaylandState *ws, int scanout_index, int fd, 
                                uint32_t w, uint32_t h, uint32_t stride, 
                                uint32_t fourcc, uint64_t modifier);

#endif /* WAYLAND_H */
