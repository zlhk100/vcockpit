# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Lei Zhou

project('automotive-dbus-proxy', 'c',
    version: '0.1.0',
    default_options: ['c_std=c11', 'warning_level=2'])

glib_dep = dependency('glib-2.0')
wayland_client_dep = dependency('wayland-client')
gbm_dep = dependency('gbm')
gio_unix_dep = dependency('gio-unix-2.0')

wayland_scanner = find_program('wayland-scanner')

# Protocol XML paths on Ubuntu 22.04 with wayland-protocols 1.25
xdg_shell_xml = '/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'
linux_dmabuf_xml = '/usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml'

protocols_src = []
protocols_hdrs = []

# Generate xdg-shell client code
protocols_src += custom_target('xdg-shell-protocol.c',
    input: xdg_shell_xml,
    output: 'xdg-shell-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']
)
protocols_hdrs += custom_target('xdg-shell-client-protocol.h',
    input: xdg_shell_xml,
    output: 'xdg-shell-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']
)

# Generate linux-dmabuf client code
protocols_src += custom_target('linux-dmabuf-protocol.c',
    input: linux_dmabuf_xml,
    output: 'linux-dmabuf-unstable-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@']
)
protocols_hdrs += custom_target('linux-dmabuf-client-protocol.h',
    input: linux_dmabuf_xml,
    output: 'linux-dmabuf-unstable-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@']
)

# '.' includes the Meson build directory where generated protocol headers land
inc = include_directories('src', '.')

executable('automotive-dbus-proxy',
    sources: [
        'src/main.c', 
        'src/wayland.c', 
        'src/dbus.c',
        'src/listener.c'
    ] + protocols_src + protocols_hdrs,
    dependencies: [glib_dep, wayland_client_dep, gbm_dep, gio_unix_dep],
    include_directories: inc,
    install: true
)
