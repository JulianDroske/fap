#ifndef _TINYCLI_H_
#define _TINYCLI_H_

struct tinycli_option {
  const char *name; char alias;
  int follows;
  const char ***val; int met;
};

int tinycli_parse(struct tinycli_option *opts, int argc, const char **argv);

#endif

