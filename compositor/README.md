<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# Host compositor image

Debian 12 with Weston 10 and the
[COVESA wayland-ivi-extension](https://github.com/COVESA/wayland-ivi-extension)
2.3.1 (IVI controller, input controller, `ivi-id-agent`,
`LayerManagerControl`), started with the x11 backend into a 3840x1080
window on the host display.

```bash
docker build -t automotive-dash-platform:latest .
```

The image also contains `/usr/local/bin/ivi-input-set`, built from
`ivi-input-set.c`: `ivi-input-set <surface-id>` gives keyboard focus to
that surface and lets surfaces 1000 and 2000 accept pointer and touch
input.

At run time, mount a `weston.ini` that maps window titles to IVI surface
IDs over `/etc/xdg/weston/weston.ini`; see
[docs/running-the-proxy.md](../docs/running-the-proxy.md).
