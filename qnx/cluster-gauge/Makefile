# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Lei Zhou
#
# Build for QNX SDP 8.0, x86_64 target:
#   source <sdp>/qnxsdp-env.sh && make
#
# Targets:
#   all        the program, build/cluster-gauge (warnings are errors)
#   check      the coding-standard gate (docs/coding-standard.md, "Build and analysis")
#   host-test  the drive-cycle test (T1) with the host compiler
#   install    copy the program to $(DESTDIR)$(BINDIR) (default build/stage/system/bin)
#   clean
# Variables:
#   DEBUG=1            build with -O0 -g instead of -O2
#   FAULT_INJECTION=1  add --inject-telltale-fault, for testing telltale-monitor
#                      only; the version string ends in "+fault-injection".
#                      The default build has no fault-injection code, and
#                      "make check" fails if it finds any.

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
ifeq ($(FAULT_INJECTION),1)
VERSION := $(VERSION)+fault-injection
endif

CFLAGS   := -std=c11 $(OPT) $(WARNINGS)
# Headers shared with telltale-monitor
COMMON   := ../common

CPPFLAGS := -D_QNX_SOURCE -I$(COMMON)
LDLIBS   := -lscreen -lEGL -lGLESv2 -lm

BUILD := build
OBJ   := $(BUILD)/obj
SRCS  := src/main.c src/render.c src/drive_cycle.c
HDRS  := src/render.h src/drive_cycle.h $(COMMON)/telltale_layout.h
OBJS  := $(patsubst src/%.c,$(OBJ)/%.o,$(SRCS))
BIN   := $(BUILD)/cluster-gauge
VERSION_STAMP := $(BUILD)/version.stamp

HOST_CC     ?= cc
HOST_CFLAGS := -std=c11 -O2 $(WARNINGS) -I$(COMMON)
HOST_TEST   := $(BUILD)/host/drive_cycle_test

DESTDIR ?= $(BUILD)/stage
BINDIR  ?= /system/bin

# Static analysis. clang-tidy parses the QNX target headers; cppcheck runs
# without them (cppcheck cannot parse the QNX <stdio.h> macros) and checks
# the program's own code.
ANALYSIS_DEFS := -D_QNX_SOURCE -D__QNX__=800 -D__QNXNTO__=1 -D__X86_64__=1 \
		 -D__LITTLEENDIAN__=1 -D__unix__=1
ANALYSIS_INC  := -I$(COMMON) -isystem $(QNX_TARGET)/usr/include
CPPCHECK_FLAGS := --enable=warning,style,performance,portability --std=c11 \
		  --error-exitcode=1 --library=posix \
		  -D_QNX_SOURCE -D__QNX__=800 -D__QNXNTO__=1 -I$(COMMON)
CLANG_TIDY_CHECKS := -*,cert-*,bugprone-*

ifeq ($(FAULT_INJECTION),1)
CPPFLAGS       += -DFAULT_INJECTION=1
ANALYSIS_DEFS  += -DFAULT_INJECTION=1
CPPCHECK_FLAGS += -DFAULT_INJECTION=1
endif

.PHONY: all objs check host-test install clean FORCE

all: $(BIN)

objs: $(OBJS)

$(BIN): $(OBJS)
	$(QCC) -V$(QVARIANT) -o $@ $(OBJS) $(LDLIBS)

$(OBJ)/%.o: src/%.c $(HDRS) | $(OBJ)
	$(QCC) -V$(QVARIANT) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# The version string is fixed at build time; main.o is rebuilt when it
# changes, and so when FAULT_INJECTION changes.
$(OBJ)/main.o: CPPFLAGS += -DCLUSTER_GAUGE_VERSION='"$(VERSION)"'
$(OBJ)/main.o: $(VERSION_STAMP)

$(VERSION_STAMP): FORCE | $(BUILD)
	@echo '$(VERSION)' | cmp -s - $@ || echo '$(VERSION)' > $@

$(OBJ) $(BUILD):
	mkdir -p $@

# Drive cycle test (T1), built and run with the host compiler.
host-test: $(HOST_TEST)
	./$(HOST_TEST)

$(HOST_TEST): src/drive_cycle.c src/drive_cycle.h $(COMMON)/telltale_layout.h
	mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -DDRIVE_CYCLE_HOST_TEST -o $@ src/drive_cycle.c -lm

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/cluster-gauge

# The gate. Every part runs, so one run shows every finding; any failing
# part, and any analyser that is not installed, fails the gate.
check:
	@fail=0; notrun=''; \
	echo '== check 1/4: QNX build, warnings as errors'; \
	if $(MAKE) --no-print-directory -k all; then echo 'check 1/4: PASS'; \
	else echo 'check 1/4: FAIL'; fail=1; fi; \
	echo '== check 2/4: host test (T1)'; \
	if $(MAKE) --no-print-directory host-test; then echo 'check 2/4: PASS'; \
	else echo 'check 2/4: FAIL'; fail=1; fi; \
	echo '== check 3/4: static storage (CS-STATE-1), forbidden calls (CS-RT-2, CS-RT-3), fault injection'; \
	part=0; \
	./check-static-storage.sh '$(QNM)' static-storage-allowlist.txt $(SRCS) || part=1; \
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
	if [ ! -f $(BIN) ]; then echo "fault injection: $(BIN) missing, not checked"; part=1; \
	else \
		inj=$$($(QNM) $(BIN) | grep -i inject); \
		str=$$(grep -c -a -e '--inject-telltale-fault' $(BIN)); \
		if [ '$(FAULT_INJECTION)' = 1 ]; then \
			if [ -n "$$inj" ] && [ "$$str" != 0 ]; then \
				echo "fault injection: present, as built with FAULT_INJECTION=1: $$(echo $$inj)"; \
			else echo 'fault injection: FAULT_INJECTION=1 build has no injection code'; part=1; fi; \
		else \
			if [ -z "$$inj" ] && [ "$$str" = 0 ]; then \
				echo 'fault injection: no injection symbol or option in the default build'; \
			else echo "fault injection: FOUND in the default build: $$(echo $$inj) ($$str option strings)"; part=1; fi; \
		fi; \
	fi; \
	if [ $$part -eq 0 ]; then echo 'check 3/4: PASS'; else echo 'check 3/4: FAIL'; fail=1; fi; \
	echo '== check 4/4: static analysis'; \
	part=0; \
	if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --version; \
		cppcheck $(CPPCHECK_FLAGS) $(SRCS) || part=1; \
	else echo 'cppcheck: NOT INSTALLED - not run, not a pass'; notrun="$$notrun cppcheck"; part=1; fi; \
	if command -v clang-tidy >/dev/null 2>&1; then \
		clang-tidy --version | head -1; \
		clang-tidy --quiet -checks='$(CLANG_TIDY_CHECKS)' --warnings-as-errors='*' \
			$(SRCS) -- -std=c11 -nostdlibinc $(ANALYSIS_DEFS) $(ANALYSIS_INC) \
			-DCLUSTER_GAUGE_VERSION='"analysis"' || part=1; \
	else echo 'clang-tidy: NOT INSTALLED - not run, not a pass'; notrun="$$notrun clang-tidy"; part=1; fi; \
	if [ $$part -eq 0 ]; then echo 'check 4/4: PASS'; else echo 'check 4/4: FAIL'; fail=1; fi; \
	[ -z "$$notrun" ] || echo "check: not run:$$notrun"; \
	if [ $$fail -eq 0 ]; then echo 'check: PASS'; else echo 'check: FAIL'; exit 1; fi

clean:
	rm -rf $(BUILD)

FORCE:
