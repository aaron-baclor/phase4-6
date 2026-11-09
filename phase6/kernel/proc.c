#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct group group[N_GROUPS];

struct proc proc[NPROC];

struct proc *initproc;

// 0 = RR, 1 = STCF, 2 = GROUP_STCF.
static int stcf_policy;

// One shared log window.
static struct spinlock log_lock;
static uint log_start;
static uint log_duration;

static void schedlog_emit(struct proc *);

static void
proc_setstate(struct proc *p, enum procstate state)
{
  int was_ready = p->state == RUNNABLE || p->state == RUNNING;
  int is_ready = state == RUNNABLE || state == RUNNING;

  if(was_ready != is_ready){
    struct group *g = &group[p->gid];
    acquire(&g->lock);
    g->run_procs += is_ready - was_ready;
    release(&g->lock);
  }
  p->state = state;
}

// Avoid taking tickslock while holding a process lock.
static uint
read_current_tick(void)
{
  return __atomic_load_n(&ticks, __ATOMIC_RELAXED);
}

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table and proc group table
void
procinit(void)
{
  struct proc *p;
  struct group *g;

  // Pick the policy set in the Makefile.
  if(strncmp(SCHEDULING_POLICY, "RR", 3) == 0)
    stcf_policy = 0;
  else if(strncmp(SCHEDULING_POLICY, "STCF", 5) == 0)
    stcf_policy = 1;
  else if(strncmp(SCHEDULING_POLICY, "GROUP_STCF", 11) == 0)
    stcf_policy = 2;
  else
    panic("Unknown scheduling policy");

  if(DEFAULT_STCF_BURST <= 0)
    panic("DEFAULT_STCF_BURST must be positive");

  initlock(&log_lock, "schedlog");
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");

  for(p = proc; p < &proc[NPROC]; p++){
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }

  // initialize proc group table
  for(g = group; g < &group[N_GROUPS]; g++){
    initlock(&g->lock, "group");
    g->shares = DEFAULT_SHARES;
    g->run_procs = 0;    
    g->io_ticks = 0;
    g->total_ticks = 0;  
    g->vruntime = 0;
    g->vruntime_frac = 0;
    g->vruntime_rem = 0;     
  }

}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // New process, fresh budget.
  p->created_tick = read_current_tick();
  p->total_burst = DEFAULT_STCF_BURST;
  p->remaining_burst = DEFAULT_STCF_BURST;
  p->burst_consumed = 0;
  p->cpu_ticks = 0;
  p->ready_ticks = 0;
  // Section 4.1.5: bootstrap Group 0; fork sets the final membership.
  p->gid = 0;
  p->tiq = 0; // Initialize tiq
  p->dispatch_erbt = -1;

  // Section 4.3: optional evidence, independent of schedlog format.
#ifdef SCHED_TEST_TRACE
  struct proc *parent = myproc();
  printf("TRACE|CREATE|%d|%u|%d\n", p->pid, p->created_tick, parent ? parent->pid : 0);
#endif

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  // Clear these before the slot gets reused.
  p->created_tick = 0;
  p->total_burst = 0;
  p->remaining_burst = 0;
  p->burst_consumed = 0;
  p->cpu_ticks = 0;
  p->ready_ticks = 0;
  p->tiq = 0;
  p->dispatch_erbt = -1;
  p->gid = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");

  // Sections 2.5, 4.1.5: publish init in the default system group.
  p->gid = 0;
  proc_setstate(p, RUNNABLE);


  release(&p->lock);
}

// Shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
shrinkproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  if(n > p->sz)
    return -1;

  sz = p->sz;
  sz = uvmdealloc(p->pagetable, sz, sz - n);
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
static int
fork_into_group(int gid)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // Sections 3.5, 4.1.5: initialize membership before RUNNABLE.
  np->gid = gid;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  proc_setstate(np, RUNNABLE);


  release(&np->lock);

  return pid;
}

// Section 4.1.5: inherit the parent group on ordinary fork.
int
fork(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  int gid = p->gid;
  release(&p->lock);
  return fork_into_group(gid);
}

// Section 3.5: reject invalid groups before allocating a child.
int
groupfork(int gid)
{
  if(gid < 0 || gid >= N_GROUPS){
    setkilled(myproc());
    return -1;
  }
  return fork_into_group(gid);
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  // Section 4.3.4: optional exact completion evidence.
#ifdef SCHED_TEST_TRACE
  printf("TRACE|EXIT|%d|%u|%d\n", p->pid, read_current_tick(), status);
#endif
  proc_setstate(p, ZOMBIE);


  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Starvation mitigation mechanism
static int
stcf_erbt(struct proc *p)
{
    // Calculates ⌊ TIQ / AT ⌋
    uint64 discounts = p->tiq / STCF_AT;
    if(p->remaining_burst <=0)
        return 0;

    // Maximum possible discount
    uint64 max_discounts =
        ((uint64)p->remaining_burst + STCF_AD - 1) /STCF_AD;

    // max(0, ...) implementation
    if(discounts >= max_discounts)
        return 0;

    // RBT - (⌊ TIQ/AT ⌋ × AD)
    return p->remaining_burst - (int)(discounts * STCF_AD);
}

// Find the shortest job, then break ties by age and PID.
// If gid == N_GROUPS, search all groups; otherwise, restrict it to that groupID
static struct proc*
find_shortest_process(int gid)
{
  struct proc *best = 0;

  // Change to erbt to avoid confusion
  // Tracks the best erbt
  int best_erbt = 0;
  uint best_created = 0;
  int best_pid = 0;

  for(int i = 0; i < NPROC; i++){
    struct proc *p = &proc[i];
    acquire(&p->lock);

    // Restrict process search within the group
    if(gid != p->gid && gid != N_GROUPS){
      release(&p->lock);
      continue;
    }

    if(p->state == RUNNABLE){
      int choose_this = 0;

      // Change to the erbt method
      int remaining = stcf_erbt(p);

      // Phase 3: use the remaining time adjusted for aging here.
      // Used the updated best erbt
      if(best == 0)
        choose_this = 1;
      else if(remaining < best_erbt)
        choose_this = 1;
      else if(remaining == best_erbt){
        if(p->created_tick < best_created)
          choose_this = 1;
        else if(p->created_tick == best_created){
          if(p->pid < best_pid)
            choose_this = 1;
        }
      }

      if(choose_this){
        best = p;
        best_erbt = remaining;
        best_created = p->created_tick;
        best_pid = p->pid;
      }
    }

    release(&p->lock);
  }

  return best;
}

// Find groupID with the lowest vruntime, break ties with lower groupID
// Return -1 if no eligible groups
static int
find_lowest_gid(void)
{
  int eligible[N_GROUPS]; // if eligible[i] is 1, group i is eligible
  int best_gid = N_GROUPS; // groupID with lowest vruntime; if best_gid == N_GROUPs, it's not initialized yet
  uint64 lowest_vruntime = 0;
  uint64 lowest_fraction = 0;
  
  for(int i = 0; i < N_GROUPS; i++)
    eligible[i] = 0;

  // scan through process table, tracking eligible groups
  for(int i = 0; i < NPROC; i++){
    struct proc *p = &proc[i];
    
    acquire(&p->lock);

    if(p->state == RUNNABLE)
      eligible[p->gid] = 1;
    
    release(&p->lock);
  }

  // checks if at least 1 group eligible; else return -1
  int exists_eligible = 0;
  for(int i = 0; i < N_GROUPS; i++){
    if(eligible[i]) {
      exists_eligible = 1;
      break;
    }
  }

  if(!exists_eligible)
    return -1;

  // scan through group table, tracking groupID of lowest vruntime
  for(int i = 0; i < N_GROUPS; i++){
    if(eligible[i] != 1) 
      continue;

    struct group *g = &group[i];

    acquire(&g->lock);

    // Section 2.5: compare the retained fraction before the group ID tie.
    if(best_gid == N_GROUPS || g->vruntime < lowest_vruntime ||
       (g->vruntime == lowest_vruntime &&
        (g->vruntime_frac < lowest_fraction ||
         (g->vruntime_frac == lowest_fraction && i < best_gid)))){
      best_gid = i;
      lowest_vruntime = g->vruntime;
      lowest_fraction = g->vruntime_frac;
    }

    release(&g->lock);
  }

  return best_gid;
}

void
scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;
  int gid = N_GROUPS;

  for(;;){
    intr_on();
    intr_off();

    // RR: go through the process table as usual.
    if(!stcf_policy){
      int ran_process = 0;

      for(int i = 0; i < NPROC; i++){
        struct proc *p = &proc[i];
        acquire(&p->lock);

        if(p->state == RUNNABLE){
          proc_setstate(p, RUNNING);
          c->proc = p;

          schedlog_emit(p);
          // Section 4.3.4: optional exact dispatch evidence.
#ifdef SCHED_TEST_TRACE
          printf("TRACE|DISPATCH|%d|%u|%d\n", p->pid, read_current_tick(), p->gid);
#endif
          swtch(&c->context, &p->context);

          c->proc = 0;
          ran_process = 1;
        }

        release(&p->lock);
      }

      if(!ran_process)
        asm volatile("wfi");

      continue;
    } else if(stcf_policy == 2){ // GROUP_STCF: find best groupID and restrict process search to that group
      gid = find_lowest_gid();

      if(gid == -1){
        asm volatile("wfi");
        continue;
      }

    }

    // STCF: pick the shortest runnable process.
    push_off();
    struct proc *p = find_shortest_process(gid); 

    if(p == 0){
      pop_off();
      asm volatile("wfi");
      continue;
    }

    acquire(&p->lock);

    // Another CPU might have picked it already.
    if(p->state != RUNNABLE){
      release(&p->lock);
      pop_off();
      continue;
    }

    proc_setstate(p, RUNNING);
    c->proc = p;


    // Phase 3: reset its separate aging counter here.

    // Reset only the TIQ when the process is selected
    // Section 2.4: earned priority survives until the first charged tick.
    p->dispatch_erbt = stcf_erbt(p);
    p->tiq = 0;
    pop_off();

    schedlog_emit(p);
    // Section 4.3.4: optional exact dispatch evidence.
#ifdef SCHED_TEST_TRACE
    printf("TRACE|DISPATCH|%d|%u|%d\n", p->pid, read_current_tick(), p->gid);
#endif
    swtch(&c->context, &p->context);

    c->proc = 0;
    release(&p->lock);
  }
}

// trap.c uses this to check which policy we're running.
int
scheduling_stcf(void)
{
  return stcf_policy;
}

// Switch only if another ready process has less time left.
// Second modification is to switch if another ready process has a smaller ERBT
// GROUP_STCF: switch if another eligible group has lower vruntime
int
stcf_should_preempt(void)
{
  if(!stcf_policy)
    return 0;

  push_off();
  struct proc *current = myproc();
  if(current == 0){
    pop_off();
    return 0;
  }

  acquire(&current->lock);
  if(current->state != RUNNING){
    release(&current->lock);
    pop_off();
    return 0;
  }
  int current_erbt = stcf_erbt(current);
  int current_gid = current->gid;

  // Section 2.4: resetting TIQ must not undo an aged selection in a return trap.
  if(current->dispatch_erbt >= 0 && current->dispatch_erbt < current_erbt)
    current_erbt = current->dispatch_erbt;

  // Section 2.5: never nest locks for the running and candidate groups.
  uint64 current_vruntime = 0, current_fraction = 0;
  if(stcf_policy == 2){
    struct group *g = &group[current_gid];
    acquire(&g->lock);
    current_vruntime = g->vruntime;
    current_fraction = g->vruntime_frac;
    release(&g->lock);
  }
  release(&current->lock);

  // Section 2.5: keep Phase 4's group-first preemption and lower-ID tie order.
  if(stcf_policy == 2){
    int other_gid = find_lowest_gid();
    if(other_gid >= 0 && other_gid != current_gid){
      struct group *g = &group[other_gid];
      acquire(&g->lock);
      int lower = g->vruntime < current_vruntime ||
        (g->vruntime == current_vruntime &&
         (g->vruntime_frac < current_fraction ||
          (g->vruntime_frac == current_fraction && other_gid < current_gid)));
      release(&g->lock);
      if(lower){
        pop_off();
        return 1;
      }
    }
  }

  // Sections 2.3, 2.5: a remote group must not mask a shorter local process.
  int shorter_found = 0;
  for(int i = 0; i < NPROC; i++){
    struct proc *other = &proc[i];
    if(other == current)
      continue;
    acquire(&other->lock);
    if(other->state == RUNNABLE &&
       (stcf_policy != 2 || other->gid == current_gid) &&
       stcf_erbt(other) < current_erbt)
      shorter_found = 1;
    release(&other->lock);
    if(shorter_found)
      break;
  }
  pop_off();
  return shorter_found;
}

// Update the counters once per clock tick.
void
scheduler_tick(void)
{
  for(int i = 0; i < NPROC; i++){
    struct proc *p = &proc[i];
    struct group *g;

    acquire(&p->lock);

    if(p->state == RUNNABLE){
      p->ready_ticks++;
      p->tiq++; // Increase TIQ because process is waiting
      // Sections 2.4, 4.3: actual aging keys for test evidence.
#ifdef SCHED_TEST_TRACE
      printf("TRACE|READY|%d|%u|%lu|%d\n", p->pid, read_current_tick(), p->tiq, stcf_erbt(p));
#endif
    } else if(p->state == SLEEPING){
      g = &group[p->gid];
      acquire(&g->lock);
      g->io_ticks++;
      release(&g->lock);
    } else if(p->state == RUNNING){
      p->cpu_ticks++;
      p->burst_consumed++;
      p->dispatch_erbt = -1;
      // Section 4.3: optional charged-tick evidence.
#ifdef SCHED_TEST_TRACE
      printf("TRACE|TICK|%d|%u|%d\n", p->pid, read_current_tick(), p->gid);
#endif
      
      g = &group[p->gid];

      acquire(&g->lock); // update group total_ticks and vruntime
      g->total_ticks++;
      // Sections 2.5, 3.6: preserve past charges across weight updates.
      uint64 charge = 1024ULL * VRUNTIME_SCALE + g->vruntime_rem;
      uint64 fraction = g->vruntime_frac + charge / g->shares;
      g->vruntime += fraction / VRUNTIME_SCALE;
      g->vruntime_frac = fraction % VRUNTIME_SCALE;
      g->vruntime_rem = charge % g->shares;
      release(&g->lock);

      if(p->remaining_burst > 0)
        p->remaining_burst--;

      if(stcf_policy && p->remaining_burst == 0){
        // Keep init and the shell alive.
        if(p == initproc ||
           strncmp(p->name, "sh", sizeof(p->name)) == 0){
          p->remaining_burst = p->total_burst;
          p->burst_consumed = 0;
        } else {
          p->killed = 1;
        }
      }
    }

    release(&p->lock);
  }

  for(int i = 0; i < N_GROUPS; i++){
    struct group *g = &group[i];
    acquire(&g->lock);
    if(g->run_procs == 0)
      g->io_ticks = 0;
    if(stcf_policy == 2 && read_current_tick() % SHARES_BOOSTING_INTERVAL == 0){
      int shares = DEFAULT_SHARES;
      // integer division avoids overflow and keeps the threshold inclusive
      if(g->io_ticks > 0 &&
         (g->total_ticks == 0 ||
          g->io_ticks / SHARES_BOOSTING_RATIO >= g->total_ticks))
        shares = BOOSTED_SHARES;
      g->vruntime_rem = g->vruntime_rem * shares / g->shares;
      g->shares = shares;
    }
    release(&g->lock);
  }
}

// Set a fresh budget, then let the scheduler pick again.
int
proc_setburst(int burst_ticks)
{
  struct proc *p = myproc();
  acquire(&p->lock);

  if(burst_ticks <= 0){
    p->killed = 1;
    release(&p->lock);
    return -1;
  }

  p->total_burst = burst_ticks;
  p->remaining_burst = burst_ticks;
  p->burst_consumed = 0;
  // Sections 2.4, 3.3: reevaluate a new budget independently.
  p->dispatch_erbt = -1;

  release(&p->lock);
  yield();

  return 0;
}

// Section 3.4: dynamic membership with ordered group accounting.
int
proc_setgroup(int gid)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  if(gid < 0 || gid >= N_GROUPS){
    p->killed = 1;
    release(&p->lock);
    return -1;
  }

  // Sections 2.5, 3.4: order both group locks by ID for simultaneous moves.
  int old = p->gid;
  if(old != gid){
    int first = old < gid ? old : gid;
    int second = old < gid ? gid : old;
    acquire(&group[first].lock);
    acquire(&group[second].lock);
    if(p->state == RUNNABLE || p->state == RUNNING){
      group[old].run_procs--;
      group[gid].run_procs++;
    }
    p->gid = gid;
    release(&group[second].lock);
    release(&group[first].lock);
  }
  release(&p->lock);
  return 0;
}

// Section 3.6: preserve history and yield after updating shares.
int
proc_setshares(int gid, int shares)
{
  if(gid < 0 || gid >= N_GROUPS || shares <= 0)
    return -1;

  struct group *g = &group[gid];
  acquire(&g->lock);
  // Preserve the sub-fixed-point fraction when its denominator changes.
  g->vruntime_rem = g->vruntime_rem * shares / g->shares;
  g->shares = shares;
  release(&g->lock);

  // Section 3.6: reevaluate both tiers immediately after the update.
  yield();
  return 0;
}

// Start a new log window, or stop logging with 0.
int
proc_schedlog(int duration_ticks)
{
  if(duration_ticks < 0)
    return -1;

  acquire(&log_lock);
  log_start = read_current_tick();
  log_duration = duration_ticks;
  release(&log_lock);

  return 0;
}

// Print the processes while the log window is active.
static void
schedlog_emit(struct proc *selected)
{
  // Take the log lock before any process lock.
  push_off();
  release(&selected->lock);
  acquire(&log_lock);

  uint now = read_current_tick();
  uint elapsed = now - log_start;

  if(log_duration > 0 && elapsed < log_duration){
    // Section 3.2: GROUP emits all eight groups; RR/STCF retain STCF format.
    int count = stcf_policy == 2 ? N_GROUPS : 1;
    for(int id = 0; id < count; id++){
      if(stcf_policy == 2){
        acquire(&group[id].lock);
        int shares = group[id].shares;
        uint64 vruntime = group[id].vruntime;
        release(&group[id].lock);
        printf("%u|GROUP|%d(%d,%lu)", now, id, shares, vruntime);
      } else {
        printf("%u|STCF|0(0,0)", now);
      }

      for(int i = 0; i < NPROC; i++){
        struct proc *p = &proc[i];
        acquire(&p->lock);
        if(p->state != UNUSED && (stcf_policy != 2 || p->gid == id)){
          int pid = p->pid;
          int state = p->state;
          int remaining = p->remaining_burst;
          int consumed = p->burst_consumed;
          char name[16];
          safestrcpy(name, p->name, sizeof(name));
          release(&p->lock);
          printf(",[%d]%s:%d(%d,%d)", pid, name, state, remaining, consumed);
        } else {
          release(&p->lock);
        }
      }
      printf("\n");
    }
  }

  release(&log_lock);
  acquire(&selected->lock);
  pop_off();
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  proc_setstate(p, RUNNABLE);
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke exec() now that file system is initialized.
    // Put the return value (argc) of exec into a0.
    p->trapframe->a0 = exec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on wait channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  proc_setstate(p, SLEEPING);


  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on wait channel chan.
// Caller should hold the condition lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        proc_setstate(p, RUNNABLE);

      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        proc_setstate(p, RUNNABLE);
        
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    // Show the counters when we press Ctrl+P.
    printf(" created=%u total=%d left=%d used=%d cpu=%lu ready=%lu group=%d\n",
           p->created_tick, p->total_burst, p->remaining_burst,
           p->burst_consumed, p->cpu_ticks, p->ready_ticks,
           p->gid);
  }
}
