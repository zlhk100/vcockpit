<!--
SPDX-License-Identifier: CC-BY-4.0
Copyright (C) 2026 Lei Zhou
-->

# C coding standard

This standard applies to C code that vcockpit owns, starting with
`qnx/cluster-gauge`. Code contributed to projects vcockpit does not own
(QEMU, Weston, Mesa, the Linux kernel) follows that project's own
conventions instead.

The cluster gauge's telltale path is developed following practices used
for ASIL-B software. vcockpit is not certified and makes no ISO 26262
compliance claim: it runs on the non-commercial QNX SDP and on a display
path (Mesa, virglrenderer, QEMU, Weston) that has no safety
qualification.

Every rule below has an ID, a reason, and how it is checked. A rule that
cannot be met in a specific place is recorded as a deviation (last
section); an unrecorded violation is a defect.

## Base standards

- **SEI CERT C Coding Standard** (2016 edition and the online rules). Rules
  cited here by their CERT ID. Where a project rule is stricter, the
  project rule applies.
- **C11** (`-std=c11`) with POSIX and QNX extensions (`_QNX_SOURCE`).

MISRA C is not used as a reference: its rule texts are licensed, and this
project does not cite rules it cannot check.

## State and ownership

| ID | Rule | Reason | Check |
|---|---|---|---|
| CS-STATE-1 | No mutable object with static storage duration (file scope or `static` in a function), except entries in `static-storage-allowlist.txt`, each with a reason | Shared mutable state hides who writes what; under a preemptive scheduler it is the usual source of races | `make check`: object-file symbols in writable sections (`nm`, types `b B d D`) must all be allowlisted |
| CS-STATE-2 | Every object that outlives one function call has one owner, the only code that writes it; others reach it only through a parameter from the owner | Ownership visible in function signatures | Review against the design spec's state-ownership table |
| CS-STATE-3 | Tables that never change are `static const` | Immutable data in a read-only section | `nm` (types `r R`), review |
| CS-STATE-4 | No `volatile` except for objects shared with hardware or with a signal handler; this project uses neither | `volatile` is not synchronisation | `grep`, review |

## Concurrency and real time

| ID | Rule | Reason | Check |
|---|---|---|---|
| CS-RT-1 | The design spec states the thread model; code creates no thread the spec does not name | Concurrency is a design decision, not an implementation detail | Review; `grep pthread_create` |
| CS-RT-2 | No signal handlers. Signals the program reacts to are blocked before any library that may start threads is initialised, and received synchronously (`sigtimedwait`, `sigwaitinfo`) | Removes async-signal-safety hazards (CERT SIG30-C, SIG31-C) and the shared flag they need | `grep sigaction\|signal(` in `make check` |
| CS-RT-3 | No heap allocation by program code after initialisation; this project allocates none at all | Bounded memory, no allocator latency in the loop | `nm -u` shows no `malloc`/`calloc`/`realloc`/`free` imports from program objects |
| CS-RT-4 | Every loop in the frame path has a fixed upper bound; no recursion | Bounded execution time | Review |
| CS-RT-5 | The frame loop measures its own period and reports overruns against its budget | Timing faults are detected, not inferred | Output line, test |
| CS-RT-6 | Scheduling policy and priority are reported at start-up and set only by explicit option | No silent dependence on inherited scheduling | Start-up output |

## Errors and fault reactions

| ID | Rule | Reason | Check |
|---|---|---|---|
| CS-ERR-1 | Every function result that can report an error is checked; a result deliberately ignored is cast to `(void)` with a comment saying why (CERT ERR33-C) | No silent failure | `-Werror=unused-result`, cppcheck, review |
| CS-ERR-2 | Initialisation failures print the failing call and its error, release what was created, and exit with status 1 | One defined fault reaction | Test T3 |
| CS-ERR-3 | Capacity limits (fixed buffers) are checked; reaching one is reported, never silent | Silent truncation hides faults | Review, test |
| CS-ERR-4 | External input (command line, environment) is validated completely, including non-finite numbers and range errors (CERT ERR34-C, FLP04-C) | Malformed input must not change behaviour silently | Review, test |

## Types and expressions

| ID | Rule | Reason | Check |
|---|---|---|---|
| CS-TYPE-1 | No implicit conversion that can lose value or sign (CERT INT31-C, FLP34-C) | Conversions are visible | `-Wconversion -Wsign-conversion -Wdouble-promotion` |
| CS-TYPE-2 | Array indices and sizes use `size_t` or are checked before use (CERT ARR30-C) | Bounds are explicit | Review, cppcheck |
| CS-TYPE-3 | No variable shadows another | Readability, mistakes | `-Wshadow` |
| CS-TYPE-4 | Prototypes for every function; internal functions are `static` | No implicit linkage | `-Wstrict-prototypes -Wmissing-prototypes` |

## Build and analysis

`make check` is the gate for every commit. It runs:

1. The QNX build with warnings as errors:
   `-std=c11 -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow
   -Wcast-qual -Wdouble-promotion -Wstrict-prototypes -Wmissing-prototypes
   -Wformat=2 -Wundef`.
2. The host test of platform-independent code.
3. The static-storage check (CS-STATE-1) and the forbidden-call checks
   (CS-RT-2, CS-RT-3).
4. Static analysis with the tools present on the build host:
   `cppcheck --enable=warning,style,performance,portability --std=c11` and,
   if installed, `clang-tidy` with `cert-*` and `bugprone-*`. A missing
   tool is reported as such, never as a pass.

## Style

- Linux kernel style: tabs for indentation, 8 columns; lines up to 100
  characters; braces on the same line except for function bodies.
- Every file starts with SPDX licence and copyright lines.
- Every function declared in a header has a comment stating what it does,
  its preconditions, and what it returns on error.
- Names: `lower_snake_case`; macros and constants `UPPER_SNAKE_CASE`; one
  prefix per module (`render_`, `drive_cycle_`, `platform_`).

## Deviations

A deviation is recorded in the component's `DEVIATIONS.md`, one entry per
place:

```
### DEV-<n> — <rule ID>
Location:     <file>:<function or line>
Reason:       why the rule cannot be met here
Alternative:  what was considered instead and why it was rejected
Mitigation:   what limits the risk
Approved:     <date>, <name>
```

The static-storage allowlist is the same kind of record, kept in a
machine-readable form: one line per symbol, `<symbol> <file> <reason>`.
