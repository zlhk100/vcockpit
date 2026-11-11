/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Lei Zhou
 */
#include "listener.h"
#include <stdio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

/* Subset of the org.qemu.Display1.Listener interface, from QEMU's
 * ui/dbus-display1.xml (GPL-2.0-or-later). */
static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.qemu.Display1.Listener'>"
  "    <method name='Scanout'>"
  "      <arg type='u' name='width' direction='in'/><arg type='u' name='height' direction='in'/>"
  "      <arg type='u' name='stride' direction='in'/><arg type='u' name='pixman_format' direction='in'/>"
  "      <arg type='u' name='data' direction='in'/>"
  "    </method>"
  "    <method name='Update'>"
  "      <arg type='i' name='x' direction='in'/><arg type='i' name='y' direction='in'/>"
  "      <arg type='i' name='w' direction='in'/><arg type='i' name='h' direction='in'/>"
  "      <arg type='u' name='stride' direction='in'/><arg type='u' name='data' direction='in'/>"
  "    </method>"
  "    <method name='ScanoutDMABUF'>"
  "      <arg type='h' name='fd' direction='in'/><arg type='u' name='width' direction='in'/>"
  "      <arg type='u' name='height' direction='in'/><arg type='u' name='stride' direction='in'/>"
  "      <arg type='u' name='fourcc' direction='in'/><arg type='t' name='modifier' direction='in'/>"
  "      <arg type='b' name='y0_top' direction='in'/>"
  "    </method>"
  "    <method name='UpdateDMABUF'>"
  "      <arg type='i' name='x' direction='in'/><arg type='i' name='y' direction='in'/>"
  "      <arg type='i' name='w' direction='in'/><arg type='i' name='h' direction='in'/>"
  "    </method>"
  "    <method name='Disable'/>"
  "  </interface>"
  "</node>";

static void
handle_method_call(GDBusConnection *conn, const gchar *sender, const gchar *object_path,
                   const gchar *interface_name, const gchar *method_name,
                   GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)conn; (void)sender; (void)object_path; (void)interface_name;
    WaylandState *ws = (WaylandState *)user_data;
    int scanout_idx = 0; /* all consoles map to scanout 0 for now (see docs/roadmap.md) */

    if (g_strcmp0(method_name, "ScanoutDMABUF") == 0) {
        gint handle;
        guint32 w, h, stride, fourcc;
        guint64 modifier;
        gboolean y0_top;

        g_variant_get(parameters, "(huuuutb)", &handle, &w, &h, &stride, &fourcc, &modifier, &y0_top);

        GDBusMessage *msg = g_dbus_method_invocation_get_message(invocation);
        GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list(msg);

        if (fd_list) {
            GError *err = NULL;
            int fd = g_unix_fd_list_get(fd_list, handle, &err);
            if (fd >= 0) {
                printf("ScanoutDMABUF: scanout=%d %ux%u fourcc=0x%08x modifier=0x%016lx\n",
                       scanout_idx, w, h, fourcc, (unsigned long)modifier);
                fflush(stdout);

                /* Import the dma-buf fd into a wl_buffer via create_immed (no roundtrip). */
                wayland_import_qemu_dmabuf(ws, scanout_idx, fd, w, h, stride, fourcc, modifier);
                close(fd);

                /*
                 * Commit immediately after ScanoutDMABUF.
                 *
                 * With the old virgl stack (QNX gpu_virtio / non-blob path), QEMU
                 * sends ScanoutDMABUF on EVERY frame — a new GBM bo is allocated
                 * per frame and delivered here. UpdateDMABUF is NOT sent in this path.
                 *
                 * With the blob/KMS path (future AGL), QEMU sends ScanoutDMABUF once
                 * to establish the bo, then UpdateDMABUF per frame for damage regions.
                 *
                 * To handle both paths correctly: always attach+commit on ScanoutDMABUF.
                 * When UpdateDMABUF also arrives (blob path), it will re-commit with
                 * damage info — redundant but harmless.
                 */
                if (ws->scanouts[scanout_idx].dmabuf_buffer) {
                    wl_surface_attach(ws->scanouts[scanout_idx].surface,
                                      ws->scanouts[scanout_idx].dmabuf_buffer, 0, 0);
                    wl_surface_damage(ws->scanouts[scanout_idx].surface,
                                      0, 0, (int32_t)w, (int32_t)h);
                    wl_surface_commit(ws->scanouts[scanout_idx].surface);
                    wl_display_flush(ws->display);
                    printf("ScanoutDMABUF: committed %ux%u to surface\n", w, h);
                    fflush(stdout);
                } else {
                    fprintf(stderr, "WARNING: ScanoutDMABUF import returned NULL buffer\n");
                }
            } else {
                fprintf(stderr, "ERROR: Failed to extract FD: %s\n", err->message);
                g_clear_error(&err);
            }
        } else {
            fprintf(stderr, "ERROR: No FD list attached to ScanoutDMABUF\n");
        }

        /* ScanoutDMABUF is fire-and-forget — reply immediately. */
        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "UpdateDMABUF") == 0) {
        gint x, y, w, h;
        g_variant_get(parameters, "(iiii)", &x, &y, &w, &h);

        printf("UpdateDMABUF: scanout=%d x=%d y=%d w=%d h=%d\n",
               scanout_idx, x, y, w, h);
        fflush(stdout);

        /*
         * Blob/KMS path: attach the already-imported buffer with damage info.
         * If dmabuf_buffer is NULL here, ScanoutDMABUF import hasn't completed —
         * reply immediately anyway to unblock QEMU (graphic_hw_gl_block).
         */
        if (ws->scanouts[scanout_idx].dmabuf_buffer) {
            wl_surface_attach(ws->scanouts[scanout_idx].surface,
                              ws->scanouts[scanout_idx].dmabuf_buffer, 0, 0);
            wl_surface_damage(ws->scanouts[scanout_idx].surface, x, y, w, h);
            wl_surface_commit(ws->scanouts[scanout_idx].surface);
            wl_display_flush(ws->display);
        } else {
            fprintf(stderr, "WARNING: UpdateDMABUF received but no dmabuf_buffer ready\n");
        }

        /* REQUIRED: always reply to unblock QEMU's graphic_hw_gl_block. */
        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "Disable") == 0) {
        printf("Disable: scanout=%d\n", scanout_idx);
        fflush(stdout);
        wl_surface_attach(ws->scanouts[scanout_idx].surface, NULL, 0, 0);
        wl_surface_commit(ws->scanouts[scanout_idx].surface);
        wl_display_flush(ws->display);
        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "Scanout") == 0 || g_strcmp0(method_name, "Update") == 0) {
        /* Pixel-copy path: refused, frames must arrive as dma-bufs. */
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                              "Pixel-copy display path not supported; use a dma-buf capable GPU device");
    } else {
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
}

static const GDBusInterfaceVTable listener_vtable = { .method_call = handle_method_call };

void listener_setup(WaylandState *ws, int scanout_index) {
    GError        *error = NULL;
    GDBusNodeInfo *info  = g_dbus_node_info_new_for_xml(introspection_xml, &error);

    if (!info) {
        fprintf(stderr, "FATAL: Failed to parse introspection: %s\n", error->message);
        g_error_free(error);
        return;
    }

    g_dbus_connection_register_object(ws->scanouts[scanout_index].p2p_conn,
                                      "/org/qemu/Display1/Listener",
                                      info->interfaces[0],
                                      &listener_vtable,
                                      ws, NULL, &error);

    if (error) {
        fprintf(stderr, "FATAL: Failed to register Listener: %s\n", error->message);
        g_error_free(error);
    } else {
        printf("Console_%d: Listener registered at /org/qemu/Display1/Listener\n", scanout_index);
    }

    g_dbus_node_info_unref(info);
}
