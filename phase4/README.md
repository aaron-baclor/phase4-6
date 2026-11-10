# Phase 4 — Two-Tier Grouped STCF (static grouping)

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
CPUS=1 make qemu SCHEDULING_POLICY=GROUP_STCF    # Phase 4 two-tier grouped STCF
```

The kernel prints `Using Policy: <policy>` after console initialization
(Section 4.2). Any other `SCHEDULING_POLICY` value is rejected by the Makefile.

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

## Main features added in Phase 4

- Exactly four process groups (`N_GROUPS = 4`), each starting with
  `DEFAULT_SHARES = 100` shares (Section 4.1.4).
- Static group assignment on allocation: `gid = pid % N_GROUPS`. `init` and
  its direct children (the shell) are placed in Group 0, the default system
  group (Section 2.5).
- Every group tracks its ID (its index in `group[]`), shares, number of
  RUNNABLE/RUNNING processes (`run_procs`), total CPU ticks consumed by its
  processes (`total_ticks`) and virtual runtime
  `vruntime = total_ticks * 1024 / shares` (Equation 2). Virtual runtime is
  never reset.
- Two-tier selection under `GROUP_STCF`: pick the group with at least one
  RUNNABLE process and the lowest virtual runtime (ties: lower group ID), then
  the RUNNABLE process in that group with the lowest effective remaining burst
  time (ties: earlier creation tick, then lower PID).
- Two-tier preemption: the running process yields when another eligible group
  has a lower virtual runtime (or an equal one with a lower ID, matching the
  selection tie rule), or when its own group has a RUNNABLE process with a
  strictly smaller ERBT.
- `schedlog` prints one `GROUP` line per group (`N_GROUPS` lines) just before
  every `swtch()` in `scheduler()` (Section 3.2).

Everything from Phases 1–3 is retained: per-process creation tick, total and
remaining burst, consumed ticks, ready (RUNNABLE) ticks and TIQ; preemptive
STCF; ERBT starvation mitigation; `setburst`; burst exhaustion kills; and
automatic budget refresh for `init` and `sh`.

## Implementation notes

### Process selection, preemption and aging

- `stcf_erbt()` computes `ERBT = max(0, RBT - floor(TIQ / STCF_AT) * STCF_AD)`
  (Equation 1) without unsigned underflow.
- `find_lowest_gid()` scans the process table for groups with a RUNNABLE
  process and returns the one with the lowest `vruntime` (lower ID on ties),
  or -1 when nothing is runnable.
- `find_shortest_process(gid)` returns the RUNNABLE process with the lowest
  ERBT within `gid` (or across all groups when `gid == N_GROUPS`, which is how
  the single-tier `STCF` policy uses it).
- `scheduler()` keeps the vanilla RR loop untouched for `RR`. For `STCF` and
  `GROUP_STCF` it selects as above, resets the chosen process's TIQ to 0
  (Section 2.4) and emits the schedlog snapshot right before `swtch()`.
- `stcf_should_preempt()` is called from `usertrap()` and `kerneltrap()` after
  every trap, so a shorter process that becomes ready through `fork()`, a
  wakeup or `setburst()` preempts the running process at the next trap
  boundary (Section 2.3).
- `dispatch_erbt` (new `struct proc` field) keeps the ERBT a process had when
  it was selected until it has been charged its first CPU tick. Without it,
  the TIQ reset at dispatch makes the process look "long" again, and the very
  next trap would preempt it before it ever ran, which defeats starvation
  mitigation. It is cleared on the first charged tick and by `setburst()`.

### Accounting

`scheduler_tick()` runs once per timer tick on CPU 0 (from `clockintr()`,
before sleepers are woken). RUNNABLE processes gain `ready_ticks` and `tiq`.
The RUNNING process is charged `cpu_ticks`, `burst_consumed`, one fewer
`remaining_burst`, and its group's `total_ticks`/`vruntime`. Under STCF
policies a process that reaches 0 remaining burst is marked killed, except
`init` and processes named `sh`, whose budget is refreshed instead
(Section 2.3). RR never kills on budget exhaustion.

`run_procs` is updated at every transition into or out of RUNNABLE/RUNNING:
`userinit()`, `fork()`, `sleep()`, `wakeup()`, `kill()` (sleeping victim)
and `exit()`.

### Locking

- Lock order is process lock → group lock. No code path holds two group locks
  at once, and no group lock is held across `swtch()`.
- `stcf_should_preempt()` snapshots the running group's `vruntime` under its
  lock, releases it, then reads the candidate group's `vruntime` separately.
- `schedlog_emit()` releases the selected process lock before taking
  `log_lock`, then takes each group/process lock one at a time.

### schedlog format

```
<tick>|GROUP|<gid>(<shares>,<vruntime>),[<pid>]<name>:<state>(<ticks left>,<ticks consumed>),...
```

`<vruntime>` is printed with exactly two decimal places, as the course team
clarified on the forum on 2026-11-10 ("Limit it to two (2) decimal places").
Phase 4 shares are constant, so the printed value is Equation 2 computed
exactly in hundredths (`total_ticks * 1024 * 100 / shares`, truncated).
For example, one tick at 100 shares prints `10.24`. Its whole part is the
integer `vruntime` the scheduler compares, and because every group has the
same shares that comparison is exact.

`GROUP_STCF` prints one line per group in ID order, listing that group's
non-UNUSED processes. `RR` and `STCF` keep the Phase 1–3 single-line format
`<tick>|STCF|0(0,0),...` (Section 3.2). `schedlog(n)` with `n > 0` opens an
`n`-tick window, `0` stops logging and a negative argument returns -1.

## Consolidation of the two Phase 4 versions

This directory is the reconciliation of two Phase 4 trees: our classmate's
branch (`phase4-work`, commit `e919ada`, with the grouped-preemption commit
`3a02a60` and grouped-schedlog commit `d0eeabf`), and our fixed
`phase4-tempv3`. `phase4-tempv3` is the baseline. Both trees share the same
Phase 1–3 code and the same group structures; they differ only in
`kernel/proc.c`, `kernel/proc.h`, the Makefile, the test suite and this README.

| Change in the classmate's version | Decision | Reason |
|---|---|---|
| Grouped preemption that returns early when the lowest eligible group is a different group but not strictly lower | Rejected | With equal vruntimes it returns "no preemption" without checking for a shorter process in the running group, which violates Section 2.3. It fails the tempv3 host check "equal-runtime remote group does not mask a required preemption". |
| Grouped preemption holding the running group's and the candidate group's locks at the same time | Rejected | The lock order depends on which group is running, so two CPUs can acquire the same pair in opposite orders and deadlock. tempv3 never nests group locks. |
| No `dispatch_erbt` | Rejected | Without it, an aged selection is preempted by the next trap after its TIQ resets. Fails the host check "aged selection is not immediately undone after TIQ reset". |
| Grouped `schedlog_emit()` (separate STCF/GROUP branches) | Not adopted | Produces the same output as tempv3's single loop. The simpler version was kept. |
| `panic("Phase 2 supports RR and STCF only")` | Not adopted | The message is stale; tempv3 says "Unknown scheduling policy". |
| UTF-8 comments `⌊ TIQ / AT ⌋`, `×` in `stcf_erbt()` | Adopted | tempv3 had these comments double-encoded (mojibake). |
| README "Main features added" section | Adopted (expanded) | tempv3's README still had a "Lorem ipsum" placeholder. |
| Committed build outputs (`*.o`, `*.d`, `fs.img`, `user/_*`, …) and Windows `:Zone.Identifier` files in the copy | Not carried over | Generated files; ignored by `.gitignore`. |

Kept from tempv3 (not present in the classmate's version): the
lock-order-safe preemption, `dispatch_erbt`, the optional `TRACE=1`
instrumentation used only by the test harness, the RV64GC Makefile pin
(needed for newer RISC-V compilers), the required test programs and the
complete Section 4.3 test suite.

## Code changes (per file)

- `kernel/stcf.h`: `DEFAULT_STCF_BURST 100`, `STCF_AT 20`, `STCF_AD 20`,
  `N_GROUPS 4`, `DEFAULT_SHARES 100` (Section 3.1).
- `kernel/param.h`: includes `stcf.h`; defaults `SCHEDULING_POLICY` to `"RR"`.
- `kernel/main.c`: prints `Using Policy: %s` after console init (Section 4.2).
- `kernel/proc.h`: process fields `created_tick`, `total_burst`,
  `remaining_burst`, `burst_consumed`, `cpu_ticks`, `ready_ticks`, `tiq`,
  `dispatch_erbt`, `gid`; new `struct group` (`lock`, `shares`, `run_procs`,
  `total_ticks`, `vruntime`).
- `kernel/proc.c`:
  - global `group[N_GROUPS]` table, initialized in `procinit()`; policy
    selection from `SCHEDULING_POLICY`
  - `allocproc()`/`freeproc()`: initialize/clear the scheduling fields; static
    `gid = pid % N_GROUPS`
  - `userinit()`, `fork()`: `init` and its children go to Group 0; `run_procs`
    updates
  - `run_procs` updates in `sleep()`, `wakeup()`, `kill()`, `exit()`
  - `stcf_erbt()`, `find_shortest_process(gid)`, new `find_lowest_gid()`
  - `scheduler()`: RR loop unchanged; STCF/GROUP_STCF selection; TIQ reset;
    `dispatch_erbt`; schedlog before `swtch()`
  - `stcf_should_preempt()`: group-first, then same-group ERBT preemption
  - `scheduler_tick()`: per-tick process and group accounting, budget kill,
    `init`/`sh` refresh
  - `proc_setburst()`, `proc_schedlog()`, `schedlog_emit()` (grouped format,
    vruntime with two decimals)
  - optional `TRACE|...` lifecycle lines, compiled only with `TRACE=1`
- `kernel/trap.c`: calls `scheduler_tick()` from `clockintr()`; STCF policies
  yield only when `stcf_should_preempt()` says so, RR keeps per-tick yields.
- `kernel/defs.h`, `kernel/syscall.h` (`SYS_schedlog 23`, `SYS_setburst 24`),
  `kernel/syscall.c`, `kernel/sysproc.c`: syscall plumbing.
- `user/user.h`, `user/usys.pl`: user stubs for `schedlog` and `setburst`.
- `Makefile`: `SCHEDULING_POLICY` (RR/STCF/GROUP_STCF, default RR) with a
  rebuild stamp; optional `TRACE=1`; `-march=rv64gc -mabi=lp64d`; installs the
  test programs.
- `mkfs/mkfs.c`: unchanged xv6 file-system builder.

## Test programs and scripts

All scripts force `CPUS=1` (Section 4.3).

- `user/workload.h`: shared helpers. `compute()` is a finite LCG loop.
  `begin()` opens a schedlog window, gives the workload a large budget and
  prints `WORKLOAD|name|pid`. `done()` stops logging and prints `DONE|name`.
- `user/test_cpu.c`: one long finite computation (CPU-bound).
- `user/test_io.c`: five rounds of a short computation followed by
  `sleep(4)` (I/O-bound).
- `user/test_f_cpu.c`: forks four CPU-bound children and waits.
- `user/test_f_io.c`: forks four I/O-bound children (compute + `sleep(4)`).
- `user/test_bg.c`: alternates long computations with `sleep(2)`.
- `user/test_group_iso.c`: because Phase 4 groups are static, it forks until
  the PID maps to the wanted group. It floods Group 1 with 12 short (burst 10)
  workers while one long worker runs in Group 2, and checks that Group 2 keeps
  getting CPU while Group 1 is flooded (Section 2.5).
- `user/test_phase4.c`: in-guest probes for the syscall ABI and invalid
  `setburst` (killed, exit status -1), schedlog window semantics, budget
  exhaustion (killed under STCF, exit 17 under RR), `fork`/`exec`, wakeup
  preemption (`preempt`) and starvation mitigation (`aging`).
- `sanity_check.sh` / `sanity_check.py`: runs the parser unit tests and the
  host checks, then for each policy builds xv6 and runs `test_phase4`,
  `forktest`, `echo`, and (STCF policies) the preemption and aging probes. It
  checks the exhaustion status, aged-release timing, every schedlog line's
  exact format, per-snapshot group completeness and unique membership, and
  that each PID's group is `0` for init/sh, `pid % 4` otherwise.
- `tests/host_checks.py` / `tests/host_checks.c`: extract the real scheduler
  functions from `kernel/proc.c` and compile them on the host against
  lock-checking stubs. The checks cover two-tier selection, tie-breaking, ERBT
  boundaries, preemption (including the equal-vruntime regression), aging
  retention, vruntime accounting, schedlog format and `setburst`.
- `tests/qemu_tools.py`: builds a policy, drives a QEMU guest with `pexpect`,
  separates kernel records from interleaved console output, and validates
  schedlog snapshots. `tests/test_log_parser.py` and
  `tests/fixtures/interleaved-console.log` are its regression tests.
- `runner.sh` / `tests/run_workloads.py`: `N` (default 10) runs of each
  workload under RR, STCF and GROUP_STCF, grouped per program
  (`runner_logs/<program>/<policy>/run-NN.log`). Optional `JOBS=k` runs `k`
  isolated guests in parallel within one program/policy. Afterwards it runs
  `tests/validate_workloads.py` (exact format, monotonic vruntime and Group 2
  progress during the Group 1 flood).
- `results.py`: reads `runner_logs/` and writes nine plots
  (`results/phase4_<policy>_<metric>.png`: context switches, response time,
  turnaround time × 3 policies, each with all 6 Phase 4 programs; the
  shares program is a Phase 5/6 requirement) plus `results/summary.csv`.
  Context switches count scheduler dispatches into the workload's process
  family. Response time is first dispatch − creation. Turnaround is exit −
  creation. All three are averaged over the workload root and its descendants.

## Testing performed

All results below were produced on 2026-11-10 in this directory, with GNU
Make, `riscv64-elf-gcc`, QEMU 11.1 and Python 3.14 with `pexpect` and
`matplotlib`.

| Check | Result |
|---|---|
| `./sanity_check.sh` (clean build of RR, STCF, GROUP_STCF; `CPUS=1`) | PASS: 5/5 parser tests (including the two-decimal vruntime check), 19/19 host checks, all in-guest probes under all three policies |
| `N=20 JOBS=2 ./runner.sh` (6 programs × 3 policies × 20 runs = 360 guest runs) | PASS: 360/360 runs completed; `validate_workloads.py` passed for all 120 GROUP_STCF logs (exact schema, unique membership, monotonic vruntime, Group 2 progress during the Group 1 flood in 20/20 runs) |
| `python3 results.py` | PASS: nine plots and `results/summary.csv` in `results/` |
| Stock `usertests -q`, RR, `CPUS=3` | PASS: `ALL TESTS PASSED` |
| `CPUS=3` smoke test under STCF and GROUP_STCF (`test_phase4`, `forktest`, `test_f_cpu`, `test_f_io`, `test_group_iso`) | PASS: no panic or hang |
| Non-TRACE build (what `make qemu` produces): schedlog during `test_f_io` | PASS: every line matches the Section 3.2 format, with vruntime to two decimals (e.g. `10.24`). GROUP_STCF prints groups 0–3 in order; RR/STCF print `STCF|0(0,0)`; no `TRACE` output |
| Classmate's `stcf_should_preempt()` + `schedlog_emit()` transplanted into this tree | FAIL, 2/19 host checks (equal-vruntime masking; aged selection undone). This is why they were rejected. |

`runner_logs/` holds the 360 logs (grouped per program, then policy).
`results/` holds the plots generated from them.

Not tested: real hardware; `CPUS>1` beyond the smoke runs listed; grading
test cases that the course team has not published.

## Known limitations

- The test harness and committed evidence use `CPUS=1`, as Section 4.3
  requires. Multi-CPU runs were only smoke-tested (see above). The scheduler
  is written to be SMP-safe, but no exhaustive SMP verification was done.
- Group eligibility in `find_lowest_gid()` is derived from a fresh scan of
  RUNNABLE processes rather than from `run_procs`. `run_procs` is maintained
  for the Section 2.5 bookkeeping requirement.
- `init` and `sh` are recognized for budget refresh by `initproc` and by the
  exact name `sh`, so any process named `sh` is also refreshed.

The original `README` file is the stock xv6 README that the Makefile packages
into `fs.img`. It is intentionally left unchanged.
