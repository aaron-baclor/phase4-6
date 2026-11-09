# Phase 4

## Group members

- BACLOR, Aaron Jori B. - 2023-09868 - THY
- FELIX, Raphael Anton G. - 2023-00226 - THY
- GARCIA, Julian Simon L. - 2023-21034 - THY
- MALAGAR, Juan Antonio Miguel M. - 2022-05081 - THY
- ORFIANO, Zachary Claude N. - 2020-11220 - THY

## Main features added

- Lorem ipsum.

## Per-file code changes

- `kernel/stcf.h`: Set `N_GROUPS` to 4
- `kernel/proc.h`:
  - static group assignment
  - `struct group` definition
- `kernel/proc.c`:
  - global variable for the group table array
  - group table initialization in `procinit()`
  - assign init and sh to group 0
  - updates to group `run_proc` count in `userinit()`, `fork()`, `sleep()`, `wakeup()`, `exit()`, `kill()`
  - updates to group `total_ticks` and `vruntime` in `scheduler_tick()`
  - new `find_lowest_gid()` helper returns ID of lowest eligible group
  - modified `scheduler()` to use group selection for `GROUP_STCF`
  - modified `find_shortest_process()` to receive a group ID and restrict process search within that group
  - modified `stcf_should_preempt()` to include grouped preemption
