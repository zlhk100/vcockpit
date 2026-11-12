#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Lei Zhou
#
# Coding standard CS-STATE-1: no mutable object with static storage
# duration except allowlisted ones.
#
# Usage: check-static-storage.sh <nm> <allowlist> <src/file.c>...
#
# For each source file, lists the symbols of build/obj/<file>.o in
# writable sections (nm types b B d D) and compares them with the
# allowlist (lines "<symbol> <file> <reason>", '#' starts a comment).
# GCC names a function-scope static "<name>.<n>"; the ".<n>" suffix is
# ignored when matching. Fails on: a symbol not allowlisted, an allowlist
# entry that matches no symbol, a missing object file.

set -u

if [ $# -lt 3 ]; then
	echo "usage: $0 <nm> <allowlist> <src/file.c>..." >&2
	exit 2
fi
nm=$1
allowlist=$2
shift 2

if [ ! -f "$allowlist" ]; then
	echo "CS-STATE-1: allowlist $allowlist missing"
	exit 1
fi

fail=0
found=''

for src in "$@"; do
	base=$(basename "$src" .c)
	obj="build/obj/$base.o"
	if [ ! -f "$obj" ]; then
		echo "CS-STATE-1: $obj missing ($src not compiled), not checked"
		fail=1
		continue
	fi
	syms=$("$nm" "$obj" | awk '$2 ~ /^[bBdD]$/ { print $2 " " $3 }') || {
		echo "CS-STATE-1: $nm $obj failed"
		fail=1
		continue
	}
	[ -n "$syms" ] || continue
	echo "$syms" | while read -r type sym; do
		name=$(echo "$sym" | sed 's/\.[0-9][0-9]*$//')
		if grep -v '^[[:space:]]*#' "$allowlist" |
		   awk -v n="$name" -v f="$src" '$1 == n && $2 == f { ok = 1 } END { exit !ok }'; then
			echo "CS-STATE-1: allowlisted     $src $sym ($type)"
		else
			echo "CS-STATE-1: NOT ALLOWLISTED $src $sym ($type)"
		fi
	done
	found="$found
$(echo "$syms" | awk -v f="$src" '{ sub(/\.[0-9]+$/, "", $2); print $2 " " f }')"
done

# Symbols not allowlisted
if [ -n "$found" ]; then
	bad=$(echo "$found" | while read -r name src; do
		[ -n "$name" ] || continue
		grep -v '^[[:space:]]*#' "$allowlist" |
		awk -v n="$name" -v f="$src" '$1 == n && $2 == f { ok = 1 } END { exit !ok }' ||
		echo "$name"
	done)
	[ -z "$bad" ] || fail=1
fi

# Allowlist entries with no matching symbol
stale=$(grep -v '^[[:space:]]*#' "$allowlist" | awk 'NF { print $1 " " $2 }' |
	while read -r name src; do
		echo "$found" | awk -v n="$name" -v f="$src" '$1 == n && $2 == f { ok = 1 } END { exit !ok }' ||
		echo "$name $src"
	done)
if [ -n "$stale" ]; then
	echo "$stale" | sed 's/^/CS-STATE-1: allowlist entry matches no symbol: /'
	fail=1
fi

if [ $fail -eq 0 ]; then
	echo "CS-STATE-1: PASS"
else
	echo "CS-STATE-1: FAIL"
fi
exit $fail
