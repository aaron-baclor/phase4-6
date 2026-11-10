#include "workload.h"
#include "kernel/stcf.h"

// Sections 4.1.4, 4.3.1: static groups require padding PIDs, not groupfork.
static int
spawn_in_group(int gid)
{
  for(;;){
    int pid = fork();
    if(pid < 0) exit(1);
    if(pid == 0){
      if(getpid() % N_GROUPS != gid) exit(0);
      return 0;
    }
    if(pid % N_GROUPS == gid) return pid;
    int status;
    if(wait(&status) < 0 || status != 0) exit(1);
  }
}

// Section 2.5: short workers in Group 1 compete with a long Group 2 worker.
int
main(void)
{
  begin("test_group_iso");
  for(int i = 1; i <= 2; i++){
    int pid = spawn_in_group(i);
    if(pid == 0){
      setburst(100000);
      if(i == 1){
        for(int j = 0; j < 12; j++){
          int child = spawn_in_group(1);
          if(child == 0){
            setburst(10);
            compute(33333333);
            exit(0);
          }
        }
        int status;
        while(wait(&status) > 0)
          if(status != 0) exit(1);
      } else {
        compute(400000000);
      }
      printf("ISOLATION|%d|complete\n", i);
      exit(0);
    }
    printf("ISOLATION_WORKER|%d|%d\n", i, pid);
  }
  int status;
  while(wait(&status) > 0)
    if(status != 0) exit(1);
  done("test_group_iso");
  exit(0);
}
