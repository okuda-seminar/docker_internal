#include <setjmp.h>
#include <signal.h>
#include <sched.h>

#include "nsexec.h"

static int child_func(void *arg)
{
  struct clone_t *ca = (struct clone_t *)arg;
  longjmp(*ca->env, ca->jmpval);
}

int clone_parent(jmp_buf *env, int jmpval)
{
  struct clone_t ca = {
      .env = env,
      .jmpval = jmpval,
  };
  return clone(child_func, ca.stack_ptr, CLONE_PARENT | SIGCHLD, &ca);
}