/* enables realpath() */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1
#endif

#include "pstrack.h"

#include "stdio.h"
#include "stdlib.h"
#include "stddef.h"
#include "stdarg.h"
#include "string.h"

#include "unistd.h"
#include "pthread.h"
#include "errno.h"
#include "signal.h"
#include "dirent.h"
#include "sys/types.h"
#include "sys/socket.h"
#include "sys/uio.h"
#include "linux/limits.h"
#include "linux/connector.h"
#include "linux/netlink.h"
#include "linux/cn_proc.h"

int pstrack_realpathf(char *dest, const char *format, ...) {
  static char path[PATH_MAX];
  va_list args; va_start(args, format);
  vsnprintf(path, sizeof(path), format, args);
  va_end(args);
  return realpath(path, dest) != NULL;
}

/* reference: https://gist.github.com/fclairamb/a16a4237c46440bdb172 */
int pstrack_getppid(pid_t *dest, pid_t pid) {
  int success = 0;
  static char path[64];
  if (snprintf(path, sizeof(path), "/proc/%d/stat", pid) < 0) return success;
  FILE *fp = fopen(path, "r");
  if (fp) {
    static char buffer[64];
    if (fread(buffer, sizeof(char), sizeof(buffer), fp) > 0) {
      int space = 0;
      for (const char *p = buffer; *p; p++) {
        if (*p == ' ') ++space;
        /* get fourth field */
        if (space == 3) { *dest = atoi(p + 1); success = 1; break; }
      }
    }
    fclose(fp);
  }
  return success;
}

/* credits: forkstat (https://github.com/ColinIanKing/forkstat) */

int pstrack_forkstat_sock() {
  int sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
  if (sock < 0) goto on_fail;

  pid_t self_pid = getpid();

  struct sockaddr_nl addr = {
    .nl_pid = self_pid,
    .nl_family = AF_NETLINK, .nl_groups = CN_IDX_PROC,
  };
  if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    goto on_fail;

  /* subscribe */

  struct nlmsghdr nlmsghdr = {
    .nlmsg_len = NLMSG_LENGTH(sizeof(struct cn_msg) +
      sizeof(enum proc_cn_mcast_op)),
    .nlmsg_pid = self_pid,
    .nlmsg_type = NLMSG_DONE,
  };

  struct cn_msg cn_msg = {
    .id.idx = CN_IDX_PROC, .id.val = CN_VAL_PROC,
    .len = sizeof(enum proc_cn_mcast_op),
  };

  enum proc_cn_mcast_op op = PROC_CN_MCAST_LISTEN;
  struct iovec iov[3] = {
    { .iov_base = &nlmsghdr, .iov_len = sizeof(nlmsghdr) },
    { .iov_base = &cn_msg, .iov_len = sizeof(cn_msg) },
    { .iov_base = &op, .iov_len = sizeof(op) },
  };

  if (writev(sock, iov, 3) < 0)
   goto on_fail;

  return sock;

  on_fail:
  if (sock >= 0) close(sock);
  return 0;
}

/* should be wrapped with mutex lock/unlock out of init func */
int pstrack_record(struct pstrack *t, pid_t pid, int reload) {
  if (pid >= t->procs_cap) {
    size_t new_cap = pid + 64;
    void *new_mem = realloc(t->procs, new_cap * sizeof(struct pstrack_proc));
    if (!new_mem) return 0;
    t->procs = (struct pstrack_proc *) new_mem;
    memset(t->procs + t->procs_cap, 0, (new_cap - t->procs_cap) * sizeof(struct pstrack_proc));
    t->procs_cap = new_cap;
  }

  struct pstrack_proc *proc = t->procs + pid;
  if (reload || !proc->appear) {
    if (!proc->appear) proc->pathname = (char *) malloc(PATH_MAX);
    proc->appear = 0;
    if (!proc->pathname) return 0;
    if (!pstrack_getppid(&proc->ppid, pid)) proc->ppid = 1;
    if (!pstrack_realpathf(proc->pathname, "/proc/%d/exe", pid))
      { free(proc->pathname); proc->pathname = NULL; return 0; }
    proc->appear = 1;
  }

  return 1;
}

void *pstrack_background(void *userdata) {
  struct pstrack *t = (struct pstrack *) userdata;

  int sock = pstrack_forkstat_sock();
  if (sock < 0) goto on_end;

  while (t->running) {
    static char buf[4096];
    ssize_t len = recv(sock, buf, sizeof(buf), 0);
    if (len <= 0) break;

    for (struct nlmsghdr *nlmsghdr = (struct nlmsghdr *) buf;
      NLMSG_OK(nlmsghdr, len);
      nlmsghdr = NLMSG_NEXT(nlmsghdr, len)) {
      struct cn_msg *cn_msg = NLMSG_DATA(nlmsghdr);
      if ((cn_msg->id.idx != CN_IDX_PROC) || (cn_msg->id.val != CN_VAL_PROC))
        continue;
      struct proc_event *event = (struct proc_event *) cn_msg->data;
      pid_t pid = 0; int reload = 0;
      if (event->what == PROC_EVENT_FORK) {
        pid = event->event_data.fork.child_pid;
        reload = 1;
      }
      else if (event->what == PROC_EVENT_EXEC) {
        pid = event->event_data.exec.process_pid;
      }
      else continue;

      if (pthread_mutex_lock(&t->mutex)) continue;
      pstrack_record(t, pid, reload);
      if (pthread_mutex_unlock(&t->mutex)) goto on_end;
    }
  }

  on_end:
  if (sock >= 0) close(sock);
  t->running = 0;
  return NULL;
}

void pstrack_fini(struct pstrack *t) {
  if (t->pthread_inited) {
    // if (!pthread_kill(&t->pthread, 0))
    if (!pthread_cancel(t->pthread))
      t->pthread_inited = 0;
  }
  if (t->mutex_inited) {
    if (!pthread_mutex_destroy(&t->mutex)) t->mutex_inited = 0;
  }
  if (t->procs) { free(t->procs); t->procs = NULL; }
}

int pstrack_init(struct pstrack *t, size_t init_cap) {
  struct pstrack_proc *procs = (struct pstrack_proc *)
    calloc(sizeof(struct pstrack_proc), init_cap);
  if (!procs) return 0;

  t->procs = procs; t->procs_cap = init_cap;
  t->running = t->pthread_inited = t->mutex_inited = 0;

  pthread_mutex_init(&t->mutex, NULL);
  t->mutex_inited = 1;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  // pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&t->pthread, &attr, pstrack_background, t))
    { pstrack_fini(t); return 0; }
  t->running = 1;
  pthread_attr_destroy(&attr);
  t->pthread_inited = 1;

  /* post init: record all procs */
  DIR *dir = opendir("/proc");
  if (dir) for (struct dirent *d = NULL; (d = readdir(dir)) && d->d_name[0];) {
    char ch = d->d_name[0]; if (ch < '0' || ch > '9') continue;
    errno = 0;
    pid_t pid = strtoul(d->d_name, NULL, 10);
    if (!errno) pstrack_record(t, pid, 0);
  }
  closedir(dir);

  return 1;
}

int pstrack_alive(struct pstrack *t) { return t->running; }

int pstrack_stop(struct pstrack *t) {
  if (!t->running) return 1;
  t->running = 0;
  void *ret = NULL;
  return !pthread_join(t->pthread, ret);
}

int pstrack_query(struct pstrack *t, struct pstrack_procinfo *dest_procinfo, pid_t pid) {
  if (pthread_mutex_lock(&t->mutex)) return 0;
  if (!pstrack_record(t, pid, 0)) goto on_fail;

  if (pid > t->procs_cap) goto on_fail;
  struct pstrack_proc *proc = t->procs + pid;
  if (!proc->appear) goto on_fail;
  dest_procinfo->ppid = proc->ppid;
  if (!strncpy(dest_procinfo->pathname, proc->pathname, PATH_MAX)) goto on_fail;

  if (pthread_mutex_unlock(&t->mutex)) return 0;
  return 1;

  on_fail:
  pthread_mutex_unlock(&t->mutex);
  return 0;
}

