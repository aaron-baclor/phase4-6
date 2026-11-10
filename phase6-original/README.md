# Phase 6

## Group members

- BACLOR, Aaron Jori B. - 2023-09868 - THY
- FELIX, Raphael Anton G. - 2023-00226 - THY
- GARCIA, Julian Simon L. - 2023-21034 - THY
- MALAGAR, Juan Antonio Miguel M. - 2022-05081 - THY
- ORFIANO, Zachary Claude N. - 2020-11220 - THY


## What we added

Phase 6 builds on the existing Phase 5 copy, which includes our classmate's
Phase 4 implementation at 3a02a60 and the corrections in phase4-tempv3.

We kept the gid process field, find_lowest_gid() group selector, and
find_shortest_process(int gid) process selector. We added dynamic group
membership, ordinary fork inheritance, direct groupfork placement, and
configurable group shares.

RR is still the default policy. STCF selects the global shortest effective
remaining burst. GROUP_STCF first selects the eligible group with the lowest
virtual runtime, then the shortest effective job within that group. Process
ties use creation tick and PID; group ties use the lower group ID.

There are eight groups. Init starts in Group 0, and ordinary children inherit
their parent's gid. GROUP_STCF automatically updates shares every 10 ticks.

## Process counters and budgets

Each new process starts with a fresh 100-tick budget and zero CPU, ready and
aging counters. Fork inherits the group, rather than the parent's budget.

The process fields track:

- Creation tick
- Total and remaining burst budget
- Ticks consumed from the current budget
- Lifetime CPU and ready ticks
- TIQ, the separate ready-queue aging counter
- gid, the group ID
- dispatch_erbt, an earned aging key retained until the first charged tick

Only RUNNING processes consume CPU/burst ticks. RUNNABLE processes gain ready
ticks and TIQ. Sleeping preserves the remaining budget and earns neither.
Dispatch resets TIQ under STCF; the retained key prevents a return-trap check
from immediately undoing an aged selection.

Under STCF, ordinary processes are marked killed when their actual budget
runs out. Init and processes named exactly sh receive a refreshed budget.
RR keeps its normal timer switching without budget-based termination.

## Process groups and shares

Each group tracks shares, RUNNABLE+RUNNING count, total consumed CPU ticks,
and virtual runtime. The whole runtime, fractional part and division residue
preserve precision when weights are large.

A charged CPU tick adds 1024/shares to the current group's runtime. Changing
a weight affects future charges and preserves accumulated history. Emptying
a group or moving a process does not reset or move its past CPU charges.

proc_setstate() updates eligibility counts at state transitions while holding
the process lock. Moving groups transfers the caller's eligible count under
both group locks, acquired in ascending ID order. No group lock crosses a
context switch.

## System calls

### schedlog(int ticks)

System call number: 23. Positive replaces the shared logging window, zero
stops it, and negative returns -1 without changing the window.

GROUP_STCF prints one GROUP record per group before a context switch, with
real shares/runtime and processes filtered by gid. RR and STCF retain the
STCF|0(0,0) format. Numeric states remain USED=1, SLEEPING=2, RUNNABLE=3,
RUNNING=4 and ZOMBIE=5. See specification Section 3.2.

### setburst(int ticks)

System call number: 24. Positive replaces total/remaining budget, resets
consumed ticks and yields. Nonpositive marks the caller killed and returns
-1 internally; the trap path exits it before returning to user code.
Lifetime counters survive. See Section 3.3.

### setgroup(int gid)

System call number: 25. Valid IDs 0-7 move the caller and update group counts.
Invalid IDs mark the caller killed and return -1 internally. Same-group
assignment is a no-op. See Section 3.4.

### groupfork(int gid)

System call number: 26. It shares the normal fork path but assigns the child
to the requested group before RUNNABLE publication. Invalid IDs mark the
parent killed before allocation and return -1 internally. See Section 3.5.

### setshares(int gid, int shares)

System call number: 27. Invalid group IDs or nonpositive weights return -1
without killing the caller. A valid update preserves history, changes the
weight, releases the group lock and yields. See Section 3.6.

Shutdown retains number 22; existing fork and exec interfaces are unchanged.

## Code changes

These entries cover the inherited implementation and Phase 5 additions.

- `.gitignore`: Ignores build products, policy stamps and local verification
  output. Required experiment logs can be retained separately (4.3).
- `mkfs/mkfs.c`: Retains the missing filesystem-builder source supplied in
  Phase 1; no Phase 5 logic was added here.
- `Makefile`: Retains the three policy options, RR default and policy rebuild
  stamps. Pins RV64GC for C/assembly, adds optional TRACE builds and installs
  all test programs (3.1, 4.2, 4.3.1).
- `kernel/stcf.h`: Sets eight groups and retains the burst/aging/default-share
  constants; defines the fractional-runtime scale (2.4-2.5, 4.1.5).
- `kernel/param.h`: Includes stcf.h and provides the RR policy fallback (3.1).
- `kernel/main.c`: Retains the selected-policy boot message (4.2).
- `kernel/proc.h`: Retains gid and process counters; adds the dispatch aging
  key and fractional group-runtime fields (2.2-2.5, 4.1.5).
- `kernel/proc.c`: Retains both Phase 4 selectors; implements inheritance,
  direct child placement, membership/share helpers, consistent state counts,
  incremental runtime, grouped logs and optional lifecycle evidence. Preserves
  preemption, aging and the RR traversal (2.2-2.5, 3.2-3.6, 4.1.5).
- `kernel/trap.c`: Retains tick accounting, preemption and killed checks from
  the earlier phases. No additional Phase 5 trap edits were needed (2.3-2.4).
- `kernel/defs.h`: Declares scheduling and group-management helpers (3.2-3.6).
- `kernel/syscall.h`: Keeps custom numbers 23-27 (3.2-3.6).
- `kernel/syscall.c`: Registers all five handlers (3.2-3.6).
- `kernel/sysproc.c`: Extracts syscall arguments and invokes helpers (3.2-3.6).
- `user/user.h`, `user/usys.pl`: Declare and generate all user stubs (3.2-3.6).
- `user/workload.h`: Provides finite computation and workload markers (4.3.1).
- `user/test_cpu.c`, `user/test_io.c`: Required CPU/I/O workloads (4.3.1).
- `user/test_f_cpu.c`, `user/test_f_io.c`: Fork-intensive variants (4.3.1).
- `user/test_bg.c`: Alternates CPU and sleeping intervals (4.3.1).
- `user/test_group_iso.c`: Short-worker flood in Group 1 alongside a long
  Group 2 worker (2.5, 4.3.1).
- `user/test_group_sh.c`: Exercises automatic 100:200 shares
  with CPU workers and sleeping companions (3.6, 4.3.1).
- `user/test_sched.c`: Public ABI, invalid inputs, inheritance, direct placement,
  exec, exhaustion, same-group preemption and aging probes (4.3.2).
- `sanity_check.sh`, `sanity_check.py`: Host and live one-CPU policy checks (4.3.2).
- `tests/host_checks.py`, `tests/host_checks.c`: Test actual extracted kernel
  functions, including fractional accounting and the equal-runtime regression.
- `tests/qemu_tools.py`, `tests/__init__.py`: Build, run and parse policy output.
- `tests/test_log_parser.py`, `tests/fixtures/interleaved-console.log`: Preserve
  a real interrupted-console regression fixture and parser checks (4.3.2).
- `tests/run_workloads.py`, `runner.sh`: Generate runs grouped per workload;
  default N=10, N=20 supported, optional JOBS parallelism (4.3.3).
- `tests/validate_workloads.py`: Check grouped snapshots, unique membership,
  monotonic runtime, isolation progress and automatic boosting (4.3).
- `tests/show_evidence.py`: Display concise actual excerpts for documentation.
- `results.py`: Produce nine plots and a CSV from actual lifecycle records.
  Counts dispatches into the workload family; response/turnaround average over
  the root and descendants (4.3.4).
- `requirements.txt`: Python dependencies for execution and plotting (4.3).
- `README.md`: Members, behavior, file changes and reproduction instructions (4.5).

## Phase 4 corrections retained

The new Phase 4 foundation needed GROUP-format logging and a preemption fix:
an equal-runtime remote group could cause an early return that skipped a
shorter local job. We also retained an aging fix so an aged selection can
reach user code before its earned priority expires. Group preemption reads
one group snapshot at a time. The requested inherited comments remain.

## Testing

Use Ubuntu with GNU Make, host GCC, Perl, RISC-V GCC/binutils, bc and QEMU
7.2 or later. On newer Ubuntu releases the emulator package is
qemu-system-riscv; older releases provide it in qemu-system-misc.

```sh
uv venv cs140-project1
source cs140-project1/bin/activate
uv pip install -r requirements.txt
./sanity_check.sh
N=20 ./runner.sh
python3 results.py
```

Every QEMU invocation uses CPUS=1. JOBS=4 optionally runs four isolated guests
for a workload/policy. QEMU snapshot overlays isolate guest filesystem writes.
TRACE is off for normal builds; the harness enables exact lifecycle evidence.

The local verification environment uses Python venv because uv is unavailable.
The equivalent setup used here is `python3 -m venv cs140-project1`, followed by
activation and `python3 -m pip install -r requirements.txt`. Run scripts from
this folder. Keep TMPDIR and package caches under verification/ when reproducing
this constrained workspace run. All guests use CPUS=1.

## Phase 6 changes (specification 4.1.6)

- `kernel/stcf.h` adds SHARES_BOOSTING_RATIO=2, BOOSTED_SHARES=200 and
  SHARES_BOOSTING_INTERVAL=10.
- `kernel/proc.h` adds the group's io_ticks counter.
- `kernel/proc.c` initializes io_ticks and counts one tick for each SLEEPING
  process before timer wakeups. SLEEPING is the I/O-duration proxy, including
  sleep(n), device waits and other blocked waits. RUNNABLE waiting is excluded.
  Every tick, groups with no RUNNABLE/RUNNING members have io_ticks cleared.
  On multiples of 10, GROUP_STCF sets shares to 200 when io_ticks >= 2*CPU ticks,
  otherwise 100. Zero CPU with positive I/O boosts, and zero/zero defaults.
  Comparing io_ticks/2 against CPU ticks implements the inclusive threshold
  without overflowing a multiplication. CPU history and virtual runtime survive.
  Fractional accounting residue is rescaled when the share denominator changes.
- `tests/host_checks.c` covers the interval, exact threshold, restoration, both
  zero-CPU cases, inactive reset and RR/STCF guards using actual kernel functions.
- `user/test_group_sh.c` keeps one computing worker in each of Groups 1 and 2
  while four extra Group 2 workers sleep. This produces different automatic
  weights while both groups remain eligible. All workers finish after 32 ticks.
- `sanity_check.py` requires a live
  Group 2 boost under GROUP_STCF in a fresh guest, alongside the inherited ABI and scheduler tests.
- `tests/validate_workloads.py` requires automatic 100:200 shares, CPU progress
  in both groups, at least as many charged ticks for the boosted group during
  the observed boosted span, valid logs and monotonic virtual runtime.
- `results.py` names the nine generated plots phase6 instead of phase5.
- `sanity_check.sh` identifies this as the Phase 6 copy.

All five syscall numbers, user declarations, syscall dispatch entries and stubs
are inherited. setshares still validates, updates and yields. Its manual value
lasts until the next automatic check under GROUP_STCF. RR and STCF keep manual
weights, with their existing selection behavior.

The original `README` is the file packaged by Makefile into fs.img. This README.md
contains the project notes and group roster.

## References and submission work

The requirements used here are CS_140_2627S1_Project1 v2.pdf, especially 4.1.6,
with inherited requirements from 2.2-2.5, 3.1-3.6 and 4.2-4.3. The starting
implementation and its existing test harness are the other code references.
OpenAI Codex assisted with Phase 6 kernel accounting, tests, validation and these
notes. Section 4.7 requires exporting the entire conversation as a PDF under
LLM_Logs and citing its pages. That complete transcript export remains a group
submission task, along with the documentation PDF, member video and peer reviews
in 4.5-4.7. This README is not that PDF submission.

Run `CPUS=1 make qemu SCHEDULING_POLICY=GROUP_STCF`, or replace GROUP_STCF with
STCF or RR. Omitting the policy selects RR.
