#ifndef NSEXEC_H
#define NSEXEC_H

#include <setjmp.h>
#include <stdint.h>

#define STAGE_SETUP -1
#define STAGE_PARENT 0
#define STAGE_CHILD 1
#define STAGE_INIT 2

#define CLONE_FLAGS_ATTR 27281

static int syncfd = -1;

/* Assume the stack grows down, so arguments should be above it. */
struct clone_t {
  /*
   * Reserve some space for clone() to locate arguments
   * and retcode in this place
   */
  char stack[4096] __attribute__((aligned(16)));
  char stack_ptr[0];

  /* There's two children. This is used to execute the different code. */
  jmp_buf *env;
  int jmpval;
};

enum sync_t {
  SYNC_RECVPID = 0x43, /* PID was correctly received by parent. */
};

struct nlconfig_t {
  char *data;

  /* Process settings. */
  uint32_t cloneflags;
};

int clone_parent(jmp_buf *env, int jmpval);

#endif  // NSEXEC_H