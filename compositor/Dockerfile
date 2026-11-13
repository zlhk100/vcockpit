# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Lei Zhou

FROM debian:12
ENV DEBIAN_FRONTEND=noninteractive

# 1. Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential cmake git pkg-config \
    libwayland-dev libpixman-1-dev wayland-protocols \
    libgles2-mesa-dev libgbm-dev libinput-dev \
    libegl1-mesa-dev libwayland-egl1 \
    weston libweston-10-dev libxkbcommon-dev \
    libpng-dev python3 python3-pip \
    wayland-scanner++ \
    mesa-utils imagemagick \
    && rm -rf /var/lib/apt/lists/*

# 2. Build COVESA Extensions 2.3.1 against libweston-10
# NOTE: Never use weston 12+ with COVESA 2.3.x — off-by-one bug in CMake
WORKDIR /opt
RUN git clone -b 2.3.1 https://github.com/COVESA/wayland-ivi-extension.git && \
    mkdir -p wayland-ivi-extension/build && \
    cd wayland-ivi-extension/build && \
    cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local -DLIBWESTON_VER=10 -DCMAKE_C_FLAGS="-fcommon" && \
    make -j$(nproc) && \
    make install && \
    ldconfig

# Clean up
RUN cd /opt && rm -rf wayland-ivi-extension

# 3. Environment setup
ENV LD_LIBRARY_PATH=/usr/local/lib
ENV XDG_RUNTIME_DIR=/tmp/weston-runtime

# 4. Create weston.ini for X11 backend with IVI shell
# Note: ivi-id-agent-module injected at runtime via weston.ini mount
RUN mkdir -p /etc/xdg/weston && \
    printf '[core]\n' > /etc/xdg/weston/weston.ini && \
    printf 'shell=ivi-shell.so\n' >> /etc/xdg/weston/weston.ini && \
    printf 'modules=ivi-controller.so\n' >> /etc/xdg/weston/weston.ini && \
    printf '\n' >> /etc/xdg/weston/weston.ini && \
    printf '[ivi-shell]\n' >> /etc/xdg/weston/weston.ini && \
    printf 'ivi-input-module=ivi-input-controller.so\n' >> /etc/xdg/weston/weston.ini && \
    printf '\n' >> /etc/xdg/weston/weston.ini && \
    printf '[output]\n' >> /etc/xdg/weston/weston.ini && \
    printf 'name=X1\n' >> /etc/xdg/weston/weston.ini && \
    printf 'mode=3840x1080\n' >> /etc/xdg/weston/weston.ini

# 5. Create startup script: start Weston on the host X display and wait for
#    its Wayland socket. VM surfaces arrive at run time through ivi-id-agent;
#    the runbook or the operator places them with LayerManagerControl.
RUN printf '#!/bin/bash\n\n' > /usr/local/bin/start-compositor.sh && \
    printf 'mkdir -p $XDG_RUNTIME_DIR\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'chmod 700 $XDG_RUNTIME_DIR\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'rm -f $XDG_RUNTIME_DIR/wayland-*\n\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo "============================================="\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo " Automotive Display Platform"\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo " Host Weston IVI Compositor"\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo "============================================="\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo ""\n\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo "Starting Weston compositor..."\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'weston --backend=x11-backend.so \\\n' >> /usr/local/bin/start-compositor.sh && \
    printf '       --width=${WESTON_WIDTH:-3840} --height=${WESTON_HEIGHT:-1080} \\\n' >> /usr/local/bin/start-compositor.sh && \
    printf '       --config=/etc/xdg/weston/weston.ini &\n\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'WESTON_PID=$!\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'sleep 4\n\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'SOCKET=$(ls $XDG_RUNTIME_DIR/wayland-* 2>/dev/null | grep -v ".lock" | head -1)\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'if [ -z "$SOCKET" ]; then\n' >> /usr/local/bin/start-compositor.sh && \
    printf '    echo "ERROR: No Wayland socket - Weston failed to start"\n' >> /usr/local/bin/start-compositor.sh && \
    printf '    exit 1\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'fi\n\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'export WAYLAND_DISPLAY=$(basename $SOCKET)\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo "Compositor started: $WAYLAND_DISPLAY"\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo ""\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo "============================================="\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo " Ready for VM surfaces"\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo " Wayland socket: $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo "============================================="\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'echo ""\n\n' >> /usr/local/bin/start-compositor.sh && \
    printf 'wait $WESTON_PID\n' >> /usr/local/bin/start-compositor.sh && \
    chmod +x /usr/local/bin/start-compositor.sh

EXPOSE 5900

ENTRYPOINT ["/usr/local/bin/start-compositor.sh"]

# Build ivi-input-set focus switcher tool
COPY ivi-input-set.c /tmp/ivi-input-set.c
RUN gcc /tmp/ivi-input-set.c \
    -I/usr/local/include \
    -L/usr/local/lib \
    -lilmCommon -lilmControl -lilmInput \
    -Wl,-rpath,/usr/local/lib \
    -o /usr/local/bin/ivi-input-set && \
    chmod +x /usr/local/bin/ivi-input-set

# Build ivi-background, the opaque background surface
COPY ivi-background.c /tmp/ivi-background.c
RUN wayland-scanner client-header \
        /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
        /tmp/xdg-shell-client-protocol.h && \
    wayland-scanner private-code \
        /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
        /tmp/xdg-shell-protocol.c && \
    gcc /tmp/ivi-background.c /tmp/xdg-shell-protocol.c \
    -I/tmp \
    -lwayland-client \
    -o /usr/local/bin/ivi-background && \
    chmod +x /usr/local/bin/ivi-background
