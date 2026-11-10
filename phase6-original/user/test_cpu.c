#include "workload.h"

// Section 4.3.1(1): compute continuously for a finite number of iterations.
int
main(void)
{
  begin("test_cpu");
  compute(800000000);
  done("test_cpu");
  exit(0);
}
