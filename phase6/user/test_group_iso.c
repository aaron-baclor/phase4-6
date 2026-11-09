#include "workload.h"

// Section 4.3.1(6): flood Group 1 while a long worker runs in Group 2.
int
main(void)
{
  begin("test_group_iso");
  for(int i = 0; i < 2; i++){
    int pid = groupfork(i + 1);
    if(pid < 0) exit(1);
    if(pid == 0){
      setburst(100000);
      if(i == 0){
        for(int j = 0; j < 12; j++){
          int child = fork();
          if(child < 0) exit(1);
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
      printf("ISOLATION|%d|complete\n", i + 1);
      exit(0);
    }
  }
  int status;
  while(wait(&status) > 0)
    if(status != 0) exit(1);
  done("test_group_iso");
  exit(0);
}
