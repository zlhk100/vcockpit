/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "wayland.h"
#include "dbus.h"

static GMainLoop *g_loop = NULL;

static void
signal_handler(int sig)
{
    (void)sig;
    if (g_loop)
        g_main_loop_quit(g_loop);
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

    /* Install signal handlers for clean shutdown */
    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);
    signal(SIGPIPE, SIG_IGN);

    WaylandState *ws = wayland_init(wayland_socket, vm_name, n_scanouts);
    if (!ws) {
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

    g_loop = g_main_loop_new(NULL, FALSE);
    wayland_attach_to_mainloop(ws, g_loop);
    g_main_loop_run(g_loop);

    printf("\nShutting down...\n");
    wayland_destroy(ws);
    g_main_loop_unref(g_loop);
    g_free(vm_name);
    g_free(wayland_socket);

    return EXIT_SUCCESS;
}
