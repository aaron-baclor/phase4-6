# Phase 4

## Group members

| Name | Student number | Section |
| --- | --- | --- |
| BACLOR, Aaron Jori B. | 2023-09868 | THY |
| FELIX, Raphael Anton G. | 2023-00226 | THY |
| GARCIA, Julian Simon L. | 2023-21034 | THY |
| MALAGAR, Juan Antonio Miguel M. | 2022-05081 | THY |
| ORFIANO, Zachary Claude N. | 2020-11220 | THY |

## What we added

For Phase 4, we added scheduling between process groups using
GROUP_STCF. The scheduler first chooses a group, then chooses a
process from that group.

There are four groups, each with 100 shares. Processes are assigned
using pid % N_GROUPS. Init and its direct children are placed in
Group 0.

Under GROUP_STCF, the scheduler picks the group with a runnable
process and the lowest virtual runtime. If two groups have the
same virtual runtime, the lower group ID goes first.

Within the selected group, the scheduler picks the runnable process
with the smallest effective remaining burst time. Ties use the
earlier creation tick, then the lower PID.

RR is still the default policy. STCF continues to choose processes
across the whole process table without selecting a group first.

Group membership and shares are fixed in this phase. The
setgroup(), groupfork(), and setshares() system calls are not
implemented yet.

## Process counters and budgets

Each process starts with a default budget of 100 ticks.

We keep fields for:

- Creation tick
- Total burst budget
- Remaining burst budget
- Ticks consumed from the current budget
- Lifetime CPU ticks
- Lifetime ticks spent runnable and waiting
- Time in the ready queue for aging
- Effective remaining burst when selected
- Group ID

Running processes gain CPU ticks, while runnable processes gain
ready ticks and time in the ready queue. Sleeping processes gain
neither.

Sleeping and ordinary yields keep the remaining budget.
Calling setburst() replaces the budget and resets its consumed
ticks, but keeps the lifetime counters.

Under STCF and GROUP_STCF, ordinary processes are marked killed
when their budget runs out. Init and processes named exactly sh
receive a fresh budget so the system and shell can keep running.

RR keeps its normal timer-based switching and does not kill
processes for running out of budget.

Pressing Ctrl+P shows the process list with the added counters.

## Aging and preemption

We keep aging from Phase 3. A process waiting in the ready queue
gets a lower effective remaining burst time:

```text
ERBT = max(0, remaining_burst - floor(tiq / STCF_AT) * STCF_AD)
```

STCF_AT and STCF_AD are both 20.

The waiting counter resets when the process is selected. We keep
its selected ERBT in dispatch_erbt until its first CPU tick so
resetting the counter does not immediately take away its turn.
Calling setburst() also clears this saved value.

Under STCF, a runnable process with a smaller ERBT can preempt
the running process.

Under GROUP_STCF, the running process yields when another eligible
group has a lower virtual runtime, or an equal virtual runtime
with a lower group ID. It also yields when a runnable process
in its own group has a smaller ERBT.

These checks happen at trap boundaries.

## Group counters

Each group tracks:

- Shares
- Number of runnable and running processes
- Total CPU ticks used by its processes
- Virtual runtime

The group's ID is its position in the group array.

Virtual runtime is calculated using:

```text
vruntime = total_ticks * 1024 / shares
```

A group gains CPU ticks when one of its processes runs.
Its accumulated CPU ticks and virtual runtime are not reset
when the group becomes inactive.

The runnable and running count is updated when processes enter
or leave those states. The scheduler checks for runnable
processes when deciding which groups can be selected.

When both locks are needed, the process lock is taken before
the group lock. We avoid holding two group locks at once and
release group locks before switching processes.

## System calls

### setburst(int ticks)

System call number: 24.

A positive value sets the calling process's total and remaining
budget to that value, clears its current consumed ticks, and yields
so the scheduler can pick again.

A zero or negative value marks the caller killed.

### schedlog(int ticks)

System call number: 23.

A positive value starts a shared logging window for that many ticks.
Calling it again replaces the current window. Zero stops logging,
and a negative value returns -1.

While logging is active, the scheduler prints the process information
before switching to the selected process. The selected process is
already marked RUNNING when the entry is printed.

Under GROUP_STCF, it prints one line for each group in group ID order:

```text
<tick>|GROUP|<gid>(<shares>,<vruntime>),[<pid>]<name>:<state>(<remaining>,<consumed>),...
```

Virtual runtime is printed with two decimal places.
For example, one CPU tick at 100 shares prints 10.24.

RR and STCF keep the earlier single-line format:

```text
<tick>|STCF|0(0,0),[<pid>]<name>:<state>(<remaining>,<consumed>),...
```

The state numbers are:

| State | Number |
| --- | --- |
| USED | 1 |
| SLEEPING | 2 |
| RUNNABLE | 3 |
| RUNNING | 4 |
| ZOMBIE | 5 |

The existing shutdown system call keeps number 22.

## Code changes

This list covers the scheduler changes and supporting files
included in this phase.

| File | What we changed |
| --- | --- |
| `.gitignore` | Excludes compiled files and local test outputs. |
| `Makefile` | Supports RR, STCF, and GROUP_STCF, rebuilds when the policy or tracing option changes, sets the RISC-V architecture flags, and includes the test programs. |
| `kernel/stcf.h` | Defines the default burst, aging constants, four groups, and default shares. |
| `kernel/param.h` | Includes stcf.h and uses RR when no policy is specified. |
| `kernel/main.c` | Prints the selected policy during boot. |
| `kernel/proc.h` | Adds the process counters, aging fields, group ID, and group structure. |
| `kernel/proc.c` | Handles group assignment, group and process selection, preemption, aging, tick accounting, budgets, scheduler logging, and the extra Ctrl+P output. Keeps the RR process-table traversal. |
| `kernel/trap.c` | Counts ticks before waking sleepers and checks whether STCF or GROUP_STCF should preempt. Keeps normal timer yields for RR. |
| `kernel/defs.h` | Declares the scheduler and syscall helper functions. |
| `kernel/syscall.h` | Defines the syscall numbers for schedlog and setburst. |
| `kernel/syscall.c` | Connects the syscall numbers to their handlers. |
| `kernel/sysproc.c` | Reads syscall arguments and calls the helpers in proc.c. |
| `user/user.h` | Declares schedlog and setburst for user programs. |
| `user/usys.pl` | Generates the user-side syscall stubs. |
| `user/workload.h` | Provides shared computation and logging helpers for the workloads. |
| `user/test_cpu.c` | Runs a CPU-bound workload. |
| `user/test_io.c` | Alternates short computations with sleep calls. |
| `user/test_f_cpu.c` | Creates four CPU-bound children. |
| `user/test_f_io.c` | Creates four children that alternate computation and sleep. |
| `user/test_bg.c` | Alternates longer computations with short sleeps. |
| `user/test_group_iso.c` | Checks that a group with many short processes does not starve another group. |
| `user/test_phase4.c` | Checks the syscalls, budgets, fork and exec behavior, preemption, and aging. |
| `sanity_check.sh` | Runs the sanity checks with one virtual CPU. |
| `sanity_check.py` | Runs the host checks, log parser tests, and xv6 checks under all three policies. |
| `tests/host_checks.py` | Compiles selected scheduler functions for testing on the host. |
| `tests/host_checks.c` | Checks selection, preemption, aging, accounting, locking, and syscall helper behavior. |
| `tests/qemu_tools.py` | Starts QEMU sessions and reads scheduler logs. |
| `tests/test_log_parser.py` | Checks that the parser handles scheduler logs correctly. |
| `tests/fixtures/interleaved-console.log` | Provides sample output for the parser tests. |
| `tests/run_workloads.py` | Runs each workload under each policy and saves its logs. |
| `tests/validate_workloads.py` | Checks log formatting, group membership, virtual runtime, and group progress. |
| `runner.sh` | Starts the workload runs, using 10 repetitions by default or the value of N. |
| `requirements.txt` | Lists the Python packages used by the scripts. |
| `results.py` | Reads the logs and produces the plots and summary CSV. |
| `README.md` | Lists the group members, scheduler behavior, commands, and code changes. |

The starter's lazy memory allocation, trap return path, and shutdown
support were kept. The filesystem builder in mkfs/mkfs.c is unchanged.

The original README is still used by the Makefile when building
fs.img. README.md contains our project notes.

## Running and testing

Run these commands from the phase's root directory.

For RR:

```sh
CPUS=1 make qemu
```

For STCF:

```sh
CPUS=1 make qemu SCHEDULING_POLICY=STCF
```

For GROUP_STCF:

```sh
CPUS=1 make qemu SCHEDULING_POLICY=GROUP_STCF
```

For the test scripts and plots:

```sh
uv venv cs140-project1
source cs140-project1/bin/activate
uv pip install -r requirements.txt
./sanity_check.sh
N=20 ./runner.sh
python3 results.py
```

The scripts use one virtual CPU. They need GNU Make, host GCC,
a RISC-V GCC toolchain, QEMU, and the Python packages listed in
requirements.txt.

The runner saves logs under runner_logs/, grouped by workload
and policy. For this phase, 6 workloads under 3 policies with
20 repetitions produce 360 logs.

results.py creates nine plots under results/: context switches,
response time, and turnaround time for each policy. It also
creates summary.csv.

Context switches count dispatches into the workload's process
family. Response time is measured from creation to first dispatch,
and turnaround time is measured from creation to exit.
