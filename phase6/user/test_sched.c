#include "kernel/types.h"
#include "user/user.h"

// Sections 3.3-3.5: fatal validation is observed by the surviving parent.
static void
invalid(int which)
{
  int pid = fork();
  if(pid < 0) exit(1);
  if(pid == 0){
    switch(which){
    case 0: setburst(0); break;
    case 1: setburst(-1); break;
    case 2: setgroup(-1); break;
    case 3: setgroup(8); break;
    case 4: groupfork(-1); break;
    case 5: groupfork(8); break;
    }
    printf("FAIL: invalid call returned to user\n");
    exit(42);
  }
  int status;
  if(wait(&status) != pid || status != -1){
    printf("FAIL: fatal validation %d status %d\n", which, status);
    exit(1);
  }
}

// Sections 3.2-3.6, 4.3.2: exercise the public syscall ABI on real xv6.
int
main(int argc, char **argv)
{
  if(argc == 2 && strcmp(argv[1], "exec") == 0){
    printf("PASS: exec preserved membership pid=%d\n", getpid());
    exit(0);
  }
  // Section 2.4: repeated short bursts must eventually yield to an aged job.
  if(argc == 2 && strcmp(argv[1], "aging") == 0){
    int gate[2];
    if(pipe(gate) < 0) exit(1);
    setburst(100000);
    schedlog(100);
    int pid = fork();
    if(pid < 0) exit(1);
    if(pid == 0){
      close(gate[1]);
      setburst(50);
      char byte;
      if(read(gate[0], &byte, 1) != 1) exit(1);
      printf("AGED_RUN|%d|%d\n", getpid(), uptime());
      exit(0);
    }
    close(gate[0]);
    setburst(10);
    int start = uptime();
    printf("AGING_RELEASE|%d|%d\n", pid, start);
    write(gate[1], "x", 1);
    close(gate[1]);
    while(uptime() - start < 70){
      int next = uptime();
      while(uptime() == next) { }
      setburst(10);
    }
    int status;
    if(wait(&status) != pid || status != 0) exit(1);
    schedlog(0);
    printf("PASS: starvation mitigation\n");
    exit(0);
  }
  // Section 2.3: retain a real trap trace of shorter-job wakeup preemption.
  if(argc == 2 && strcmp(argv[1], "preempt") == 0){
    setburst(1000);
    schedlog(100);
    int pid = fork();
    if(pid < 0) exit(1);
    if(pid == 0){
      setburst(5);
      sleep(2);
      printf("SHORT_READY|%d|%d\n", getpid(), uptime());
      for(;;) { }
    }
    setburst(40);
    int start = uptime();
    while(uptime() - start < 10) { }
    int status;
    if(wait(&status) != pid || (status != -1 && status != 0)) exit(1);
    printf("PASS: wakeup preemption\n");
    schedlog(0);
    exit(0);
  }
  setburst(100000);
  if(schedlog(-1) != -1 || schedlog(0) != 0 || schedlog(40) != 0)
    exit(1);
  for(int i = 0; i < 6; i++) invalid(i);
  printf("PASS: fatal arguments\n");
  if(setshares(-1, 100) != -1 || setshares(8, 100) != -1 ||
     setshares(0, 0) != -1 || setshares(0, -1) != -1 ||
     setshares(7, 2147483647) != 0 || setshares(7, 100) != 0)
    exit(1);
  printf("PASS: shares validation and caller survives\n");
  if(setgroup(7) != 0 || setgroup(7) != 0) exit(1);
  printf("MEMBER|%d|7|setgroup\n", getpid());
  int pid = fork();
  if(pid < 0) exit(1);
  if(pid == 0){
    printf("MEMBER|%d|7|inherit\n", getpid());
    sleep(2);
    exit(0);
  }
  int status;
  if(wait(&status) != pid || status != 0) exit(1);
  pid = groupfork(3);
  if(pid < 0) exit(1);
  if(pid == 0){
    printf("MEMBER|%d|3|groupfork\n", getpid());
    char *args[] = {"test_sched", "exec", 0};
    exec(args[0], args);
    exit(1);
  }
  if(wait(&status) != pid || status != 0) exit(1);
  if(setgroup(0) != 0) exit(1);
  // Section 3.3: expiration kills STCF workers; RR retains timer switching.
  pid = fork();
  if(pid < 0) exit(1);
  if(pid == 0){
    setburst(3);
    int start = uptime();
    while(uptime() - start < 8) { }
    exit(17);
  }
  if(wait(&status) != pid || (status != -1 && status != 17)) exit(1);
  printf("EXHAUST|%d\n", status);
  // Section 3.2: replacing and stopping the common log window.
  schedlog(2);
  sleep(4);
  schedlog(0);
  printf("PASS: test_sched\n");
  exit(0);
}
