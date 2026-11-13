#ifndef _PSTRACK_H_
#define _PSTRACK_H_

#include "stddef.h"
#include "sys/types.h"
#include "linux/limits.h"

struct pstrack_procinfo {
  pid_t ppid;
  char pathname[PATH_MAX];
};

/* internal storage */
struct pstrack_proc {
  char *pathname; pid_t ppid; int appear;
};

struct pstrack {
  struct pstrack_proc *procs; size_t procs_cap;

  int running;

  pthread_t pthread; int pthread_inited;
  pthread_mutex_t mutex; int mutex_inited;
};

int pstrack_init(struct pstrack *t, size_t init_cap);
void pstrack_fini(struct pstrack *t);

int pstrack_alive(struct pstrack *t);
int pstrack_stop(struct pstrack *t);
int pstrack_query(struct pstrack *t, struct pstrack_procinfo *dest_procinfo, pid_t pid);

#endif

