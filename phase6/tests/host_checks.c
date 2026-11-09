#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
typedef uint64 *pagetable_t;
#include "kernel/proc.h"

// Section 4.3.2: fail on double locks and enforce the documented lock order.
static int group_locks, process_locks, yields;
static struct proc proc[NPROC], *current, *initproc;
static struct group group[N_GROUPS];
static struct spinlock log_lock;
static int stcf_policy;
static uint ticks, log_start, log_duration;
static int assertions;
static char log_output[8192];
static int log_length;
#define CHECK(test) do { assertions++; if(!(test)) { \
  fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); abort(); } } while(0)

static void acquire(struct spinlock *lock)
{
  CHECK(!lock->locked);
  for(int i = 0; i < NPROC; i++)
    if(lock == &proc[i].lock){ CHECK(group_locks == 0); process_locks++; }
  for(int i = 0; i < N_GROUPS; i++)
    if(lock == &group[i].lock) group_locks++;
  lock->locked = 1;
}
static void release(struct spinlock *lock)
{
  CHECK(lock->locked);
  for(int i = 0; i < NPROC; i++)
    if(lock == &proc[i].lock) process_locks--;
  for(int i = 0; i < N_GROUPS; i++)
    if(lock == &group[i].lock) group_locks--;
  lock->locked = 0;
}
static void push_off(void) { }
static void pop_off(void) { }
static uint read_current_tick(void) { return ticks; }
static struct proc *myproc(void) { return current; }
static void yield(void) { CHECK(!process_locks && !group_locks); yields++; }
static char *safestrcpy(char *to, const char *from, int n)
{
  int i;
  for(i = 0; i < n - 1 && from[i]; i++) to[i] = from[i];
  to[i] = 0;
  return to;
}
static int test_printf(char *format, ...)
{
  va_list args;
  va_start(args, format);
  int count = vsnprintf(log_output + log_length, sizeof(log_output) - log_length, format, args);
  va_end(args);
  CHECK(count >= 0 && log_length + count < (int)sizeof(log_output));
  log_length += count;
  return count;
}
#define printf test_printf
#include "tested_functions.inc"
#undef printf

// Sections 2.5, 4.1.5: exercise the actual two-tier Phase 4 helper interface.
static struct proc *selected_process(void)
{
  int gid = stcf_policy == 2 ? find_lowest_gid() : N_GROUPS;
  return gid < 0 ? 0 : find_shortest_process(gid);
}

static void reset(void)
{
  CHECK(!process_locks && !group_locks);
  memset(proc, 0, sizeof(proc));
  memset(group, 0, sizeof(group));
  for(int i = 0; i < N_GROUPS; i++) group[i].shares = DEFAULT_SHARES;
  current = &proc[0];
  initproc = &proc[NPROC - 1];
  yields = 0;
  ticks = 1;
  stcf_policy = 2;
}

static void ready(int slot, int gid, int burst, uint created)
{
  struct proc *p = &proc[slot];
  p->gid = gid;
  p->remaining_burst = p->total_burst = burst;
  p->created_tick = created;
  p->pid = slot + 1;
  p->dispatch_erbt = -1;
  acquire(&p->lock);
  proc_setstate(p, RUNNABLE);
  release(&p->lock);
}

int main(void)
{
  // Sections 2.2, 2.5: all eligibility transitions preserve CPU history.
  reset(); ready(0, 0, 100, 1);
  acquire(&current->lock);
  proc_setstate(current, RUNNING);
  release(&current->lock);
  scheduler_tick();
  CHECK(group[0].run_procs == 1 && group[0].total_ticks == 1);
  CHECK(current->cpu_ticks == 1 && current->remaining_burst == 99);
  uint64 history = group[0].vruntime;
  acquire(&current->lock);
  proc_setstate(current, SLEEPING);
  release(&current->lock);
  scheduler_tick();
  CHECK(group[0].run_procs == 0 && group[0].vruntime == history);
  CHECK(current->cpu_ticks == 1 && current->ready_ticks == 0);
  CHECK(kill(current->pid) == 0 && current->killed);
  CHECK(current->state == RUNNABLE && group[0].run_procs == 1);
  CHECK(kill(999) == -1);
  current->killed = 0;
  scheduler_tick();
  CHECK(current->tiq == 1 && current->ready_ticks == 1);
  CHECK(current->remaining_burst == 99 && group[0].total_ticks == 1);
  CHECK(proc_setgroup(7) == 0 && proc_setgroup(7) == 0);
  CHECK(group[0].run_procs == 0 && group[7].run_procs == 1);
  CHECK(group[0].vruntime == history && current->cpu_ticks == 1);
  acquire(&current->lock); proc_setstate(current, ZOMBIE); release(&current->lock);
  CHECK(group[7].run_procs == 0);
  puts("PASS: state counts, migration, sleep/wakeup/exit and historical charges");

  // Sections 2.3-2.5: group priority, ERBT, creation tick and PID ordering.
  reset(); ready(0, 1, 50, 2); ready(1, 2, 1, 1); ready(2, 1, 20, 1);
  group[2].vruntime = VRUNTIME_SCALE;
  CHECK(selected_process() == &proc[2]);
  group[1].vruntime_frac = 1; group[2].vruntime = 0;
  proc[1].remaining_burst = 100;
  CHECK(selected_process() == &proc[1]);
  group[1].vruntime_frac = 0; group[2].vruntime = VRUNTIME_SCALE;
  proc[1].remaining_burst = 1;
  stcf_policy = 1;
  CHECK(selected_process() == &proc[1]);
  stcf_policy = 2;
  proc[0].remaining_burst = 20;
  CHECK(selected_process() == &proc[2]);
  proc[0].created_tick = 1;
  CHECK(selected_process() == &proc[0]);
  proc[0].remaining_burst = 50;
  proc[0].tiq = 60;
  CHECK(stcf_erbt(&proc[0]) == 0 && selected_process() == &proc[0]);
  proc[0].tiq = 19; CHECK(stcf_erbt(&proc[0]) == 50);
  proc[0].tiq = 20; CHECK(stcf_erbt(&proc[0]) == 30);
  proc[0].tiq = 40; CHECK(stcf_erbt(&proc[0]) == 10);
  proc[0].tiq = ~0ULL; CHECK(stcf_erbt(&proc[0]) == 0);
  acquire(&proc[0].lock); proc_setstate(&proc[0], RUNNING); release(&proc[0].lock);
  CHECK(!stcf_should_preempt());
  proc[0].tiq = 0;
  CHECK(stcf_should_preempt());
  proc[0].dispatch_erbt = 0;
  CHECK(!stcf_should_preempt());
  scheduler_tick();
  CHECK(proc[0].dispatch_erbt == -1 && stcf_should_preempt());
  proc[0].remaining_burst = 50;
  proc[2].remaining_burst = 50;
  CHECK(!stcf_should_preempt());
  group[2].vruntime = 0; group[1].vruntime = VRUNTIME_SCALE;
  CHECK(stcf_should_preempt());
  acquire(&proc[1].lock); proc_setstate(&proc[1], SLEEPING); release(&proc[1].lock);
  CHECK(!stcf_should_preempt());
  puts("PASS: two-tier eligibility, group ties, ERBT boundaries, job ties and preemption");

  // Sections 3.3-3.6: validation, yield, high weights and budget semantics.
  reset(); ready(0, 0, 100, 0);
  CHECK(proc_setgroup(-1) == -1 && current->killed);
  current->killed = 0;
  CHECK(proc_setgroup(N_GROUPS) == -1 && current->killed);
  current->killed = 0;
  CHECK(proc_setshares(-1, 1) == -1 && proc_setshares(8, 1) == -1);
  CHECK(proc_setshares(0, 0) == -1 && proc_setshares(0, -1) == -1);
  CHECK(!current->killed && yields == 0);
  CHECK(proc_setshares(0, 2147483647) == 0 && yields == 1);
  acquire(&current->lock); proc_setstate(current, RUNNING); release(&current->lock);
  scheduler_tick(); CHECK(group[0].vruntime_frac > 0);
  history = group[0].vruntime;
  uint64 fractional_history = group[0].vruntime_frac;
  CHECK(proc_setshares(0, 100) == 0 && group[0].vruntime == history);
  CHECK(group[0].vruntime_frac == fractional_history);
  CHECK(group[0].total_ticks == 1);
  CHECK(proc_setburst(7) == 0 && current->remaining_burst == 7);
  CHECK(current->burst_consumed == 0 && current->cpu_ticks == 1);
  CHECK(proc_setburst(0) == -1 && current->killed);
  current->killed = 0; current->remaining_burst = 1;
  scheduler_tick(); CHECK(current->killed);
  current->killed = 0; current->remaining_burst = 1;
  stcf_policy = 0; scheduler_tick(); CHECK(!current->killed);
  stcf_policy = 2; strcpy(current->name, "sh"); current->remaining_burst = 1;
  scheduler_tick(); CHECK(!current->killed && current->remaining_burst == 7);
  CHECK(current->burst_consumed == 0);
  puts("PASS: validation, setshares yield/history, extreme shares, budget death and RR/shell guards");

  // Sections 2.5, 3.2: fractional accuracy and finite logging windows.
  reset(); ready(0, 0, 10000, 0);
  acquire(&current->lock); proc_setstate(current, RUNNING); release(&current->lock);
  for(int i = 0; i < 25; i++) scheduler_tick();
  CHECK(group[0].vruntime == 256 && group[0].vruntime_frac == 0);
  CHECK(group[0].total_ticks == 25);
  ticks = 100;
  CHECK(proc_schedlog(5) == 0 && log_duration == 5 && log_start == 100);
  CHECK(proc_schedlog(-1) == -1 && log_duration == 5);
  ticks = 102; CHECK(proc_schedlog(2) == 0 && log_start == 102);
  CHECK(proc_schedlog(0) == 0 && log_duration == 0);
  acquire(&current->lock); schedlog_emit(current); release(&current->lock);
  CHECK(!log_lock.locked && !process_locks && !group_locks);
  CHECK(log_length == 0);
  strcpy(current->name, "worker");
  CHECK(proc_schedlog(2) == 0);
  acquire(&current->lock); schedlog_emit(current); release(&current->lock);
  CHECK(strstr(log_output, "102|GROUP|0(100,256),[1]worker:4(9975,25)\n") != 0);
  CHECK(strstr(log_output, "102|GROUP|7(100,0)\n") != 0);
  int length = log_length;
  ticks = 104;
  acquire(&current->lock); schedlog_emit(current); release(&current->lock);
  CHECK(log_length == length);
  stcf_policy = 0; ticks = 105; proc_schedlog(2);
  acquire(&current->lock); schedlog_emit(current); release(&current->lock);
  CHECK(strstr(log_output, "105|STCF|0(0,0),[1]worker:4(9975,25)\n") != 0);
  puts("PASS: fractional runtime, log replacement/stop/negative validation");

  // Sections 2.3, 2.5: a group-runtime tie must not hide a shorter local job.
  reset(); ready(0, 2, 50, 1); ready(1, 2, 10, 2); ready(2, 1, 1000, 3);
  acquire(&current->lock); proc_setstate(current, RUNNING); release(&current->lock);
  CHECK(stcf_should_preempt());
  printf("PASS: equal-runtime remote group preemption regression (%d checked assertions)\n", assertions);
  // check the boundary, idle reset and both zero-cpu cases
  reset(); ready(0, 1, 1000, 0); ready(1, 1, 1000, 0);
  acquire(&proc[1].lock); proc_setstate(&proc[1], SLEEPING); release(&proc[1].lock);
  group[1].total_ticks = 5;
  group[1].io_ticks = 8;
  ticks = 9; scheduler_tick();
  CHECK(group[1].io_ticks == 9 && group[1].shares == 100);
  ticks = 10; scheduler_tick();
  CHECK(group[1].io_ticks == 10 && group[1].shares == 200);
  CHECK(group[1].total_ticks == 5);
  history = group[1].vruntime;
  group[1].total_ticks = 6;
  ticks = 20; scheduler_tick();
  CHECK(group[1].shares == 100 && group[1].vruntime == history);
  group[1].total_ticks = 0;
  ticks = 30; scheduler_tick(); CHECK(group[1].shares == 200);
  acquire(&proc[1].lock); proc_setstate(&proc[1], ZOMBIE); release(&proc[1].lock);
  group[1].io_ticks = 0;
  ticks = 40; scheduler_tick(); CHECK(group[1].shares == 100);
  group[1].io_ticks = 100;
  acquire(&proc[0].lock); proc_setstate(&proc[0], SLEEPING); release(&proc[0].lock);
  ticks = 41; scheduler_tick(); CHECK(group[1].io_ticks == 0);
  CHECK(group[1].vruntime == history);
  ready(2, 1, 1000, 0);
  for(int policy = 0; policy < 2; policy++){
    stcf_policy = policy; group[1].shares = 300;
    ticks = 50; scheduler_tick(); CHECK(group[1].shares == 300);
  }
  puts("PASS: Phase 6 interval, inclusive ratio, restore, zero CPU, idle reset and policy guards");
  return 0;
}
