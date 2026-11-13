#ifndef _FATRACK_H_
#define _FATRACK_H_

#include "stdint.h"
#include "sys/types.h"
#include "sys/statfs.h"
#include "sys/fanotify.h"
#include "linux/limits.h"

#include "tinyradix.h"

struct fatrack_fsinfo { int mount_fd; fsid_t fsid; };

struct fatrack {
  /* params */
  uint64_t mark_masks;
  struct tinyradix prefix_radix;
  struct fatrack_fsinfo *fsinfos; int fsinfos_len, fsinfos_cap;

  char error[256]; int error_len;

  /* internals */
  int paths_len;
  int fd;
  pid_t self_pid;

  struct fanotify_event_metadata *metadata;
  char events_buf[4096]; ssize_t read_size;
};

struct fatrack_event {
  char filepath[PATH_MAX];
  struct fanotify_event_metadata metadata;
};

int fatrack_init(struct fatrack *fatrack, uint64_t mark_masks);
void fatrack_fini(struct fatrack *fatrack);
int fatrack_haserror(struct fatrack *fatrack);
void fatrack_clearerror(struct fatrack *fatrack);

int fatrack_markpath(struct fatrack *fatrack, const char *path);
int fatrack_marksubmounts(struct fatrack *fatrack);
int fatrack_next(struct fatrack *fatrack, struct fatrack_event *ev);

#endif

