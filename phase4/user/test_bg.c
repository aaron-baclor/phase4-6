#include "workload.h"

// Section 4.3.1(5): alternate CPU-heavy and sleeping intervals.
int
main(void)
{
  begin("test_bg");
  for(int i = 0; i < 6; i++){
    compute(40000000);
    sleep(2);
  }
  done("test_bg");
  exit(0);
}
