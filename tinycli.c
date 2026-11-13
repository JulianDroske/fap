#include "tinycli.h"

#include "string.h"

int tinycli_parse(struct tinycli_option *opts, int argc, const char **argv) {
  int pos = 0;
  while (pos < argc) {
    const char *arg = argv[pos];
    if (*arg++ != '-') break;

    struct tinycli_option *target = NULL;
    if (*arg != '-') {
      /* short option, no conjunction is allowed */
      if (!arg[0] || arg[1]) return -pos - 1;
      for (struct tinycli_option *opt = opts; opt->name; ++opt)
        if (opt->alias && opt->alias == *arg) { target = opt; break; }
    }
    else {
      ++arg;
      for (struct tinycli_option *opt = opts; opt->name; ++opt)
        if (opt->name && !strcmp(opt->name, arg)) { target = opt; break; }
    }

    if (!target) return -pos - 1;
    int needs = target->follows + 1;
    if (target->met || pos + needs > argc) return -pos - 1;
    target->met = 1; if (target->val) *target->val = argv + pos;
    pos += needs;
  }

  return pos;
}

