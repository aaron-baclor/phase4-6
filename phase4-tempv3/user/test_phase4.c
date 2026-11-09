#include "kernel/types.h"
#include "kernel/stcf.h"
#include "user/user.h"

// Section 4.1.4: prepare a same-group child using the static assignment rule.
static int
same_group(void)
{
  int parent = getpid();
  for(;;){
    int pid = fork();
    if(pid < 0) exit(1);
    if(pid == 0){
      if(getpid() % N_GROUPS != parent % N_GROUPS) exit(0);
      return 0;
    }
    if(pid % N_GROUPS == parent % N_GROUPS) return pid;
    int status;
    if(wait(&status) < 0 || status != 0) exit(1);
  }
}

// Sections 2.3-2.4: controlled same-group preemption and actual aging progress.
int
main(int argc, char **argv)
{
  if(argc == 2 && !strcmp(argv[1], "exec")){
    printf("PASS: Phase 4 exec\n");
    exit(0);
  }
  if(argc == 2 && !strcmp(argv[1], "aging")){
    int gate[2];
    if(pipe(gate) < 0) exit(1);
    setburst(100000); schedlog(100);
    int child = same_group();
    if(child == 0){
      close(gate[1]); setburst(50);
      char byte;
      if(read(gate[0], &byte, 1) != 1) exit(1);
      printf("AGED_RUN|%d|%d\n", getpid(), uptime());
      exit(0);
    }
    close(gate[0]); setburst(10);
    int start = uptime();
    printf("AGING_RELEASE|%d|%d\n", child, start);
    write(gate[1], "x", 1); close(gate[1]);
    while(uptime() - start < 70){
      int tick = uptime();
      while(uptime() == tick) { }
      setburst(10);
    }
    int status;
    if(wait(&status) != child || status) exit(1);
    schedlog(0); printf("PASS: starvation mitigation\n");
    exit(0);
  }
  if(argc == 2 && !strcmp(argv[1], "preempt")){
    setburst(100000); schedlog(100);
    int child = same_group();
    if(child == 0){
      setburst(5); sleep(2);
      printf("SHORT_READY|%d|%d\n", getpid(), uptime());
      for(;;) { }
    }
    setburst(40);
    int start = uptime();
    while(uptime() - start < 10) { }
    int status;
    if(wait(&status) != child || status != -1) exit(1);
    schedlog(0); printf("PASS: wakeup preemption\n");
    exit(0);
  }

  // Sections 3.2-3.3: exercise both public syscalls and fatal validation.
  setburst(100000);
  if(schedlog(-1) != -1 || schedlog(0) || schedlog(40)) exit(1);
  for(int i = 0; i < 2; i++){
    int pid = fork();
    if(pid < 0) exit(1);
    if(pid == 0){
      setburst(-i);
      printf("FAIL: invalid burst returned\n");
      exit(42);
    }
    int status;
    if(wait(&status) != pid || status != -1) exit(1);
  }
  printf("PASS: fatal burst arguments\n");
  int child = fork();
  if(child < 0) exit(1);
  if(child == 0){
    char *args[] = {"test_phase4", "exec", 0};
    exec(args[0], args); exit(1);
  }
  int status;
  if(wait(&status) != child || status) exit(1);
  child = fork();
  if(child < 0) exit(1);
  if(child == 0){
    setburst(3);
    int start = uptime();
    while(uptime() - start < 8) { }
    exit(17);
  }
  if(wait(&status) != child || (status != -1 && status != 17)) exit(1);
  printf("EXHAUST|%d\n", status);
  schedlog(2); sleep(4); schedlog(0);
  printf("PASS: test_phase4\n");
  exit(0);
}
