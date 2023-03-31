#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/netlink.h>
#include <sys/socket.h>

#include "namespace.h"

#define STAGE_SETUP -1
#define STAGE_PARENT 0
#define STAGE_CHILD 1
#define STAGE_INIT 2
#define CLONE_FLAGS_ATTR 27281

int current_stage = STAGE_SETUP;
static int syncfd = -1;

static uint32_t readint32(char *buf) { return *(uint32_t *)buf; }

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

static int child_func(void *arg) {
  struct clone_t *ca = (struct clone_t *)arg;
  longjmp(*ca->env, ca->jmpval);
}

static int clone_parent(jmp_buf *env, int jmpval) {
  struct clone_t ca = {
      .env = env,
      .jmpval = jmpval,
  };
  return clone(child_func, ca.stack_ptr, CLONE_PARENT | SIGCHLD, &ca);
}

#define bail(fmt, ...)                                      \
  do {                                                      \
    fprintf(stderr, "FATAL: " fmt ": %m\n", ##__VA_ARGS__); \
    exit(1);                                                \
  } while (0)

static int getenv_int(const char *name) {
  char *val, *endptr;
  int ret;

  val = getenv(name);
  /* Treat empty value as unset variable. */
  if (val == NULL || *val == '\0') return -ENOENT;

  ret = strtol(val, &endptr, 10);
  if (val == endptr || *endptr != '\0')
    bail("unable to parse %s=%s", name, val);
  /*
   * Sanity check: this must be a non-negative number.
   */
  if (ret < 0) bail("bad value for %s=%s (%d)", name, val, ret);

  return ret;
}

static void nl_parse(int fd, struct nlconfig_t *config) {
  size_t len, size;
  struct nlmsghdr hdr;
  char *current, *data;

  /* Retrieve the netlink header. */
  len = read(fd, &hdr, NLMSG_HDRLEN);
  if (len != NLMSG_HDRLEN) bail("invalid netlink header length %zu", len);

  /* Retrieve data. */
  size = NLMSG_PAYLOAD(&hdr, 0);
  data = (char *)malloc(size);
  current = data;

  if (!data)
    bail("failed to allocate %zu bytes of memory for nl_payload", size);

  len = read(fd, data, size);
  if (len != size)
    bail("failed to read netlink payload, %zu != %zu", len, size);

  /* Parse the netlink payload. */
  config->data = data;
  while (current < data + size) {
    struct nlattr *nlattr = (struct nlattr *)current;
    size_t payload_len = nlattr->nla_len - NLA_HDRLEN;

    /* Advance to payload. */
    current += NLA_HDRLEN;

    /* Handle payload. */
    switch (nlattr->nla_type) {
      case CLONE_FLAGS_ATTR:
        config->cloneflags = readint32(current);
        break;

      default:
        bail("unknown netlink message type %d", nlattr->nla_type);
    }

    current += NLA_ALIGN(payload_len);
  }
}

void nl_free(struct nlconfig_t *config) { free(config->data); }

// nsenter.go call nsexec function for creating containers.
void nsexec(void) {
  int pipenum;  // Libcontainer Initpipe
  jmp_buf env;
  struct nlconfig_t config;
  int sync_pipe[2];

  pipenum = getenv_int("_LIBCONTAINER_INITPIPE");
  if (pipenum < 0) {
    return;
  }

  // Receive initWaiter() in esenter_test.go
  if (write(pipenum, "", 1) != 1)
    bail("could not inform the parent we are past initial setup");

  // Parse a config which describes setting from io.Copy() method in
  // nsenter_test.go
  nl_parse(pipenum, &config);

  // Create socket pair between parent and child.
  if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sync_pipe) < 0)
    bail("failed to setup sync pipe between parent and child");

  current_stage = setjmp(env);
  switch (current_stage) {
    // The runc init parent process creates new child process. The child process
    // creates a grandchild process and sends PID (named stage2_pid) to parent
    // process.
    case STAGE_PARENT: {
      enum sync_t s;
      pid_t stage1_pid = -1, stage2_pid = -1;

      stage1_pid = clone_parent(&env, STAGE_CHILD);
      if (stage1_pid < 0) bail("unable to spawn stage-1");

      syncfd = sync_pipe[1];
      if (close(sync_pipe[0]) < 0) bail("failed to close sync_pipe[0] fd");

      if (read(syncfd, &stage2_pid, sizeof(stage2_pid)) != sizeof(stage2_pid))
        bail("failed to sync with stage-1: read(stage2_pid)");

      s = SYNC_RECVPID;
      if (write(syncfd, &s, sizeof(s)) != sizeof(s))
        bail("failed to sync with stage-1: write(SYNC_RECVPID)");

      int len = dprintf(pipenum, "{\"stage1_pid\":%d,\"stage2_pid\":%d}\n",
                        stage1_pid, stage2_pid);
      if (len < 0) bail("failed to sync with runc: write(pid-JSON)");
      nl_free(&config);
      exit(0);
    } break;
    case STAGE_CHILD: {
      pid_t stage2_pid = -1;
      enum sync_t s;

      /* We're in a child. */
      syncfd = sync_pipe[0];
      if (close(sync_pipe[1]) < 0) bail("failed to close sync_pipe[1] fd");

      if (config.cloneflags && CLONE_NEWUSER) {
        if (unshare(config.cloneflags) < 0)
          bail("failed to unshare remaining namespaces (except cgroupns)");

        stage2_pid = clone_parent(&env, STAGE_INIT);
        if (stage2_pid < 0) bail("unable to spawn stage-2");

        if (write(syncfd, &stage2_pid, sizeof(stage2_pid)) !=
            sizeof(stage2_pid))
          bail("failed to sync with parent: write(stage2_pid)");

        /* ... wait for parent to get the pid ... */
        if (read(syncfd, &s, sizeof(s)) != sizeof(s))
          bail("failed to sync with parent: read(SYNC_RECVPID)");
        if (s != SYNC_RECVPID)
          bail("failed to sync with parent: SYNC_RECVPID: got %u", s);
      }

      exit(0);
    } break;
    case STAGE_INIT: {
      exit(0);
    }
    default:
      exit(0);
  }
  /* Should never be reached. */
  bail("should never be reached");
}