# Phase 5

## Group members

- BACLOR, Aaron Jori B. - 2023-09868 - THY
- FELIX, Raphael Anton G. - 2023-00226 - THY
- GARCIA, Julian Simon L. - 2023-21034 - THY
- MALAGAR, Juan Antonio Miguel M. - 2022-05081 - THY
- ORFIANO, Zachary Claude N. - 2020-11220 - THY

## What we added

We added dynamic process groups and configurable shares, keeping
the process counters, preemption, aging, and grouped scheduling
from the earlier phases.

RR is still the default policy. STCF picks the runnable process
with the smallest effective remaining burst time across the whole
process table.

When GROUP_STCF is selected, the scheduler first picks an eligible
group with the lowest virtual runtime. A group is eligible if it
has at least one runnable process. If two groups have the same
virtual runtime at the stored comparison precision, the lower
group ID goes first.

The scheduler then picks the runnable process in that group with
the smallest effective remaining burst time. If two processes
have the same value, the older process goes first. If their
creation ticks are also equal, the lower PID goes first.

There are eight groups, each starting with 100 shares. Init starts
in Group 0. An ordinary fork() child inherits its parent's group,
replacing the earlier pid % N_GROUPS assignment.

We added setgroup() to move the calling process to another group,
groupfork() to create a child directly in a chosen group, and
setshares() to change a group's shares.

Calling exec() keeps the process's group. Automatic shares
boosting is not implemented in this phase.

## Process counters and budgets

Each process starts with a default budget of 100 ticks.

We added fields for:

- Creation tick
- Total burst budget
- Remaining burst budget
- Ticks consumed from the current budget
- Lifetime CPU ticks
- Lifetime ticks spent runnable and waiting
- Time in the ready queue for aging
- Effective remaining burst saved when selected
- Group ID

Running processes gain CPU ticks, while runnable processes gain
ready ticks and time in the ready queue. Sleeping processes gain
neither.

Sleeping, ordinary yields, and preemption keep the remaining budget.
Calling setburst() replaces the budget and resets its consumed
ticks, but keeps the lifetime counters.

Under STCF and GROUP_STCF, ordinary processes are marked killed
when their budget runs out. Init and processes named exactly sh
receive a fresh budget so the system and shell can keep running.

RR keeps its normal timer-based switching and does not kill
processes for running out of budget.

Pressing Ctrl+P shows the process list with the added counters.

## Aging and preemption

We keep aging from Phase 3. For every STCF_AT ticks a process
waits in the ready queue, its effective remaining burst is reduced
by STCF_AD. Both constants are 20.

The effective remaining burst cannot go below zero. Aging changes
the value used for scheduling, without changing the actual
remaining budget.

The ready-queue aging counter resets when the process is selected.
We save its selected effective remaining burst in dispatch_erbt
until its first CPU tick. This prevents the reset from immediately
taking away the priority it earned while waiting.

Under STCF, the running process yields when another runnable
process has a smaller effective remaining burst.

Under GROUP_STCF, it yields when another eligible group has a
lower virtual runtime, or an equal value at the stored comparison
precision with a lower group ID. It also yields when a runnable
process in its own group has a smaller effective remaining burst.

An equal effective remaining burst does not trigger process-level
preemption. Process tie-breaking rules apply when the scheduler
selects between runnable processes.

We check for preemption before returning to user code after
handling a syscall or trap, and after handling interrupts during
kernel work.

## Group counters and shares

Each group tracks:

- Shares
- Number of runnable and running processes
- Total CPU ticks used by its processes
- Whole and fractional virtual runtime
- Remainder from the virtual runtime calculation

The group's ID is its position in the group array.

Each CPU tick adds 1024 / shares to the running group's virtual
runtime. A group with more shares gains virtual runtime more
slowly, giving it more opportunities to run under GROUP_STCF.

We keep fractional values so a tick's charge is not rounded down
to a whole number. Group comparisons check the whole virtual
runtime, then its stored fractional part, then the group ID.

Changing shares affects future CPU charges. It does not recalculate
all earlier CPU ticks using the new shares. The whole and stored
fractional virtual runtime are kept, while the division remainder
is rescaled using integer arithmetic.

Group CPU ticks and virtual runtime are kept even when the group
has no runnable or running processes.

Moving a process updates the runnable and running counts of the
old and new groups. Past CPU charges stay with the group that
used them. Moving does not transfer or reset group virtual runtime.

proc_setstate() updates the group count when a process enters
or leaves the runnable and running states.

When both locks are needed, we take the process lock before the
group lock. Moving between groups takes both group locks in
ascending group ID order. Group locks are released before
switching processes.

## System calls

### setburst(int ticks)

System call number: 24.

A positive value sets the calling process's total and remaining
budget to that value, clears its current consumed ticks, and yields
so the scheduler can pick again. It also clears the saved
dispatch_erbt value.

A zero or negative value marks the caller killed.

### schedlog(int ticks)

System call number: 23.

A positive value starts a shared logging window for that many ticks.
Calling it again replaces the current window. Zero stops logging,
and a negative value returns -1.

While the window is active, the scheduler prints the process
information before switching to a process. The selected process
is already marked RUNNING when the entry is printed.

Under GROUP_STCF, it prints one GROUP line for each of the eight
groups in group ID order. Each line includes the group's shares,
virtual runtime, and processes that are not UNUSED.

Virtual runtime is printed with two decimal places, truncated
rather than rounded. For example, one CPU tick at 100 shares
prints 10.24, while one tick at 300 shares prints 3.41.

RR and STCF keep the earlier single-line format with STCF in
the prefix and 0(0,0) as the group information.

The state numbers are:

- USED: 1
- SLEEPING: 2
- RUNNABLE: 3
- RUNNING: 4
- ZOMBIE: 5

### setgroup(int gid)

System call number: 25.

Moves the calling process to the chosen group. Valid group IDs
are 0 to 7. Choosing the current group leaves it unchanged.

An invalid group ID marks the caller killed. The caller exits
with status -1 before returning to user code.

Changing groups keeps the process's budget and counters.
setgroup() does not directly call yield(); the trap return path
checks whether the process should be preempted.

### groupfork(int gid)

System call number: 26.

Creates a child in the chosen group. The child is assigned its
group before it becomes runnable, so its first execution already
happens in that group.

Like fork(), it returns the child's PID to the parent and 0
to the child. An allocation failure returns -1 to the parent.

Valid group IDs are 0 to 7. An invalid group ID marks the caller
killed before a child is allocated.

The parent's group does not change.

### setshares(int gid, int shares)

System call number: 27.

Changes the chosen group's shares and yields so the scheduler
can pick again. The new shares affect future virtual-runtime
charges.

Valid group IDs are 0 to 7, and shares must be positive.
An invalid argument returns -1 without killing the caller.
A successful call returns 0.

The existing shutdown system call keeps number 22.

## Code changes

This list covers the cumulative scheduler changes and supporting
files from the provided xv6 starter included in this branch.

- `.gitignore`: Changed the mkfs ignore rule to ignore only the
  compiled binary, so its source can be committed. Also excludes
  compiled files and local test outputs.

- `mkfs/mkfs.c`: Added the missing filesystem image builder source
  from the provided mkfs archive.

- `Makefile`: Added a default target that builds the kernel and
  filesystem image, the scheduling policy option, and automatic
  rebuilding when the policy or tracing option changes. Supports
  RR, STCF, and GROUP_STCF. Added the test programs, shared workload
  header dependency, and RISC-V architecture flags. Replaced
  test_phase4 with test_sched and added test_group_sh.

- `kernel/stcf.h`: Added the default burst, aging constants,
  N_GROUPS set to 8, DEFAULT_SHARES set to 100, and the scale
  used for fractional virtual runtime.

- `kernel/param.h`: Included stcf.h and used RR when no policy
  is specified.

- `kernel/main.c`: Added the selected policy to the boot output.

- `kernel/proc.h`: Added the process budget, creation tick,
  lifetime counters, aging counter, saved dispatch ERBT, and
  group ID. Added the group structure for shares, runnable and
  running counts, CPU ticks, and virtual runtime. Added fields
  for fractional virtual runtime and its division remainder.

- `kernel/proc.c`: Added policy setup, counter initialization and
  cleanup, process selection, tick accounting, budget handling,
  syscall helpers, scheduler logging, and the extra Ctrl+P output.
  Added aging, saved dispatch ERBT, group-first selection, and
  group-aware preemption. Replaced static group assignment with
  parent-group inheritance. Added fork_into_group(), groupfork(),
  proc_setgroup(), and proc_setshares(). Added proc_setstate()
  for group count updates and fractional virtual-runtime
  accounting for changing shares. Added optional TRACE output
  for testing. Kept the RR process-table traversal.

- `kernel/trap.c`: Added tick accounting before waking sleepers.
  Added preemption checks in usertrap() and kerneltrap() for STCF
  and GROUP_STCF. Added another killed-process check after a
  possible yield in usertrap(). Kept normal timer switching for RR.

- `kernel/defs.h`: Added declarations for the scheduler and syscall
  helpers, including stcf_should_preempt(), proc_setgroup(),
  groupfork(), and proc_setshares().

- `kernel/syscall.h`: Added the numbers for schedlog, setburst,
  setgroup, groupfork, and setshares.

- `kernel/syscall.c`: Connected the new syscall numbers to
  their handlers.

- `kernel/sysproc.c`: Added handlers that read the syscall
  arguments and call the helpers in proc.c.

- `user/user.h`: Added the user-side declarations for schedlog,
  setburst, setgroup, groupfork, and setshares.

- `user/usys.pl`: Added entries to generate the new syscall stubs.

- `user/workload.h`: Added shared helpers for computation,
  workload identification, budgets, and logging.

- `user/test_cpu.c`: Added a CPU-bound workload.

- `user/test_io.c`: Added a workload that alternates short
  computations with sleep calls.

- `user/test_f_cpu.c`: Added a workload with four CPU-bound
  children.

- `user/test_f_io.c`: Added a workload with four children that
  alternate computation and sleep.

- `user/test_bg.c`: Added a workload that alternates longer
  computations with short sleeps.

- `user/test_group_iso.c`: Uses groupfork() to place many short
  processes in Group 1 and a long process in Group 2, checking
  that Group 2 still receives CPU time.

- `user/test_group_sh.c`: Runs CPU workers in Groups 1 and 2 with
  shares of 100 and 300, then swaps their shares to check how
  CPU allocation changes.

- `user/test_sched.c`: Added checks for syscall arguments,
  group inheritance, groupfork placement, group preservation
  across exec, budgets, preemption, aging, and scheduler logging.
  Replaces the earlier test_phase4 program.

- `sanity_check.sh`: Runs the sanity checks with one virtual CPU.

- `sanity_check.py`: Runs the log parser tests, host checks,
  and xv6 checks under RR, STCF, and GROUP_STCF.

- `tests/__init__.py`: Makes the test helpers importable as
  a Python package.

- `tests/host_checks.py`: Extracts scheduler functions from proc.c
  and compiles them for testing on the host.

- `tests/host_checks.c`: Checks selection, preemption, aging,
  group counts and movement, locking, logging, budgets, shares,
  and fractional virtual runtime.

- `tests/qemu_tools.py`: Starts QEMU sessions, runs commands,
  and reads scheduler logs.

- `tests/test_log_parser.py`: Checks that the parser handles
  scheduler logs and interleaved console output correctly.

- `tests/fixtures/interleaved-console.log`: Added sample output
  used by the log parser tests.

- `runner.sh`: Starts the workload runs. Uses 10 repetitions
  by default, or the value supplied through N.

- `tests/run_workloads.py`: Runs the workloads under each policy
  and saves the logs by workload, policy, and repetition.

- `tests/validate_workloads.py`: Checks the log format, group
  membership, virtual runtime, progress between groups, and
  CPU allocation before and after changing shares.

- `tests/show_evidence.py`: Prints saved log excerpts for the
  documentation screenshots.

- `requirements.txt`: Lists the Python packages used by the
  test and plotting scripts.

- `results.py`: Reads the workload logs and creates nine plots
  covering context switches, response time, and turnaround time
  under the three policies. Also creates a summary CSV.

- `runner_logs/`: Added 420 workload logs from seven workloads,
  three policies, and 20 repetitions.

- `results/`: Added the generated plots and summary CSV.

- `README.md`: Added the group members, Phase 5 behavior, and
  explanations of the cumulative code changes from the starter.
