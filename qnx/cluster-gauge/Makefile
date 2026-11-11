# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Lei Zhou
#
# Build for QNX SDP 8.0, x86_64 target:
#   source <sdp>/qnxsdp-env.sh && make

QCC      ?= qcc
QVARIANT ?= gcc_ntox86_64
CFLAGS   ?= -O2
CFLAGS   += -std=c11 -D_QNX_SOURCE -Wall -Wextra
LDLIBS   := -lscreen -lEGL -lGLESv2 -lm

BUILD := build
SRCS  := src/main.c
BIN   := $(BUILD)/cluster-gauge

.PHONY: all clean

all: $(BIN)

$(BIN): $(SRCS) | $(BUILD)
	$(QCC) -V$(QVARIANT) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
