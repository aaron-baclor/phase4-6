# Phase 6

## Group members

- BACLOR, Aaron Jori B. - 2023-09868 - THY
- FELIX, Raphael Anton G. - 2023-00226 - THY
- GARCIA, Julian Simon L. - 2023-21034 - THY
- MALAGAR, Juan Antonio Miguel M. - 2022-05081 - THY
- ORFIANO, Zachary Claude N. - 2020-11220 - THY

## What we added

We added automatic shares boosting for groups that spend more
time waiting on I/O compared to running on the CPU. We kept the
process counters, preemption, aging, dynamic groups, and system
calls from the earlier phases.

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
in Group 0. An ordinary fork() child inherits its parent's group.

setgroup() moves the calling process to another group, groupfork()
creates a child directly in a chosen group, and setshares()
changes a group's shares. Calling exec() keeps the process's group.

Automatic boosting applies only under GROUP_STCF. RR and STCF
keep their existing scheduling behavior.

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
neither, but qualifying sleeping ticks count toward their group's
I/O ticks.

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
- I/O ticks
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

## Automatic shares boosting

We added these constants:

- SHARES_BOOSTING_RATIO: 2
- BOOSTED_SHARES: 200
- SHARES_BOOSTING_INTERVAL: 10

Each timer tick, a process sleeping on a channel other than itself
adds one I/O tick to its group. We use this to count sleep calls
and other blocked waits, while excluding wait() for a child.

Under GROUP_STCF, we check every group's shares every 10 ticks.
If its I/O ticks are at least twice its total CPU ticks, its shares
are set to 200. Otherwise, its shares are set to 100.

If a group has no CPU ticks but has positive I/O ticks, it receives
200 shares. If both counters are zero, it receives 100 shares.

After the shares decision, a group with no runnable or running
processes has its I/O ticks reset to zero. Its CPU ticks and
virtual runtime are kept.

Our implementation treats waiting for a child as separate from
I/O. It also resets inactive groups' I/O ticks at the 10-tick
check, after deciding their shares, rather than on every tick.
These are the interpretations used for this phase.

Excluding wait() prevents a parent waiting for CPU-bound children
from making the group look I/O-heavy. Resetting at the interval
allows a group whose members are all sleeping to accumulate
I/O ticks between checks.

The CPU counter used in the comparison is the group's lifetime
total. Only the I/O counter is cleared by the inactive-group reset.

All five custom system calls remain available. Under GROUP_STCF,
a manual setshares() value lasts until the next automatic check.
Under RR and STCF, automatic boosting does not replace it.

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
rather than rounded. Changes in shares appear in the group
information printed by schedlog.

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

Under GROUP_STCF, the next automatic boosting check replaces
this value with either 100 or 200.

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
  header dependency, and RISC-V architecture flags. Includes
  test_sched and test_group_sh.

- `kernel/stcf.h`: Added the default burst, aging constants,
  N_GROUPS set to 8, DEFAULT_SHARES set to 100, and the scale
  used for fractional virtual runtime. Added the boosting ratio,
  boosted shares, and boosting interval constants.

- `kernel/param.h`: Included stcf.h and used RR when no policy
  is specified.

- `kernel/main.c`: Added the selected policy to the boot output.

- `kernel/proc.h`: Added the process budget, creation tick,
  lifetime counters, aging counter, saved dispatch ERBT, and
  group ID. Added the group structure for shares, runnable and
  running counts, CPU ticks, I/O ticks, and virtual runtime.
  Added fields for fractional virtual runtime and its remainder.

- `kernel/proc.c`: Added policy setup, counter initialization and
  cleanup, process selection, tick accounting, budget handling,
  syscall helpers, scheduler logging, and the extra Ctrl+P output.
  Added aging, saved dispatch ERBT, group-first selection, and
  group-aware preemption. Added parent-group inheritance,
  fork_into_group(), groupfork(), proc_setgroup(), proc_setshares(),
  and proc_setstate(). Added fractional virtual-runtime accounting.
  Added I/O tick accounting that excludes wait(), automatic shares
  updates every 10 ticks under GROUP_STCF, and inactive-group I/O
  resets after the interval's decision. Added optional TRACE
  output for testing. Kept the RR process-table traversal.

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

- `user/test_group_sh.c`: Runs one CPU worker in Group 1 and
  one CPU worker with four sleeping workers in Group 2.
  Checks automatic boosting while both groups remain eligible.

- `user/test_sched.c`: Added checks for syscall arguments,
  group inheritance, groupfork placement, group preservation
  across exec, budgets, preemption, aging, and scheduler logging.

- `sanity_check.sh`: Runs the sanity checks with one virtual CPU.

- `sanity_check.py`: Runs the log parser tests, host checks,
  and xv6 checks under RR, STCF, and GROUP_STCF. Also checks
  that Group 2 reaches 200 shares in a fresh GROUP_STCF guest.

- `tests/__init__.py`: Makes the test helpers importable as
  a Python package.

- `tests/host_checks.py`: Extracts scheduler functions from proc.c
  and compiles them for testing on the host.

- `tests/host_checks.c`: Checks selection, preemption, aging,
  group counts and movement, locking, logging, budgets, shares,
  and fractional virtual runtime. Added checks for the boosting
  interval, exact ratio threshold, restoration to default shares,
  zero-CPU cases, inactive-group resets, and the RR/STCF guard.
  Also checks that a sleeping group can earn a boost and that
  wait() does not add I/O ticks.

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
  membership, virtual runtime, and progress between groups.
  For test_group_sh, checks for 100:200 shares and that the boosted
  group receives at least as many CPU ticks during the observed
  boosted period.

- `requirements.txt`: Lists the Python packages used by the
  test and plotting scripts.

- `results.py`: Reads the workload logs and creates nine plots
  covering context switches, response time, and turnaround time
  under the three policies. Uses phase6 in the plot filenames
  and also creates a summary CSV.

- `runner_logs/`: Added 420 workload logs from seven workloads,
  three policies, and 20 repetitions using the Phase 6 kernel.

- `results/`: Added the generated plots and summary CSV.

- `README.md`: Added the group members, Phase 6 behavior,
  boosting interpretations, and explanations of the cumulative
  code changes from the starter.
