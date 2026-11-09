#include "workload.h"

// Sections 3.6, 4.3.1(7): keep two groups busy across a live weight change.
int
main(void)
{
  begin("test_group_sh");
  setshares(1, 100);
  setshares(2, 300);
  int start = uptime();
  for(int i = 1; i <= 2; i++){
    int pid = groupfork(i);
    if(pid < 0) exit(1);
    if(pid == 0){
      setburst(100000);
      while(uptime() - start < 32)
        compute(100000);
      exit(0);
    }
  }
  sleep(16);
  printf("SHARES_CHANGE|%d\n", uptime());
  setshares(1, 300);
  setshares(2, 100);
  int status;
  while(wait(&status) > 0)
    if(status != 0) exit(1);
  done("test_group_sh");
  exit(0);
}
