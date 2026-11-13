<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# Host compositor image

Debian 12 with Weston 10 and the
[COVESA wayland-ivi-extension](https://github.com/COVESA/wayland-ivi-extension)
2.3.1 (IVI controller, input controller, `ivi-id-agent`,
`LayerManagerControl`), started with the x11 backend into a window on the
host display. The output, and the window, is `WESTON_WIDTH` by
`WESTON_HEIGHT` pixels (environment variables, set with `docker run -e`;
default 3840x1080). The size on Weston's command line overrides any
`mode` in the mounted `weston.ini`.

```bash
docker build -t automotive-dash-platform:latest .
```

The image also contains `/usr/local/bin/ivi-input-set`, built from
`ivi-input-set.c`: `ivi-input-set <surface-id>` gives keyboard focus to
that surface and lets surfaces 1000 and 2000 accept pointer and touch
input; `ivi-input-set all` gives keyboard and pointer focus to both
surfaces, for two guests side by side (see
[running the proxy](../docs/running-the-proxy.md#7-two-guests-side-by-side)).

It also contains `/usr/local/bin/ivi-background`, built from
`ivi-background.c`: a client that shows one opaque black 16x16 surface
titled `vcockpit-background` until it receives SIGINT or SIGTERM. Map that
title to an IVI surface in `weston.ini` and scale it over the whole output,
under the guest surfaces: the IVI shell has no background of its own, and
areas no surface covers are never repainted (see
[running the proxy](../docs/running-the-proxy.md#background)).

At run time, mount a `weston.ini` that maps window titles to IVI surface
IDs over `/etc/xdg/weston/weston.ini`; see
[docs/running-the-proxy.md](../docs/running-the-proxy.md).
