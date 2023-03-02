#ifndef NSEXEC_H
#define NSEXEC_H

#include <setjmp.h>

#define CLONE_PARENT (1 << 24)

/* Assume the stack grows down, so arguments should be above it. */
struct clone_t
{
  /*
   * Reserve some space for clone() to locate arguments
   * and retcode in this place
   */
  char stack[4096] __attribute__((aligned(16)));
  char stack_ptr[0];

  /* There's two children. They are used to execute the different code. */
  jmp_buf *env;
  int jmpval;
};

int clone_parent(jmp_buf *env, int jmpval);

#endif // NSEXEC_H