# Phase 6 — Advanced Two-Tier Grouped STCF (dynamic shares boosting)

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
CPUS=1 make qemu SCHEDULING_POLICY=GROUP_STCF    # two-tier grouped STCF + boosting
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

## What Phase 6 adds

Phase 6 is the cumulative implementation. It contains the consolidated
Phase 4 scheduler (see `phase4/README.md`), the Phase 5 dynamic grouping
(`setgroup`, `groupfork`, `setshares`, 8 groups, parent-group inheritance;
see `phase5/README.md`), and dynamic shares boosting (Section 4.1.6):

- `kernel/stcf.h`: `SHARES_BOOSTING_RATIO 2`, `BOOSTED_SHARES 200`,
  `SHARES_BOOSTING_INTERVAL 10`.
- Each group tracks `io_ticks` next to its CPU ticks (`total_ticks`).
- On every timer tick, each process that is SLEEPING on I/O adds one tick to
  its group's `io_ticks`, so `sleep(n)` contributes `n` ticks.
- Every `SHARES_BOOSTING_INTERVAL` ticks, under `GROUP_STCF`, every group's
  shares are recomputed:
  - 0 CPU ticks: `BOOSTED_SHARES` if `io_ticks > 0`, else `DEFAULT_SHARES`;
  - otherwise `BOOSTED_SHARES` if `io_ticks >= SHARES_BOOSTING_RATIO *
    total_ticks` (the ratio is multiplied out rather than divided), else
    `DEFAULT_SHARES`.
  After that decision, a group with no RUNNABLE/RUNNING process has its
  `io_ticks` reset to 0.
- A share change only affects future virtual-runtime charges. The history is
  preserved exactly as with `setshares` in Phase 5, which is what the course
  team's forum clarification requires. schedlog prints vruntime with two
  decimal places (e.g. `2(200,40.96)` in `test_group_sh`).
- All five custom syscalls remain. `setshares` still validates, updates and
  yields. Under `GROUP_STCF` a manual value lasts until the next boosting
  check. Under `RR`/`STCF` no boosting happens and manual shares persist.

### Interpretation decisions for boosting

Section 4.1.6 leaves two points open. Our first Phase 6 version took the
other reading on both, and its own runner logs showed the feature working
backwards. Across 20 runs per workload, the I/O-bound programs (`test_io`,
`test_f_io`, `test_bg`) were never boosted, while the CPU-bound `test_f_cpu`
was boosted in all 20. The two decisions below fix that:

1. **What counts as I/O.** I/O ticks are ticks spent SLEEPING on anything
   except `wait()`. `sleep(n)`, console, pipe, disk and log waits all count.
   A parent blocked in `wait()` sleeps on its own `struct proc`
   (`chan == p`). It is waiting for a child, not doing I/O. Counting it made
   every fork-and-wait CPU workload (and `init`/`sh`, which are always in
   `wait()` while a command runs in Group 0) look I/O-heavy.
2. **When the idle reset happens.** "If a group has no RUNNABLE/RUNNING
   processes, reset the I/O ticks" is applied at each boosting check, after
   the shares decision, not on every tick. When the reset ran every tick, a
   group whose members were all sleeping, which is exactly an I/O-bound
   group, lost its I/O on every tick and could never qualify. The reset still
   clears stale I/O history once a group goes idle, so a later CPU-bound
   member does not inherit an old boost.

`total_ticks` is the group's lifetime CPU count, the same counter used for
virtual runtime, as Section 4.1.6 says ("which you should already be
tracking").

## Code changes (per file)

Changes since Phase 5:

- `kernel/stcf.h`: the three boosting constants.
- `kernel/proc.h`: `struct group` gains `io_ticks`.
- `kernel/proc.c`:
  - `procinit()` initializes `io_ticks`
  - `scheduler_tick()` counts I/O ticks for sleeping processes (excluding
    `wait()`) and runs the per-interval boosting and idle reset described
    above
- `user/test_group_sh.c`: one CPU worker in Group 1. One CPU worker plus four
  `sleep(1)` workers in Group 2. Both groups stay eligible while Group 2
  earns the boost (100:200 shares).
- `sanity_check.py`, `sanity_check.sh`: add a fresh-guest check that Group 2
  is boosted to 200 during `test_group_sh`.
- `tests/host_checks.c`: Phase 6 checks for the interval, inclusive ratio,
  restore to default, both zero-CPU cases, idle reset after the decision, and
  the RR/STCF guard. Two regressions: a lone `sleep(n)` group must earn a
  boost, and `wait()` must not count as I/O. Both regressions fail against
  the previous Phase 6 kernel.
- `tests/validate_workloads.py`: for `test_group_sh`, requires a 100:200
  snapshot and CPU progress for both groups, with the boosted group receiving
  at least as many ticks during the boosted span.
- `results.py`: plots are named `phase6_<policy>_<metric>.png`.

Everything else (Phase 1–5 kernel code, syscalls 23–27, Makefile, other
workloads, harness) is identical to `phase5/`. The exception is Phase 5's
documentation-only `tests/show_evidence.py` helper, which is not included
here.

## Test programs and scripts

All scripts force `CPUS=1` (Section 4.3). The programs and scripts are the
same as in Phase 5 (see `phase5/README.md` for descriptions), except
`test_group_sh` and the checks listed above. In summary:

- workloads: `test_cpu`, `test_io`, `test_f_cpu`, `test_f_io`, `test_bg`,
  `test_group_iso` (Group 1 flood vs Group 2 long worker), `test_group_sh`
  (automatic 100:200 shares). Probes: `test_sched`.
- `sanity_check.sh`/`.py`: parser tests, host checks, and per-policy in-guest
  checks, plus the live boost check.
- `runner.sh`/`tests/run_workloads.py` → `runner_logs/<program>/<policy>/`,
  then `tests/validate_workloads.py`.
- `results.py` → nine plots + `results/summary.csv` in `results/`.

## Testing performed

All results below were produced on 2026-11-10 in this directory, with GNU
Make, `riscv64-elf-gcc`, QEMU 11.1 and Python 3.14 with `pexpect` and
`matplotlib`.

| Check | Result |
|---|---|
| `./sanity_check.sh` (clean build of RR, STCF, GROUP_STCF; `CPUS=1`) | PASS: 5/5 parser tests (including the two-decimal vruntime check); host checks, including the Phase 6 block and both new regressions and the two-decimal printing check; all in-guest probes under all three policies; live Group 2 boost to 200 in a fresh guest |
| New host regressions against the *previous* Phase 6 `proc.c` | FAIL (as intended), at the idle-group accumulation check. This confirms the tests detect the old defect. |
| `N=20 JOBS=2 ./runner.sh` (7 programs × 3 policies × 20 runs = 420 guest runs) | PASS: 420/420 runs; `validate_workloads.py` passed for all 140 GROUP_STCF logs; isolation 20/20; automatic shares 20/20 (boosted Group 2 got more CPU, e.g. 15 vs 8 ticks in run-01) |
| `python3 results.py` | PASS: nine plots and `results/summary.csv` |
| Stock `usertests -q`, RR, `CPUS=3` | PASS: `ALL TESTS PASSED` |
| `CPUS=3` smoke test under STCF and GROUP_STCF (`test_sched`, `forktest`, `test_f_cpu`, `test_f_io`, `test_group_iso`, `test_group_sh`) | PASS: no panic or hang |
| Non-TRACE build schedlog check (`test_f_io`) | PASS: exact format with two-decimal vruntime, groups 0–7 in order, no `TRACE` output; Group 0 shows `(200,…)` while the I/O workload runs |

Boosting behavior in the GROUP_STCF runner logs (runs with any group at
200 shares, out of 20), before and after the accounting fix:

| Program | Previous Phase 6 | This version |
|---|---|---|
| `test_cpu` (CPU) | 0 | 0 |
| `test_f_cpu` (CPU, parent in `wait()`) | 20 | 0 |
| `test_bg` (mostly CPU, short sleeps) | 0 | 1 |
| `test_io` (I/O) | 0 | 20 |
| `test_f_io` (I/O) | 0 | 20 |
| `test_group_iso` (CPU flood) | 0 | 0 |
| `test_group_sh` (Group 2 sleepers) | 20 (Group 2) | 20 (Group 2) |

`runner_logs/` holds the 420 logs regenerated with this kernel. `results/`
holds the plots generated from them.

Not tested: real hardware; `CPUS>1` beyond the smoke runs listed; grading
test cases that the course team has not published.

## Known limitations

- Committed evidence and the harness use `CPUS=1` (Section 4.3). Multi-CPU
  runs were only smoke-tested.
- The two boosting interpretations above are documented choices. A grader
  who counts `wait()` as I/O, or resets on every tick, would expect different
  boost timing.
- Because the CPU side of the ratio is lifetime `total_ticks` while I/O ticks
  are cleared whenever a group goes idle, a long-lived group whose members
  alternate CPU and sleep is boosted mainly early in its life. This follows
  from the ratio as specified.
- Printed vruntime hundredths are truncated, not rounded (see
  `phase5/README.md`). With 100 and 200 shares every value is exact to two
  decimals anyway (10.24 and 5.12 per tick).

