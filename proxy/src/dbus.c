/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
#define _GNU_SOURCE
#include "dbus.h"
#include "listener.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>

int
dbus_init(WaylandState *ws)
{
    GError      *error    = NULL;
    const gchar *bus_addr = g_getenv("DBUS_SESSION_BUS_ADDRESS");

    if (!bus_addr) {
        fprintf(stderr, "FATAL: DBUS_SESSION_BUS_ADDRESS not set\n");
        return -1;
    }
    printf("D-Bus session bus: %s\n", bus_addr);

    ws->session_bus = g_dbus_connection_new_for_address_sync(
        bus_addr,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);

    if (!ws->session_bus) {
        fprintf(stderr, "FATAL: Failed to connect to D-Bus session: %s\n",
                error->message);
        g_clear_error(&error);
        return -1;
    }
    printf("Connected to D-Bus session bus\n");

    GVariant *res = g_dbus_connection_call_sync(
        ws->session_bus,
        "org.qemu",
        "/org/qemu/Display1/VM",
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)", "org.qemu.Display1.VM", "ConsoleIDs"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        5000,
        NULL,
        &error);

    if (!res) {
        fprintf(stderr, "FATAL: Failed to read ConsoleIDs from org.qemu: %s\n",
                error->message);
        g_clear_error(&error);
        return -1;
    }

    GVariant *v_val = NULL;
    g_variant_get(res, "(v)", &v_val);

    gsize           n_consoles = 0;
    const guint32  *consoles   = g_variant_get_fixed_array(v_val,
                                     &n_consoles, sizeof(guint32));

    printf("ConsoleIDs: [");
    for (gsize i = 0; i < n_consoles; i++)
        printf("%u%s", consoles[i], i < n_consoles - 1 ? ", " : "");
    printf("]\n");

    if ((int)n_consoles != ws->n_scanouts) {
        fprintf(stderr, "WARNING: QEMU reports %zu console(s), proxy configured "
                        "for %d scanout(s) — using minimum\n",
                n_consoles, ws->n_scanouts);
        ws->n_scanouts = ((int)n_consoles < ws->n_scanouts)
                         ? (int)n_consoles : ws->n_scanouts;
    }

    g_variant_unref(v_val);
    g_variant_unref(res);

    printf("D-Bus init OK: org.qemu found, %d scanout(s) confirmed\n",
           ws->n_scanouts);
    return 0;
}

int
dbus_register_listeners(WaylandState *ws)
{
    for (int i = 0; i < ws->n_scanouts; i++) {
        GError *error = NULL;

        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0) {
            fprintf(stderr, "ERROR: Console_%d: socketpair() failed: %s\n",
                    i, strerror(errno));
            return -1;
        }
        ws->scanouts[i].p2p_fd = fds[0];
        int qemu_fd = fds[1];

        GUnixFDList *fd_list = g_unix_fd_list_new();
        gint handle = g_unix_fd_list_append(fd_list, qemu_fd, &error);
        if (handle < 0) {
            fprintf(stderr, "ERROR: Console_%d: g_unix_fd_list_append: %s\n",
                    i, error->message);
            g_clear_error(&error);
            g_object_unref(fd_list);
            close(fds[0]);
            close(fds[1]);
            ws->scanouts[i].p2p_fd = -1;
            return -1;
        }

        gchar   *obj_path = g_strdup_printf("/org/qemu/Display1/Console_%d", i);
        GVariant *res = g_dbus_connection_call_with_unix_fd_list_sync(
            ws->session_bus,
            "org.qemu",
            obj_path,
            "org.qemu.Display1.Console",
            "RegisterListener",
            g_variant_new("(h)", handle),
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            5000,
            fd_list,
            NULL,
            NULL,
            &error);

        g_free(obj_path);
        g_object_unref(fd_list);

        close(qemu_fd);

        if (!res) {
            fprintf(stderr, "ERROR: Console_%d: RegisterListener failed: %s\n",
                    i, error->message);
            g_clear_error(&error);
            return -1;
        }
        g_variant_unref(res);

        GSocket           *sock      = g_socket_new_from_fd(ws->scanouts[i].p2p_fd,
                                                             &error);
        if (!sock) {
            fprintf(stderr, "ERROR: Console_%d: g_socket_new_from_fd: %s\n",
                    i, error->message);
            g_clear_error(&error);
            return -1;
        }
        GSocketConnection *sock_conn = g_socket_connection_factory_create_connection(sock);
        g_object_unref(sock);

        ws->scanouts[i].p2p_conn = g_dbus_connection_new_sync(
            G_IO_STREAM(sock_conn),
            NULL,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
            NULL, NULL, &error);
        g_object_unref(sock_conn);

        if (!ws->scanouts[i].p2p_conn) {
            fprintf(stderr, "ERROR: Console_%d: p2p GDBusConnection failed: %s\n",
                    i, error->message);
            g_clear_error(&error);
            return -1;
        }

        printf("Console_%d: RegisterListener OK — p2p connection established\n", i);
        
        /* Export org.qemu.Display1.Listener on the new connection */
        listener_setup(ws, i);
    }

    return 0;
}

void
dbus_destroy(WaylandState *ws)
{
    if (!ws) return;

    for (int i = 0; i < ws->n_scanouts; i++) {
        if (ws->scanouts[i].p2p_conn) {
            g_dbus_connection_close_sync(ws->scanouts[i].p2p_conn,
                                         NULL, NULL);
            g_object_unref(ws->scanouts[i].p2p_conn);
            ws->scanouts[i].p2p_conn = NULL;
        }
        ws->scanouts[i].p2p_fd = -1;
    }

    if (ws->session_bus) {
        g_dbus_connection_close_sync(ws->session_bus, NULL, NULL);
        g_object_unref(ws->session_bus);
        ws->session_bus = NULL;
    }
}
