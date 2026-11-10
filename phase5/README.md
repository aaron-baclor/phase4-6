# Phase 5 — Two-Tier Grouped STCF (dynamic grouping)

## Group members

- BACLOR, Aaron Jori B. - 2023-09868 - THY
- FELIX, Raphael Anton G. - 2023-00226 - THY
- GARCIA, Julian Simon L. - 2023-21034 - THY
- MALAGAR, Juan Antonio Miguel M. - 2022-05081 - THY
- ORFIANO, Zachary Claude N. - 2020-11220 - THY

## Running

```sh
CPUS=1 make qemu                                 # default: vanilla xv6 round robin (RR)
CPUS=1 make qemu SCHEDULING_POLICY=STCF          # single-tier STCF with aging
CPUS=1 make qemu SCHEDULING_POLICY=GROUP_STCF    # two-tier grouped STCF
```

Test and analysis commands (Section 4.3), run from this directory:

```sh
uv venv cs140-project1
source cs140-project1/bin/activate
uv pip install -r requirements.txt
./sanity_check.sh
N=20 ./runner.sh
python3 results.py
```

If `uv` is unavailable, `python3 -m venv cs140-project1` followed by
`pip install -r requirements.txt` is equivalent. The scripts also need GNU
Make, host `gcc`, a RISC-V GCC toolchain and `qemu-system-riscv64`.

## What Phase 5 adds

Phase 5 builds on the consolidated Phase 4 implementation (see `phase4/`)
and changes the following (Section 4.1.5):

- `N_GROUPS` is 8. Every group still starts with `DEFAULT_SHARES = 100`.
- The static `pid % N_GROUPS` assignment is removed. `init` starts in Group 0
  and every child inherits its parent's group, unless it was created with
  `groupfork()` or later moves with `setgroup()`. `exec()` keeps the group.
- New system calls:
  - `setgroup(gid)` (#25): moves the caller to `gid`.
  - `groupfork(gid)` (#26): `fork()`, with the child placed directly in `gid`
    before it becomes RUNNABLE.
  - `setshares(gid, shares)` (#27): changes a group's weight, then calls
    `yield()`.
- Invalid `gid` in `setgroup`/`groupfork` (outside 0–7) marks the caller
  killed and returns -1, so the caller exits with status -1 before reaching
  user code. `setshares` returns -1 for an invalid `gid` or `shares <= 0`
  without killing the caller (Sections 3.4–3.6).

Unchanged from Phases 1–4: the per-process tracking, STCF selection with ERBT
aging and `dispatch_erbt`, `setburst`, budget kills with `init`/`sh` refresh,
two-tier selection and preemption with lower-ID group ties, the grouped
schedlog format, the untouched RR scheduler and the boot-time policy message.

## Implementation notes

### Group membership and counts

`fork()` and `groupfork()` share `fork_into_group(gid)`. It sets `np->gid`
while the child is still `USED` and invisible to the scheduler, so the child's
first dispatch already happens in the right group. `fork()` reads the parent's
`gid` under the parent's lock.

`proc_setstate()` is now the single place where a process enters or leaves
the RUNNABLE/RUNNING set. It adjusts the group's `run_procs` under the group
lock while the caller holds the process lock. `userinit()`, `fork`,
`scheduler()`, `yield()`, `sleep()`, `wakeup()`, `kill()` and `exit()` all go
through it, replacing the hand-written increments and decrements of Phase 4.

`proc_setgroup()` moves the caller's `run_procs` contribution between the old
and new groups while holding both group locks, always taken in ascending ID
order. Past CPU charges stay with the group that consumed them. Moving
neither resets nor transfers virtual runtime.

### Virtual runtime with changeable shares

Every charged CPU tick adds exactly `1024 / shares` to the running group's
virtual runtime. The integer part is in `vruntime`. The remainder is carried
as a 32-bit fixed-point fraction (`vruntime_frac`) plus the division residue
(`vruntime_rem`), so no precision is lost even for shares above 1024. While a
group's shares stay constant, `vruntime` equals
`floor(total_ticks * 1024 / shares)`, which is exactly Equation 2.

When `setshares` changes the weight, only future ticks are charged at the new
rate. Accumulated virtual runtime is kept, as Section 2.5 requires ("maintained
and never reset"), and it never jumps up or down. The course team confirmed
both points on the forum thread "[Phase 5] Effect of setshares() on
accumulated group vruntime" (2026-11-09/10): "The new shares would apply to
future CPU ticks", and fractional increments must be stored and taken into
account rather than rounded off. Group comparisons (`find_lowest_gid()`,
`stcf_should_preempt()`) compare `vruntime`, then `vruntime_frac`, then the
group ID.

schedlog prints vruntime with exactly two decimal places, as the same thread
requires: the whole part, then the hundredths truncated from
`vruntime_frac`. For example, one tick at 100 shares prints `10.24`, and one
tick at 300 shares prints `3.41`.

### Locking

Lock order is process lock → group lock. `proc_setgroup()` is the only place
that holds two group locks, and it takes them in ascending ID order. No group
lock is held across `swtch()`. `setshares` releases the group lock before
`yield()`.

## Code changes (per file)

Changes since Phase 4:

- `kernel/stcf.h`: `N_GROUPS 8`; `VRUNTIME_SCALE` (fixed-point scale).
- `kernel/proc.h`: `struct group` gains `vruntime_frac` and `vruntime_rem`.
- `kernel/proc.c`:
  - new `proc_setstate()`, used for every state transition that affects
    `run_procs`
  - `allocproc()`: no more `pid % N_GROUPS`; `userinit()` places `init` in
    Group 0
  - `fork()` → `fork_into_group(parent gid)`; new `groupfork(gid)`
  - new `proc_setgroup()` and `proc_setshares()`
  - `scheduler_tick()`: incremental fractional `vruntime`
  - `find_lowest_gid()`, `stcf_should_preempt()`: fraction-aware comparison
  - `schedlog_emit()`: vruntime printed with two decimal places
- `kernel/defs.h`: declares `proc_setgroup`, `groupfork`, `proc_setshares`.
- `kernel/syscall.h`: `SYS_setgroup 25`, `SYS_groupfork 26`,
  `SYS_setshares 27`.
- `kernel/syscall.c`, `kernel/sysproc.c`: `sys_setgroup`, `sys_groupfork`,
  `sys_setshares`.
- `user/user.h`, `user/usys.pl`: user stubs for the three new syscalls.
- `Makefile`: installs `test_sched` (replacing `test_phase4`) and
  `test_group_sh`.

Inherited from Phases 1–4 and unchanged here: `kernel/param.h`,
`kernel/main.c`, `kernel/trap.c`, the schedlog/setburst plumbing,
`mkfs/mkfs.c`. See `phase4/README.md` for those changes.

## Test programs and scripts

All scripts force `CPUS=1` (Section 4.3).

- `user/workload.h`, `user/test_cpu.c`, `user/test_io.c`, `user/test_f_cpu.c`,
  `user/test_f_io.c`, `user/test_bg.c`: the Section 4.3.1 workloads (shared
  helper, CPU-bound, I/O-bound with `sleep(4)`, fork-intensive CPU and I/O,
  CPU/sleep alternation). All run in the shell's group (Group 0).
- `user/test_group_iso.c`: uses `groupfork(1)` for a parent that floods
  Group 1 with 12 short (burst 10) workers, and `groupfork(2)` for one long
  worker. It shows that the flood cannot starve Group 2.
- `user/test_group_sh.c`: sets shares to 100 (Group 1) and 300 (Group 2),
  runs one CPU-bound worker in each, prints `SHARES_CHANGE|<tick>` after 16
  ticks, swaps the shares to 300:100, and runs until tick 32. The validator
  checks that the group with three times the shares receives at least twice
  the CPU ticks both before and after the swap (Section 4.3.1 item 7).
- `user/test_sched.c`: in-guest probes. Fatal validation (`setburst(0/-1)`,
  `setgroup(-1/8)`, `groupfork(-1/8)` → exit status -1). `setshares`
  validation, with the caller surviving. Inheritance, direct `groupfork`
  placement, and membership surviving `exec`. Budget exhaustion. schedlog
  window semantics. Wakeup preemption (`preempt`) and starvation
  mitigation (`aging`).
- `sanity_check.sh` / `sanity_check.py`: parser unit tests and host checks,
  then per policy: `test_sched`, `echo`, `forktest`, and (STCF policies)
  `preempt`/`aging`. It checks exhaustion status, aged-release timing, that
  the woken short job is charged a tick right away, exact schedlog format, and
  that every `MEMBER|pid|gid` claim matches the group the process appears
  under in schedlog and its first dispatch.
- `tests/host_checks.py` / `tests/host_checks.c`: compile the real
  `proc.c` scheduler functions on the host against lock-order-checking stubs.
  The checks cover state counts and migration, two-tier eligibility and ties,
  ERBT boundaries, preemption (including the equal-vruntime regression),
  `setshares` validation, yield and history, extreme shares, budget death,
  RR/shell guards, fractional vruntime, and schedlog.
- `tests/qemu_tools.py`, `tests/test_log_parser.py`,
  `tests/fixtures/interleaved-console.log`: guest driver, log parser and its
  regression tests.
- `runner.sh` / `tests/run_workloads.py`: `N` (default 10) runs of each of the
  7 programs under RR, STCF and GROUP_STCF, grouped per program
  (`runner_logs/<program>/<policy>/run-NN.log`). Optional `JOBS=k` runs
  parallel isolated guests. It finishes with `tests/validate_workloads.py`:
  format, monotonic vruntime, isolation progress and the shares ratios
  described above.
- `results.py`: writes nine plots, `results/phase5_<policy>_<metric>.png`
  (context switches, response time and turnaround × 3 policies, each with all 7
  programs), plus `results/summary.csv`. The metric definitions are as in
  Phase 4.
- `tests/show_evidence.py`: prints log excerpts for the documentation
  screenshots. The `shares`, `isolation` and `host` topics expect the
  runner/sanity console output saved as `workload-checks.log` and
  `sanity-summary.log` in this directory.

## Testing performed

All results below were produced on 2026-11-10 in this directory, with GNU
Make, `riscv64-elf-gcc`, QEMU 11.1 and Python 3.14 with `pexpect` and
`matplotlib`.

| Check | Result |
|---|---|
| `./sanity_check.sh` (clean build of RR, STCF, GROUP_STCF; `CPUS=1`) | PASS: 5/5 parser tests (including the two-decimal vruntime check), host checks (17,232 assertions, including the two-decimal printing check), all in-guest probes under all three policies, including fatal validation, inheritance, `groupfork` placement and exec preservation |
| `N=20 JOBS=2 ./runner.sh` (7 programs × 3 policies × 20 runs = 420 guest runs) | PASS: 420/420 runs; `validate_workloads.py` passed for all 140 GROUP_STCF logs. Isolation passed 20/20. Shares passed 20/20, e.g. run-01: Group 1:2 CPU ticks 5:14 at 100:300 shares, then 9:3 after the swap to 300:100 |
| `python3 results.py` | PASS: nine plots and `results/summary.csv` |
| Stock `usertests -q`, RR, `CPUS=3` | PASS: `ALL TESTS PASSED` |
| `CPUS=3` smoke test under STCF and GROUP_STCF (`test_sched`, `forktest`, `test_f_cpu`, `test_f_io`, `test_group_iso`, `test_group_sh`) | PASS: no panic or hang |
| Non-TRACE build schedlog check (`test_f_io`) | PASS: exact format with two-decimal vruntime (e.g. `13.65` at 300 shares), groups 0–7 in order, no `TRACE` output |

`runner_logs/` holds the 420 logs. `results/` holds the plots generated from
them.

Not tested: real hardware; `CPUS>1` beyond the smoke runs listed; grading
test cases that the course team has not published.

## Known limitations

- Committed evidence and the harness use `CPUS=1` (Section 4.3). Multi-CPU
  runs were only smoke-tested.
- The printed hundredths are truncated, not rounded; the forum only said
  "two decimal places". The two methods differ only when `1024 / shares` has
  more than two decimals (for example, 300 shares). The scheduler itself
  compares the full-precision value.
- `setgroup()` does not yield. A move into a group with lower virtual runtime
  takes effect at the next trap's preemption check, at most one tick later.


