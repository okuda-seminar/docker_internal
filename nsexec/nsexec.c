#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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
  SYNC_USERMAP_PLS = 0x40,  /* Request parent to map our users. */
  SYNC_USERMAP_ACK = 0x41,  /* Mapping finished by the parent. */
  SYNC_RECVPID_PLS = 0x42,  /* Tell parent we're sending the PID. */
  SYNC_RECVPID_ACK = 0x43,  /* PID was correctly received by parent. */
  SYNC_GRANDCHILD = 0x44,   /* The grandchild is ready to run. */
  SYNC_CHILD_FINISH = 0x45, /* The child or grandchild has finished. */
  SYNC_MOUNTSOURCES_PLS =
      0x46, /* Tell parent to send mount sources by SCM_RIGHTS. */
  SYNC_MOUNTSOURCES_ACK = 0x47, /* All mount sources have been sent. */
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

#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARNING 2
#define LOG_LEVEL_INFO 3
#define LOG_LEVEL_DEBUG 4

void write_log(int log_level, const char *message) {
  time_t t = time(NULL);
  struct tm tm = *localtime(&t);

  char log_level_str[8];
  switch (log_level) {
    case LOG_LEVEL_ERROR:
      strcpy(log_level_str, "ERROR");
      break;
    case LOG_LEVEL_WARNING:
      strcpy(log_level_str, "WARNING");
      break;
    case LOG_LEVEL_INFO:
      strcpy(log_level_str, "INFO");
      break;
    case LOG_LEVEL_DEBUG:
      strcpy(log_level_str, "DEBUG");
      break;
  }

  fprintf(stdout, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s\n",
          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
          tm.tm_sec, log_level_str, message);
}

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
  int pipenum;
  jmp_buf env;
  struct nlconfig_t config = {0};
  int sync_child_pipe[2], sync_grandchild_pipe[2];
  printf("start nsexec\n");
  // sync_child_pipe[0] and sync_child_pipe[1] are now connected to each other
  // and can be used to send and receive data using the read and write functions
  if (setresgid(0, 0, 0) < 0) bail("failed to become root in user namespace");

  pipenum = getenv_int("_LIBCONTAINER_INITPIPE");
  printf("%d\n", pipenum);
  if (pipenum < 0) {
    return;
  }

  if (write(pipenum, "", 1) != 1)
    bail("could not inform the parent we are past initial setup");

  // (To Do) Parse a config which describes setting for creating user-specific
  // container.
  nl_parse(pipenum, &config);

  // Create socket pair between parent and child.
  if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sync_child_pipe) < 0)
    bail("failed to setup sync pipe between parent and child");

  // Create socket pair between parent and grandchild.
  if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sync_grandchild_pipe) < 0)
    bail("failed to setup sync pipe between parent and grandchild");

  // printf("uid = %u, euid = %u, gid = %u, egid = %u\n", getuid(), geteuid(),
  // getgid(), getegid());
  current_stage = setjmp(env);
  switch (current_stage) {
    // The runc init parent process creates new child process, the uid map, and
    // gid map. The child process creates a grandchild process and sends PID.
    case STAGE_PARENT: {
      char message[1024];
      pid_t stage1_pid = -1, stage2_pid = -1;
      bool stage1_complete, stage2_complete;

      write_log(LOG_LEVEL_DEBUG, "stage-1");
      stage1_pid = clone_parent(&env, STAGE_CHILD);
      if (stage1_pid < 0) bail("unable to spawn stage-1");
      syncfd = sync_child_pipe[1];
      if (close(sync_child_pipe[0]) < 0)
        bail("failed to close sync_child_pipe[0] fd");
      stage1_complete = false;
      write_log(LOG_LEVEL_DEBUG, "stage-1 synchronisation loop");
      while (!stage1_complete) {
        enum sync_t s;
        if (read(syncfd, &s, sizeof(s)) != sizeof(s)) {
          bail("failed to sync with stage-1: next state\n");
        }
        switch (s) {
          case SYNC_USERMAP_PLS:
            write_log(LOG_LEVEL_DEBUG, "stage-1 requested userns mappings");
            s = SYNC_USERMAP_ACK;
            if (write(syncfd, &s, sizeof(s)) != sizeof(s)) {
              bail("failed to sync with stage-1: write(SYNC_USERMAP_ACK)");
            }
            break;
          case SYNC_RECVPID_PLS:
            write_log(LOG_LEVEL_DEBUG, "stage-1 requested pid to be forwarded");
            if (read(syncfd, &stage2_pid, sizeof(stage2_pid)) !=
                sizeof(stage2_pid))
              bail("failed to sync with stage-1: read(stage2_pid)");
            s = SYNC_RECVPID_ACK;
            if (write(syncfd, &s, sizeof(s)) != sizeof(s))
              bail("failed to sync with stage-1: write(SYNC_RECVPID_ACK)");
            snprintf(message, 1024,
                     "forward stage-1 (%d) and stage-2 (%d) pids to runc",
                     stage1_pid, stage2_pid);

            write_log(LOG_LEVEL_DEBUG, message);
            int len =
                dprintf(pipenum, "{\"stage1_pid\":%d,\"stage2_pid\":%d}\n",
                        stage1_pid, stage2_pid);
            if (len < 0) bail("failed to sync with runc: write(pid-JSON)");
            break;
          case SYNC_CHILD_FINISH:
            write_log(LOG_LEVEL_DEBUG, "stage-1 complete");
            stage1_complete = true;
            break;
          default: {
            break;
          }
        }
      }
      write_log(LOG_LEVEL_DEBUG, "<- stage-1 synchronisation loop");
      /* Now sync with grandchild. */
      syncfd = sync_grandchild_pipe[1];
      if (close(sync_grandchild_pipe[0]) < 0)
        bail("failed to close sync_grandchild_pipe[0] fd");

      write_log(LOG_LEVEL_DEBUG, "-> stage-2 synchronisation loop");
      stage2_complete = false;
      while (!stage2_complete) {
        enum sync_t s;

        write_log(LOG_LEVEL_DEBUG, "signalling stage-2 to run");
        s = SYNC_GRANDCHILD;
        if (write(syncfd, &s, sizeof(s)) != sizeof(s)) {
          bail("failed to sync with child: write(SYNC_GRANDCHILD)");
        }

        if (read(syncfd, &s, sizeof(s)) != sizeof(s))
          bail("failed to sync with child: next state");

        switch (s) {
          case SYNC_CHILD_FINISH:
            write_log(LOG_LEVEL_DEBUG, "stage-2 complete");
            stage2_complete = true;
            break;
          default:
            bail("unexpected sync value: %u", s);
        }
      }
      write_log(LOG_LEVEL_DEBUG, "<- stage-2 synchronisation loop");
      write_log(LOG_LEVEL_DEBUG, "<~ nsexec stage-0");
      exit(0);
    }
    case STAGE_CHILD: {
      char message[1024];
      pid_t stage2_pid = -1;
      enum sync_t s;

      /* We're in a child and thus need to tell the parent if we die. */
      syncfd = sync_child_pipe[0];
      if (close(sync_child_pipe[1]) < 0)
        bail("failed to close sync_child_pipe[1] fd");
      prctl(PR_SET_NAME, (unsigned long)"runc:[1:CHILD]", 0, 0, 0);
      write_log(LOG_LEVEL_DEBUG, "~> nsexec stage-1");

      if (config.cloneflags & CLONE_NEWUSER) {
        // Create new user namespace.
        if (unshare(CLONE_NEWUSER) < 0)
          bail("failed to unshare user namespace");
        s = SYNC_USERMAP_PLS;
        if (write(syncfd, &s, sizeof(s)) < 0) {
          bail("failed to sync with parent: write(SYNC_USERMAP_PLS)\n");
        }
        /* ... wait for mapping ... */
        write_log(LOG_LEVEL_DEBUG, "request stage-0 to map user namespace");
        if (read(syncfd, &s, sizeof(s)) != sizeof(s))
          bail("failed to sync with parent: read(SYNC_USERMAP_ACK)");
        if (s != SYNC_USERMAP_ACK)
          bail("failed to sync with parent: SYNC_USERMAP_ACK: got %u", s);

        /* Become root in the namespace proper. */
        if (setresuid(0, 0, 0) < 0)
          bail("failed to become root in user namespace");
        if (setresgid(0, 0, 0) < 0)
          bail("failed to become root in user namespace");
      }
      write_log(LOG_LEVEL_DEBUG,
                "unshare remaining namespace (except cgroupns)");
      // printf("uid = %u, euid = %u, gid = %u, egid = %u\n", getuid(),
      // geteuid(), getgid(), getegid());
      if (unshare(config.cloneflags & ~CLONE_NEWCGROUP) < 0)
        bail("failed to unshare remaining namespaces (except cgroupns)");
      write_log(LOG_LEVEL_DEBUG, "stage-2");
      stage2_pid = clone_parent(&env, STAGE_INIT);
      if (stage2_pid < 0) bail("unable to spawn stage-2");
      // printf("uid = %u, euid = %u, gid = %u, egid = %u\n", getuid(),
      // geteuid(), getgid(), getegid());
      snprintf(message, 1024, "request stage-0 to forward stage-2 pid (%d)",
               stage2_pid);

      write_log(LOG_LEVEL_DEBUG, message);
      s = SYNC_RECVPID_PLS;
      if (write(syncfd, &s, sizeof(s)) != sizeof(s))
        bail("failed to sync with parent: write(SYNC_RECVPID_PLS)");
      if (write(syncfd, &stage2_pid, sizeof(stage2_pid)) != sizeof(stage2_pid))
        bail("failed to sync with parent: write(stage2_pid)");

      /* ... wait for parent to get the pid ... */
      if (read(syncfd, &s, sizeof(s)) != sizeof(s))
        bail("failed to sync with parent: read(SYNC_RECVPID_ACK)");
      if (s != SYNC_RECVPID_ACK)
        bail("failed to sync with parent: SYNC_RECVPID_ACK: got %u", s);
      write_log(LOG_LEVEL_DEBUG, "signal completion to stage-0");
      s = SYNC_CHILD_FINISH;
      if (write(syncfd, &s, sizeof(s)) != sizeof(s))
        bail("failed to sync with parent: write(SYNC_CHILD_FINISH)");
      write_log(LOG_LEVEL_DEBUG, "<~ nsexec stage-1");
      exit(0);
    } break;
    case STAGE_INIT: {
      enum sync_t s;
      write_log(LOG_LEVEL_DEBUG, "STAGE_INIT");
      /* We're in a child and thus need to tell the parent if we die. */
      syncfd = sync_grandchild_pipe[0];
      if (close(sync_grandchild_pipe[1]) < 0)
        bail("failed to close sync_grandchild_pipe[1] fd");

      if (close(sync_child_pipe[0]) < 0)
        bail("failed to close sync_child_pipe[0] fd");

      write_log(LOG_LEVEL_DEBUG, "~> nsexec stage-2");

      if (read(syncfd, &s, sizeof(s)) != sizeof(s))
        bail("failed to sync with parent: read(SYNC_GRANDCHILD)");
      if (s != SYNC_GRANDCHILD)
        bail("failed to sync with parent: SYNC_GRANDCHILD: got %u", s);

      if (config.cloneflags & CLONE_NEWCGROUP) {
        if (unshare(CLONE_NEWCGROUP) < 0)
          bail("failed to unshare cgroup namespace");
      }

      write_log(LOG_LEVEL_DEBUG, "signal completion to stage-0");
      s = SYNC_CHILD_FINISH;
      if (write(syncfd, &s, sizeof(s)) != sizeof(s))
        bail("failed to sync with parent: write(SYNC_CHILD_FINISH)");

      /* Close sync pipes. */
      if (close(sync_grandchild_pipe[0]) < 0)
        bail("failed to close sync_grandchild_pipe[0] fd");

      /* Finish executing, let the Go runtime take over. */
      write_log(LOG_LEVEL_DEBUG, "<= nsexec container setup");
      write_log(LOG_LEVEL_DEBUG, "booting up go runtime ...");
      return;
    }
    default:
      bail("unknown stage '%d' for jump value", current_stage);
  }
  /* Should never be reached. */
  bail("should never be reached");
}