/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "wayland.h"
#include "dbus.h"
#include "input.h"

/* Owned by main(); handed to the signal sources as user data. */
typedef struct {
    GMainLoop  *loop;
    const char *signal_name;  /* set by the source that ended the loop */
} ShutdownRequest;

/* Runs from the main loop (g_unix_signal_add), not from a signal handler. */
static gboolean
on_sigint(gpointer user_data)
{
    ShutdownRequest *req = user_data;
    req->signal_name = "SIGINT";
    g_main_loop_quit(req->loop);
    return G_SOURCE_CONTINUE;
}

static gboolean
on_sigterm(gpointer user_data)
{
    ShutdownRequest *req = user_data;
    req->signal_name = "SIGTERM";
    g_main_loop_quit(req->loop);
    return G_SOURCE_CONTINUE;
}

/*
 * Unmap and destroy every scanout surface, then wait (one roundtrip) until
 * the compositor has processed the destroy requests, so that it has removed
 * the surfaces before we disconnect. Call after dbus_destroy(), so that QEMU
 * no longer sends frames. Returns the number of surfaces removed, or -1 if
 * the roundtrip failed.
 */
static int
remove_surfaces(WaylandState *ws)
{
    int removed = 0;

    for (int i = 0; i < ws->n_scanouts; i++) {
        if (!ws->scanouts[i].surface)
            continue;
        wl_surface_attach(ws->scanouts[i].surface, NULL, 0, 0);
        wl_surface_commit(ws->scanouts[i].surface);
        if (ws->scanouts[i].toplevel) {
            xdg_toplevel_destroy(ws->scanouts[i].toplevel);
            ws->scanouts[i].toplevel = NULL;
        }
        if (ws->scanouts[i].xdg_surface) {
            xdg_surface_destroy(ws->scanouts[i].xdg_surface);
            ws->scanouts[i].xdg_surface = NULL;
        }
        wl_surface_destroy(ws->scanouts[i].surface);
        ws->scanouts[i].surface = NULL;
        removed++;
    }

    if (wl_display_roundtrip(ws->display) < 0) {
        fprintf(stderr, "Shut down: Wayland roundtrip failed: %s\n",
                g_strerror(wl_display_get_error(ws->display)));
        return -1;
    }
    return removed;
}

int
main(int argc, char *argv[])
{
    gchar   *vm_name       = NULL;
    gchar   *wayland_socket = NULL;
    gint     n_scanouts    = 1;

    GOptionEntry entries[] = {
        { "vm-name",        0, 0, G_OPTION_ARG_STRING, &vm_name,
          "Short VM name: 'qnx' or 'agl' (not 'qemu-qnx')", "NAME" },
        { "wayland-socket", 0, 0, G_OPTION_ARG_STRING, &wayland_socket,
          "Wayland socket name or full path (e.g. wayland-1 or /tmp/weston-shared/wayland-1)", "PATH" },
        { "n-scanouts",     0, 0, G_OPTION_ARG_INT,    &n_scanouts,
          "Number of scanouts to create (default: 1, max: 8)", "N" },
        { NULL }
    };

    GError         *error   = NULL;
    GOptionContext *context = g_option_context_new("- QEMU D-Bus display to Wayland proxy");
    g_option_context_add_main_entries(context, entries, NULL);

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        fprintf(stderr, "Argument error: %s\n", error->message);
        g_error_free(error);
        g_option_context_free(context);
        return EXIT_FAILURE;
    }
    g_option_context_free(context);

    if (!vm_name || !wayland_socket) {
        fprintf(stderr,
                "Usage: %s --vm-name <name> --wayland-socket <path> [--n-scanouts <N>]\n"
                "Example: %s --vm-name qnx --wayland-socket wayland-1 --n-scanouts 1\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    if (n_scanouts < 1 || n_scanouts > 8) {
        fprintf(stderr, "Error: --n-scanouts must be 1-8\n");
        return EXIT_FAILURE;
    }

    printf("=== automotive-dbus-proxy ===\n");
    printf("VM name:        %s\n", vm_name);
    printf("Wayland socket: %s\n", wayland_socket);
    printf("Scanouts:       %d\n", n_scanouts);
    printf("==================================================\n");

    /* SIGINT and SIGTERM end the main loop (no application signal handler) */
    ShutdownRequest req = { g_main_loop_new(NULL, FALSE), NULL };
    guint sigint_id  = g_unix_signal_add(SIGINT,  on_sigint,  &req);
    guint sigterm_id = g_unix_signal_add(SIGTERM, on_sigterm, &req);
    signal(SIGPIPE, SIG_IGN);

    WaylandState *ws = wayland_init(wayland_socket, vm_name, n_scanouts);
    if (!ws) {
        g_source_remove(sigint_id);
        g_source_remove(sigterm_id);
        g_main_loop_unref(req.loop);
        g_free(vm_name);
        g_free(wayland_socket);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < ws->n_scanouts; i++) {
        wayland_create_surface(ws, i);
    }

    if (wayland_gbm_init(ws, "/dev/dri/renderD128") == 0) {
        for (int i = 0; i < ws->n_scanouts; i++) {
            if (wayland_attach_dmabuf(ws, i, 1280, 720) == 0) {
                printf("Scanout %d: dma-buf import and attach successful\n", i);
            } else {
                fprintf(stderr, "Scanout %d: dma-buf attach failed\n", i);
            }
        }
    } else {
        fprintf(stderr, "WARNING: GBM init failed — running with dummy buffer only\n");
    }

    if (dbus_init(ws) == 0) {
        /* Before the listeners: once registered, QEMU waits for the proxy to
         * answer frame calls, and a synchronous read here would stall both. */
        input_dbus_init(ws);
        if (dbus_register_listeners(ws) != 0) {
            fprintf(stderr, "WARNING: D-Bus listener registration failed\n");
        }
    } else {
        fprintf(stderr, "WARNING: D-Bus init failed — display events will not be received\n");
    }

    printf("\nAll %d surface(s) created. Verify in host compositor:\n", ws->n_scanouts);
    printf("  docker exec dash-test bash -c '\n");
    printf("    export XDG_RUNTIME_DIR=/tmp/weston-runtime\n");
    printf("    export WAYLAND_DISPLAY=wayland-1\n");
    printf("    LayerManagerControl get surfaces'\n");
    printf("\nPress Ctrl+C to exit.\n\n");

    wayland_attach_to_mainloop(ws, req.loop);
    g_main_loop_run(req.loop);

    printf("\nShutting down (%s)...\n", req.signal_name ? req.signal_name : "unknown");

    g_source_remove(sigint_id);
    g_source_remove(sigterm_id);

    /* 1. Release held buttons and keys in the guest, print the input counters. */
    input_shutdown(ws);

    /* 2. Close the D-Bus connections, so QEMU stops sending frames. */
    dbus_destroy(ws);

    /* 3. Remove the surfaces and wait until the compositor has processed them. */
    int exit_status = EXIT_SUCCESS;
    int removed = remove_surfaces(ws);
    if (removed < 0)
        exit_status = EXIT_FAILURE;
    else
        printf("Shut down: %d surface(s) removed\n", removed);

    /* 4. Release the remaining objects and disconnect. */
    wayland_destroy(ws);
    g_main_loop_unref(req.loop);
    g_free(vm_name);
    g_free(wayland_socket);

    return exit_status;
}
