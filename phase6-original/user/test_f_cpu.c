#include "workload.h"

// Section 4.3.1(3): four children share the finite CPU work.
int
main(void)
{
  begin("test_f_cpu");
  for(int i = 0; i < 4; i++){
    int pid = fork();
    if(pid < 0) exit(1);
    if(pid == 0){
      setburst(100000);
      compute(200000000);
      exit(0);
    }
  }
  int status;
  while(wait(&status) > 0)
    if(status != 0) exit(1);
  done("test_f_cpu");
  exit(0);
}
