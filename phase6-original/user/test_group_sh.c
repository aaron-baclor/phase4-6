#include "workload.h"

int
main(void)
{
  begin("test_group_sh");
  int start = uptime();
  // keep both groups ready while group 2 collects sleeping ticks
  for(int i = 0; i < 6; i++){
    int pid = groupfork(i == 0 ? 1 : 2);
    if(pid < 0) exit(1);
    if(pid == 0){
      setburst(i < 2 ? 100000 : 1000);
      while(uptime() - start < 32){
        if(i < 2)
          compute(100000);
        else
          sleep(1);
      }
      exit(0);
    }
  }
  int status;
  while(wait(&status) > 0)
    if(status != 0) exit(1);
  done("test_group_sh");
  exit(0);
}
