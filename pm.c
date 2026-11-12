#include "stdio.h"
#include "stdlib.h"
#include "stddef.h"
#include "stdarg.h"
#include "string.h"

#include "unistd.h"

#include "sys/wait.h"

#define PM_NAME_MAX 256

int pm_readexecv(char *buf, size_t size, const char **argv) {
  int len = 0;

  int link[2] = {};
  if (pipe(link) < 0) return 0;
  pid_t pid = fork();
  if (pid < 0) return 0;

  if (pid) {
    close(link[1]);
    len = read(link[0], buf, size);
    waitpid(pid, NULL, 0);
  }
  else {
    dup2(link[1], STDOUT_FILENO);
    close(link[0]); close(link[1]);
    /* https://pubs.opengroup.org/onlinepubs/9699919799/functions/exec.html */
    execvp(argv[0], (char *const*) argv);
    perror("execvp"); exit(1);
  }

  buf[len] = 0;
  return 1;
}

int pm_readexec(char *buf, size_t size, ...) {
  static const char *argv[16] = {};
  int argv_len = 0;

  va_list args; va_start(args, size);
  const char *arg = NULL;
  while ((arg = va_arg(args, const char *)) != NULL) {
    argv[argv_len] = arg;
    if (++argv_len >= 15) break;
  }
  argv[argv_len] = NULL;
  va_end(args);

  return pm_readexecv(buf, size, argv);
}

int pm_systemf(const char *format, ...) {
  static char cmdline[256];
  va_list args; va_start(args, format);
  vsnprintf(cmdline, sizeof(cmdline), format, args);
  va_end(args);
  return !system(cmdline);
}


struct pm {
  const char *name;
  int (*queryfile)(char *dest, const char *file);
};

int pm_apk_queryfile(char *dest, const char *filepath) {
  return pm_readexec(dest, PM_NAME_MAX, "apk", "info", "-qW", filepath, NULL);
}

struct pm pm_backends[] = {
  { "apk", pm_apk_queryfile },
  { NULL },
};

struct pm *pm_autoget() {
  for (struct pm *pm = pm_backends; pm->name; ++pm) {
    if (pm_systemf("command -v '%s' > /dev/null 2>&1", pm->name))
      return pm;
  }
  return NULL;
}

struct pm *pm_get(const char *type) {
  if (!type) return pm_autoget();
  for (struct pm *pm = pm_backends; pm->name; ++pm) {
    if (!strcmp(pm->name, type)) return pm;
  }
  return NULL;
}

int pm_queryfile(struct pm *pm, char *dest, const char *filepath) {
  if (!pm->queryfile(dest, filepath)) return 0;
  int len = strlen(dest);
  while (len && dest[len - 1] == '\n') dest[--len] = 0;
}

