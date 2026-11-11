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
SRCS  := src/main.c src/render.c src/drive_cycle.c
HDRS  := src/render.h src/drive_cycle.h
BIN   := $(BUILD)/cluster-gauge

HOST_CC     ?= cc
HOST_CFLAGS ?= -O2 -std=c11 -Wall -Wextra
HOST_TEST   := $(BUILD)/host/drive_cycle_test

.PHONY: all clean host-test

all: $(BIN)

# Drive cycle test (T1), built and run with the host compiler.
host-test: $(HOST_TEST)
	./$(HOST_TEST)

$(HOST_TEST): src/drive_cycle.c src/drive_cycle.h
	mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -DDRIVE_CYCLE_HOST_TEST -o $@ src/drive_cycle.c -lm

$(BIN): $(SRCS) $(HDRS) | $(BUILD)
	$(QCC) -V$(QVARIANT) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
