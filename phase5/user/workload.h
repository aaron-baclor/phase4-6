#include "kernel/types.h"
#include "user/user.h"

// Section 4.3.1: finite computations provide a repeatable CPU workload.
static void
compute(int rounds)
{
  volatile uint value = 1;
  for(int i = 0; i < rounds; i++)
    value = value * 1664525U + 1013904223U;
}

// Section 4.3.4: identify the workload root without guessing from names.
static void
begin(char *name)
{
  schedlog(10000);
  setburst(100000);
  printf("WORKLOAD|%s|%d\n", name, getpid());
}

static void
done(char *name)
{
  schedlog(0);
  printf("DONE|%s\n", name);
}
