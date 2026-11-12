# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Lei Zhou
#
# Build for QNX SDP 8.0, x86_64 target:
#   source <sdp>/qnxsdp-env.sh && make
#
# Targets:
#   all        the program, build/telltale-monitor (warnings are errors)
#   check      the coding-standard gate (docs/coding-standard.md, "Build and analysis")
#   host-test  the classification and comparison test with the host compiler
#   install    copy the program to $(DESTDIR)$(BINDIR) (default build/stage/system/bin)
#   clean
# Variables:
#   DEBUG=1    build with -O0 -g instead of -O2

QCC      ?= qcc
QVARIANT ?= gcc_ntox86_64
QNM      ?= x86_64-pc-nto-qnx8.0.0-nm

# The warning set of the coding standard, warnings as errors.
WARNINGS := -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow \
	    -Wcast-qual -Wdouble-promotion -Wstrict-prototypes -Wmissing-prototypes \
	    -Wformat=2 -Wundef

ifeq ($(DEBUG),1)
OPT := -O0 -g
else
OPT := -O2
endif

VERSION := $(shell git describe --always --dirty 2>/dev/null || echo unknown)

# Headers shared with cluster-gauge
COMMON   := ../common

CFLAGS   := -std=c11 $(OPT) $(WARNINGS)
CPPFLAGS := -D_QNX_SOURCE -I$(COMMON)
# Screen only: the monitor draws its warning with screen_fill, without GL.
LDLIBS   := -lscreen

BUILD := build
OBJ   := $(BUILD)/obj
SRCS  := src/main.c
HDRS  := $(COMMON)/telltale_layout.h
OBJS  := $(patsubst src/%.c,$(OBJ)/%.o,$(SRCS))
BIN   := $(BUILD)/telltale-monitor
VERSION_STAMP := $(BUILD)/version.stamp

HOST_CC     ?= cc
HOST_CFLAGS := -std=c11 -O2 $(WARNINGS) -I$(COMMON)
HOST_TEST   := $(BUILD)/host/telltale_monitor_test

DESTDIR ?= $(BUILD)/stage
BINDIR  ?= /system/bin

# The static-storage check is shared with cluster-gauge.
CHECK_STATIC_STORAGE := ../cluster-gauge/check-static-storage.sh

# Static analysis, as for cluster-gauge: clang-tidy parses the QNX target
# headers; cppcheck checks the program's own code without them.
ANALYSIS_DEFS := -D_QNX_SOURCE -D__QNX__=800 -D__QNXNTO__=1 -D__X86_64__=1 \
		 -D__LITTLEENDIAN__=1 -D__unix__=1
ANALYSIS_INC  := -I$(COMMON) -isystem $(QNX_TARGET)/usr/include
CPPCHECK_FLAGS := --enable=warning,style,performance,portability --std=c11 \
		  --error-exitcode=1 --library=posix \
		  -D_QNX_SOURCE -D__QNX__=800 -D__QNXNTO__=1 -I$(COMMON)
CLANG_TIDY_CHECKS := -*,cert-*,bugprone-*

.PHONY: all objs check host-test install clean FORCE

all: $(BIN)

objs: $(OBJS)

$(BIN): $(OBJS)
	$(QCC) -V$(QVARIANT) -o $@ $(OBJS) $(LDLIBS)

$(OBJ)/%.o: src/%.c $(HDRS) | $(OBJ)
	$(QCC) -V$(QVARIANT) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# The version string is fixed at build time; main.o is rebuilt when it changes.
$(OBJ)/main.o: CPPFLAGS += -DTELLTALE_MONITOR_VERSION='"$(VERSION)"'
$(OBJ)/main.o: $(VERSION_STAMP)

$(VERSION_STAMP): FORCE | $(BUILD)
	@echo '$(VERSION)' | cmp -s - $@ || echo '$(VERSION)' > $@

$(OBJ) $(BUILD):
	mkdir -p $@

# Classification and comparison test, built and run with the host compiler.
host-test: $(HOST_TEST)
	./$(HOST_TEST)

$(HOST_TEST): src/main.c $(HDRS)
	mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -DTELLTALE_MONITOR_HOST_TEST -o $@ src/main.c

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/telltale-monitor

# The gate. Every part runs, so one run shows every finding; any failing
# part, and any analyser that is not installed, fails the gate.
check:
	@fail=0; notrun=''; \
	echo '== check 1/4: QNX build, warnings as errors'; \
	if $(MAKE) --no-print-directory -k all; then echo 'check 1/4: PASS'; \
	else echo 'check 1/4: FAIL'; fail=1; fi; \
	echo '== check 2/4: host test'; \
	if $(MAKE) --no-print-directory host-test; then echo 'check 2/4: PASS'; \
	else echo 'check 2/4: FAIL'; fail=1; fi; \
	echo '== check 3/4: static storage (CS-STATE-1), forbidden calls (CS-RT-2, CS-RT-3), no GL'; \
	part=0; \
	$(CHECK_STATIC_STORAGE) '$(QNM)' static-storage-allowlist.txt $(SRCS) || part=1; \
	if grep -nE '\bsigaction\b|\bsignal[[:space:]]*\(' $(SRCS) $(HDRS); then \
		echo 'CS-RT-2: signal handler API used (above)'; part=1; \
	else echo 'CS-RT-2: no sigaction or signal() in the source'; fi; \
	heap=''; for o in $(OBJS); do \
		[ -f "$$o" ] || { echo "CS-RT-3: $$o missing, not checked"; part=1; continue; }; \
		u=$$($(QNM) -u "$$o" | awk '{print $$NF}' | grep -E '^(malloc|calloc|realloc|free)$$'); \
		[ -z "$$u" ] || { heap="$$heap $$o:$$(echo $$u)"; }; \
	done; \
	if [ -n "$$heap" ]; then echo "CS-RT-3: heap calls imported:$$heap"; part=1; \
	else echo 'CS-RT-3: no malloc/calloc/realloc/free imported by program objects'; fi; \
	if [ ! -f $(BIN) ]; then echo "no GL: $(BIN) missing, not checked"; part=1; \
	else \
		gl=$$($(QNM) -u $(BIN) | awk '{print $$NF}' | grep -E '^(gl|egl|GL|EGL)[A-Z_]'); \
		if [ -n "$$gl" ]; then echo "no GL: GL/EGL symbols imported: $$(echo $$gl)"; part=1; \
		else echo "no GL: $(BIN) imports no gl*/egl* symbol"; fi; \
	fi; \
	if [ $$part -eq 0 ]; then echo 'check 3/4: PASS'; else echo 'check 3/4: FAIL'; fail=1; fi; \
	echo '== check 4/4: static analysis'; \
	part=0; \
	if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --version; \
		cppcheck $(CPPCHECK_FLAGS) $(SRCS) || part=1; \
		cppcheck $(CPPCHECK_FLAGS) -DTELLTALE_MONITOR_HOST_TEST $(SRCS) || part=1; \
	else echo 'cppcheck: NOT INSTALLED - not run, not a pass'; notrun="$$notrun cppcheck"; part=1; fi; \
	if command -v clang-tidy >/dev/null 2>&1; then \
		clang-tidy --version | head -1; \
		clang-tidy --quiet -checks='$(CLANG_TIDY_CHECKS)' --warnings-as-errors='*' \
			$(SRCS) -- -std=c11 -nostdlibinc $(ANALYSIS_DEFS) $(ANALYSIS_INC) \
			-DTELLTALE_MONITOR_VERSION='"analysis"' || part=1; \
	else echo 'clang-tidy: NOT INSTALLED - not run, not a pass'; notrun="$$notrun clang-tidy"; part=1; fi; \
	if [ $$part -eq 0 ]; then echo 'check 4/4: PASS'; else echo 'check 4/4: FAIL'; fail=1; fi; \
	[ -z "$$notrun" ] || echo "check: not run:$$notrun"; \
	if [ $$fail -eq 0 ]; then echo 'check: PASS'; else echo 'check: FAIL'; exit 1; fi

clean:
	rm -rf $(BUILD)

FORCE:
