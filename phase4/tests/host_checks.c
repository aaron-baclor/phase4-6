#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
typedef uint64 *pagetable_t;
#include "kernel/proc.h"

static struct proc proc[NPROC], *current, *initproc;
static struct group group[N_GROUPS];
static int stcf_policy, failures, tests, yields;
static uint ticks, log_start, log_duration;
static struct spinlock log_lock;
static char output[16384];
static int length;
#define GID gid

static void acquire(struct spinlock *lock) { if(lock->locked) abort(); lock->locked = 1; }
static void release(struct spinlock *lock) { if(!lock->locked) abort(); lock->locked = 0; }
static void push_off(void) { }
static void pop_off(void) { }
static uint read_current_tick(void) { return ticks; }
static struct proc *myproc(void) { return current; }
static void yield(void) { yields++; }
static char *safestrcpy(char *to, const char *from, int n)
{
  int i;
  for(i = 0; i < n - 1 && from[i]; i++) to[i] = from[i];
  to[i] = 0;
  return to;
}
static int capture(char *format, ...)
{
  va_list args;
  va_start(args, format);
  int count = vsnprintf(output + length, sizeof(output) - length, format, args);
  va_end(args);
  if(count < 0 || count + length >= (int)sizeof(output)) abort();
  length += count;
  return count;
}
#define printf capture
#include "paired_functions.inc"
#undef printf

static void expect(int value, char *name)
{
  tests++;
  if(!value) failures++;
  printf("%s: %s\n", value ? "PASS" : "FAIL", name);
}
static void reset(void)
{
  memset(proc, 0, sizeof(proc)); memset(group, 0, sizeof(group));
  for(int i = 0; i < N_GROUPS; i++) group[i].shares = DEFAULT_SHARES;
  current = &proc[0]; initproc = &proc[NPROC - 1];
  stcf_policy = 2; ticks = 10; length = 0; output[0] = 0;
  for(int i = 0; i < NPROC; i++){
    proc[i].pid = i + 1;
    proc[i].dispatch_erbt = -1;
  }
}
static void process(int slot, int gid, enum procstate state, int burst)
{
  proc[slot].GID = gid; proc[slot].state = state;
  proc[slot].total_burst = proc[slot].remaining_burst = burst;
  if(state == RUNNABLE || state == RUNNING) group[gid].run_procs++;
}
static struct proc *selected(void)
{
  int gid = find_lowest_gid();
  return gid < 0 ? 0 : find_shortest_process(gid);
}
int main(void)
{
  reset(); process(0, 0, RUNNING, 500); process(1, 1, RUNNABLE, 30);
  process(2, 2, RUNNABLE, 1);
  group[0].vruntime = 0; group[1].vruntime = 10; group[2].vruntime = 20;
  expect(selected() == &proc[1], "eligible group precedes job burst; running-only group excluded");
  group[2].vruntime = 10;
  expect(selected() == &proc[1], "equal-runtime group tie selects lower group ID");
  proc[1].tiq = 19; expect(stcf_erbt(&proc[1]) == 30, "aging below boundary");
  proc[1].tiq = 20; expect(stcf_erbt(&proc[1]) == 10, "aging at boundary");
  proc[1].tiq = 40; expect(stcf_erbt(&proc[1]) == 0, "aging saturation");

  reset(); process(0, 2, RUNNING, 50); process(1, 2, RUNNABLE, 10);
  expect(stcf_should_preempt(), "strictly shorter same-group process preempts");
  process(2, 1, RUNNABLE, 1000);
  expect(stcf_should_preempt(), "equal-runtime remote group does not mask a required preemption");
  group[1].vruntime = 20; group[2].vruntime = 30;
  expect(stcf_should_preempt(), "lower-runtime remote group preempts despite longer job");

  reset(); process(0, 2, RUNNING, 50); process(1, 2, RUNNABLE, 10);
  current->dispatch_erbt = 0;
  expect(!stcf_should_preempt(), "aged selection is not immediately undone after TIQ reset");

  reset(); process(0, 1, RUNNING, 1000);
  for(int i = 0; i < 25; i++) scheduler_tick();
  expect(group[1].total_ticks == 25 && group[1].vruntime == 256,
         "25 charged ticks at 100 shares retain exact whole runtime");
  expect(current->cpu_ticks == 25 && current->remaining_burst == 975,
         "per-process execution and group CPU accounting agree");

  reset(); process(1, 2, SLEEPING, 50); proc[1].chan = &ticks;
  wakeup(&ticks);
  expect(proc[1].state == RUNNABLE && group[2].run_procs == 1,
         "wakeup restores one eligible group member");
  proc[1].state = SLEEPING; group[2].run_procs = 0;
  expect(kill(proc[1].pid) == 0 && proc[1].killed && group[2].run_procs == 1,
         "kill of sleeper restores eligibility without losing membership");

  reset(); process(0, 2, RUNNING, 50); strcpy(current->name, "review");
  proc_schedlog(3);
  acquire(&current->lock); schedlog_emit(current); release(&current->lock);
  expect(strstr(output, "|GROUP|0(100,0.00)") != 0,
         "GROUP_STCF logger emits required GROUP headers");
  stcf_policy = 0; length = 0; output[0] = 0;
  acquire(&current->lock); schedlog_emit(current); release(&current->lock);
  expect(strstr(output, "10|STCF|0(0,0)") != 0, "RR logger retains STCF compatibility format");
  expect(proc_schedlog(-1) == -1 && log_duration == 3, "negative schedlog preserves active window");
  expect(proc_schedlog(0) == 0 && !log_duration, "zero schedlog stops logging");

  reset(); process(0, 1, RUNNING, 50);
  current->cpu_ticks = 5;
  expect(proc_setburst(7) == 0 && current->remaining_burst == 7 && current->cpu_ticks == 5,
         "positive setburst preserves lifetime accounting and yields");
  expect(yields == 1 && proc_setburst(0) == -1 && current->killed,
         "invalid setburst kills the caller");
  printf("RESULT: %d/%d checks passed; %d failures\n", tests - failures, tests, failures);
  return failures ? 1 : 0;
}
