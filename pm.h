#ifndef _PM_H_
#define _PM_H_

#define PM_NAME_MAX 256

struct pm {
  const char *name;
  int (*queryfile)(char *dest, const char *file);
  int (*checkpkg)(int *exist, const char *name);
};

struct pm *pm_autoget();
struct pm *pm_get(const char *type);

#endif

