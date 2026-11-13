#ifndef _TINYRADIX_H_
#define _TINYRADIX_H_

#include "stddef.h"

struct tinyradix_node {
  /* left/right for parents, sibling for children */
  /* auto calc fields */
  int index; int before_len;
  int left, right, sibling;
  size_t key_off, key_len;
  int leaf;
};

struct tinyradix {
  char *buf; size_t buf_len, buf_cap;
  struct tinyradix_node *nodes; int nodes_len, nodes_cap;

  int parent_index;
  int max_prefix_index; int max_prefix_len;
};

int tinyradix_init(struct tinyradix *r);
void tinyradix_fini(struct tinyradix *r);

int tinyradix_record(struct tinyradix *r, const char *key, int len);
int tinyradix_search(struct tinyradix *r, const char *key, int len);

#endif

