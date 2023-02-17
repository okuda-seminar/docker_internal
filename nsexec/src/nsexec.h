#ifndef NSEXEC_H
#define NSEXEC_H

#include <setjmp.h>

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

static int child_func(void *arg)
{
  struct clone_t *ca = (struct clone_t *)arg;
  longjmp(*ca->env, ca->jmpval);
}

static int clone_parent(jmp_buf *env, int jmpval)
{
  struct clone_t ca = {
      .env = env,
      .jmpval = jmpval,
  };
  return clone(child_func, ca.stack_ptr, CLONE_PARENT | SIGCHLD, &ca);
}

#endif // NSEXEC_H