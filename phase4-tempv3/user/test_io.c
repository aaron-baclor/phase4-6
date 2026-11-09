#include "workload.h"

// Section 4.3.1(2): long sleeps alternate with short finite computations.
int
main(void)
{
  begin("test_io");
  for(int i = 0; i < 5; i++){
    compute(500000);
    sleep(4);
  }
  done("test_io");
  exit(0);
}
