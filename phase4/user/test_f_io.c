#include "workload.h"

// Section 4.3.1(4): multiple children alternate short computation and I/O.
int
main(void)
{
  begin("test_f_io");
  for(int i = 0; i < 4; i++){
    int pid = fork();
    if(pid < 0) exit(1);
    if(pid == 0){
      setburst(100000);
      for(int j = 0; j < 5; j++){
        compute(125000);
        sleep(4);
      }
      exit(0);
    }
  }
  int status;
  while(wait(&status) > 0)
    if(status != 0) exit(1);
  done("test_f_io");
  exit(0);
}
