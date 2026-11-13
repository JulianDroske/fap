/* enables open_by_handle_at() */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1
#endif

#include "fatrack.h"

#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "stdint.h"
#include "stdarg.h"

#include "errno.h"
#include "fcntl.h"
#include "unistd.h"
#include "sys/statfs.h"
#include "sys/fanotify.h"
// #include "linux/fanotify.h"
#include "linux/limits.h"
#include "mntent.h"

#include "tinyradix.h"

int fatrack_readlinkf(char *dest, const char *format, ...) {
  static char path[PATH_MAX];
  va_list args; va_start(args, format);
  vsnprintf(path, sizeof(path), format, args);
  va_end(args);

  ssize_t len = readlink(path, dest, PATH_MAX - 1);
  if (len < 0) return 0;
  dest[len] = 0;
  return 1;
}

int fatrack_realpathf(char *dest, const char *format, ...) {
  static char path[PATH_MAX];
  va_list args; va_start(args, format);
  vsnprintf(path, sizeof(path), format, args);
  va_end(args);
  return realpath(path, dest) != NULL;
}

int fatrack_haserror(struct fatrack *fatrack) {
  return fatrack->error_len > 0;
}

void fatrack_clearerror(struct fatrack *fatrack) {
  fatrack->error_len = 0; fatrack->error[0] = 0;
}

void fatrack_seterrorf(struct fatrack *fatrack, const char *format, ...) {
  if (fatrack->error_len + 3 >= sizeof(fatrack->error)) return;
  if (fatrack->error_len)
    fatrack->error_len += snprintf(fatrack->error + fatrack->error_len,
      sizeof(fatrack->error) - fatrack->error_len, ", ");

  va_list print_args; va_start(print_args, format);
  fprintf(stderr, "fatrack: "); vfprintf(stderr, format, print_args); fprintf(stderr, "\n");
  va_end(print_args);

  va_list args; va_start(args, format);
  fatrack->error_len += vsnprintf(fatrack->error + fatrack->error_len,
    sizeof(fatrack->error) - fatrack->error_len, format, args);
  va_end(args);

  fatrack->error[fatrack->error_len] = 0;
}

int fatrack_fsinfo(struct fatrack *fatrack, struct fatrack_fsinfo *info, const char *pathname) {
  int fd = open(pathname, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    fatrack_seterrorf(fatrack, "failed to open path: %s: %d", pathname, errno);
    return 0;
  }
  struct statfs s;
  if (fstatfs(fd, &s) < 0) {
    fatrack_seterrorf(fatrack, "failed to statfs: %s: %d", pathname, errno);
    close(fd); return 0;
  }

  memcpy(&info->fsid, &s.f_fsid, sizeof(fsid_t));
  info->mount_fd = fd;
  return 1;
}

int fatrack_match_pathname(struct tinyradix *r, const char *path, int path_len) {
  int prefix_len = tinyradix_search(r, path, path_len);
  if (prefix_len > 0) {
    const char *end = path + prefix_len - 1;
    if (*end++ == '/') return 1;
    return !*end || *end == '/';
  }
  return 0;
}

void fatrack_fini(struct fatrack *fatrack) {
  if (fatrack->fsinfos) {
    for (int i = 0, n = fatrack->fsinfos_len; i < n; ++i) {
      struct fatrack_fsinfo *info = fatrack->fsinfos + i;
      close(info->mount_fd);
    }
    free(fatrack->fsinfos);
    fatrack->fsinfos = NULL; fatrack->fsinfos_len = 0;
  }
  if (fatrack->fd >= 0) {
    close(fatrack->fd); fatrack->fd = -1;
  }
  tinyradix_fini(&fatrack->prefix_radix);
}

int fatrack_init(struct fatrack *fatrack, uint64_t mark_masks) {
  fatrack_clearerror(fatrack);

  fatrack->mark_masks = mark_masks;
  fatrack->paths_len = 0;

  if (!tinyradix_init(&fatrack->prefix_radix)) {
    fatrack_seterrorf(fatrack, "failed to init prefix radix: %d", errno);
    return 0;
  }

  fatrack->fd = fanotify_init(FAN_CLOEXEC | FAN_CLASS_NOTIF | FAN_REPORT_FID | FAN_REPORT_DIR_FID | FAN_REPORT_NAME,
    O_RDONLY | O_LARGEFILE);
  if (fatrack->fd < 0) {
    fatrack_seterrorf(fatrack, "fanotify: %d", errno);
    fatrack_fini(fatrack);
    return 0;
  }

  fatrack->fsinfos_len = 0; fatrack->fsinfos_cap = 16;
  fatrack->fsinfos = (struct fatrack_fsinfo *)
    malloc(fatrack->fsinfos_cap * sizeof(struct fatrack_fsinfo));
  if (!fatrack->fsinfos) {
    fatrack_seterrorf(fatrack, "malloc: %d", errno);
    fatrack_fini(fatrack);
    return 0;
  }

  fatrack->self_pid = getpid();
  return 1;
}

int fatrack_add_fsinfo(struct fatrack *fatrack, const char *pathname) {
  if (fatrack->fsinfos_len + 1 > fatrack->fsinfos_cap) {
    int fsinfos_cap = fatrack->fsinfos_cap + 16;
    void *new = realloc(fatrack->fsinfos, fsinfos_cap * sizeof(struct fatrack_fsinfo));
    if (!new) {
      fatrack_seterrorf(fatrack, "failed to extend fsinfo array: %d", errno);
      return 0;
    }
    fatrack->fsinfos = (struct fatrack_fsinfo *) new;
    fatrack->fsinfos_cap = fsinfos_cap;
  }

  struct fatrack_fsinfo *info = fatrack->fsinfos + fatrack->fsinfos_len;
  if (!fatrack_fsinfo(fatrack, info, pathname)) return 0;
  ++fatrack->fsinfos_len;
  return 1;
}

int fatrack_markpath(struct fatrack *fatrack, const char *path) {
  fatrack_clearerror(fatrack);

  if (fanotify_mark(fatrack->fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, fatrack->mark_masks,
    AT_FDCWD, path) < 0) {
    fatrack_seterrorf(fatrack, "failed to mark directory %s: %d", path, errno);
    return 0;
  }

  if (!fatrack_add_fsinfo(fatrack, path))
    fatrack_seterrorf(fatrack, "failed to add fsinfo for dir %s", path);

  if (!tinyradix_record(&fatrack->prefix_radix, path, strlen(path))) {
    fatrack_seterrorf(fatrack, "failed to record tinyradix node"); return 0;
  }
  ++fatrack->paths_len;
  return 1;
}

int fatrack_marksubmounts(struct fatrack *fatrack) {
  fatrack_clearerror(fatrack);

  FILE *mounts = setmntent("/proc/self/mounts", "r");
  if (!mounts) {
    fatrack_seterrorf(fatrack, "/proc/self/mounts: %d", errno);
    return 0;
  }
  for (struct mntent *mount = NULL; (mount = getmntent(mounts)) != NULL;) {
    const char *mountpoint = mount->mnt_dir;
    ssize_t mountpoint_len = strlen(mountpoint);
    int matched = !fatrack->paths_len;
    if (!matched)
      matched = fatrack_match_pathname(&fatrack->prefix_radix, mountpoint, mountpoint_len);

    if (!matched) continue;
    if (fanotify_mark(fatrack->fd, FAN_MARK_ADD | FAN_MARK_FILESYSTEM, fatrack->mark_masks,
      AT_FDCWD, mountpoint) < 0)
      { fatrack_seterrorf(fatrack, "fanotify_mark: %s: %d", mountpoint, errno); continue; }

    if (!fatrack_add_fsinfo(fatrack, mountpoint))
      fatrack_seterrorf(fatrack, "failed to add fsinfo for mountpoint %s", mountpoint);
  }
  endmntent(mounts);

  return 1;
}

int fatrack_next(struct fatrack *fatrack, struct fatrack_event *ev) {
  fatrack_clearerror(fatrack);

  while (FAN_EVENT_OK(fatrack->metadata, fatrack->read_size)) {
    struct fanotify_event_metadata *data = fatrack->metadata;
    fatrack->metadata = FAN_EVENT_NEXT(fatrack->metadata, fatrack->read_size);
    struct fanotify_event_info_fid *fid = (struct fanotify_event_info_fid *)
      (data + 1);
    if (fid->hdr.info_type != FAN_EVENT_INFO_TYPE_DFID_NAME) continue;

    struct file_handle *handle = (struct file_handle *) fid->handle;
    const char *filename = (char *) handle->f_handle + handle->handle_bytes;

    if (data->pid == fatrack->self_pid) continue;

    int mount_fd = AT_FDCWD;
    if (fatrack->fsinfos) {
      fsid_t *fsid = (fsid_t *) &fid->fsid;
      for (int i = 0, n = fatrack->fsinfos_len; i < n; ++i) {
        struct fatrack_fsinfo *info = fatrack->fsinfos + i;
        if (!memcmp(&info->fsid, fsid, sizeof(fsid_t)))
          { mount_fd = info->mount_fd; break; }
      }
      if (mount_fd == AT_FDCWD)
        fatrack_seterrorf(fatrack, "fsid not found");
    }

    int fd = open_by_handle_at(mount_fd, handle, O_RDONLY | O_LARGEFILE | O_PATH);
    if (fd < 0) {
      // if (errno != ESTALE)
      fatrack_seterrorf(fatrack, "open_by_handle_at: %d", errno);
      continue;
    }

    ev->metadata = *data;
    ev->filepath[0] = 0;
    if (!fatrack_readlinkf(ev->filepath, "/proc/self/fd/%d", fd))
      fatrack_seterrorf(fatrack, "failed to get filepath from fd: %d", fd, errno);
    size_t dirpath_len = strlen(ev->filepath); ev->filepath[dirpath_len++] = '/';
    strncpy(ev->filepath + dirpath_len, filename, PATH_MAX - dirpath_len);

    close(fd);

    if (fatrack->paths_len && !fatrack_match_pathname(&fatrack->prefix_radix, ev->filepath, strlen(ev->filepath)))
      continue;

    return 1;
  }

  fatrack->read_size = read(fatrack->fd, fatrack->events_buf,
    sizeof(fatrack->events_buf));
  if (!fatrack->read_size) return -1;
  else if (fatrack->read_size < 0) {
    if (errno == EINTR) return 0;
    return -1;
  }
  fatrack->metadata = (struct fanotify_event_metadata *) fatrack->events_buf;
  return 0;
}

