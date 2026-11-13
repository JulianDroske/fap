#include "tinyradix.h"

#include "stddef.h"
#include "stdlib.h"
#include "string.h"

/*
  a compressed radix tree implementation with buffered memory
  may store useless nodes and string data, but prevents memory holes
*/

// void dump_tinyradix(struct tinyradix *r, int index, int depth);

void tinyradix_fini(struct tinyradix *r) {
  if (r->nodes) { free(r->nodes); r->nodes = NULL; r->nodes_cap = 0; }
  if (r->buf) { free(r->buf); r->buf = NULL; r->buf_cap = 0; }
}

int tinyradix_init(struct tinyradix *r) {
  r->buf_cap = 128;
  r->buf = malloc(r->buf_cap);
  if (!r->buf) { tinyradix_fini(r); return 0; }
  r->buf_len = 0;

  r->nodes_cap = 16;
  r->nodes = malloc(r->nodes_cap * sizeof(struct tinyradix_node));
  if (!r->nodes) { tinyradix_fini(r); return 0; }
  r->nodes_len = 0;

  r->parent_index = -1; r->max_prefix_index = -1; r->max_prefix_len = 0;

  return 1;
}

int tinyradix_check(struct tinyradix *r, int nodes_inc, size_t buf_inc) {
  if (r->buf_len + buf_inc > r->buf_cap) {
    int buf_cap = r->buf_cap + 128;
    void *mem = realloc(r->buf, buf_cap);
    if (!mem) return 0;
    r->buf = (char *) mem; r->buf_cap = buf_cap;
  }

  if (r->nodes_len + nodes_inc > r->nodes_cap) {
    int nodes_cap = r->nodes_cap + 16;
    void *mem = realloc(r->nodes, nodes_cap * sizeof(struct tinyradix_node));
    if (!mem) return 0;
    r->nodes = (struct tinyradix_node *) mem; r->nodes_cap = nodes_cap;
  }

  return 1;
}

void tinyradix_attach(struct tinyradix *r, struct tinyradix_node *parent, struct tinyradix_node *node) {
  // printf(": attach: %d <- %d\n", parent->index, node->index);
  node->before_len = parent->before_len + parent->key_len;
  node->sibling = -1;

  if (parent->left < 0) { parent->left = parent->right = node->index; return; }

  struct tinyradix_node *child = r->nodes + parent->right;
  parent->right = child->sibling = node->index;
}

struct tinyradix_node *tinyradix_node(struct tinyradix *r, size_t key_off, size_t len, int leaf) {
  if (!tinyradix_check(r, 1, 0)) return NULL;

  struct tinyradix_node *node = r->nodes + r->nodes_len;
  node->index = r->nodes_len;
  node->left = node->right = node->sibling = -1;
  node->key_off = key_off; node->key_len = len;
  node->before_len = 0;
  node->leaf = leaf;

  ++r->nodes_len;
  return node;
}

int tinyradix_alloc(struct tinyradix *r, struct tinyradix_node **dest, const char *key, int len, int leaf) {
  if (!tinyradix_check(r, 1, len)) return 0;

  size_t key_off = r->buf_len;
  memcpy(r->buf + r->buf_len, key, len);
  r->buf_len += len;

  struct tinyradix_node *node = tinyradix_node(r, key_off, len, leaf);
  if (!node) return 0;

  ++r->nodes_len;
  if (dest) *dest = node;
  return 1;
}

int tinyradix_search(struct tinyradix *r, const char *key, int len) {
  if (!r->nodes_len) return 0;

  r->parent_index = -1;
  r->max_prefix_index = -1; r->max_prefix_len = 0;

  int index = 0;
  while (index >= 0) {
    struct tinyradix_node *node = r->nodes + index;

    const char *node_key = r->buf + node->key_off; int node_key_len = node->key_len;
    const char *k = key; int l = len;
    while (node_key_len && l && *node_key++ == *k++) { --node_key_len; --l; }
    /* for tinyradix_record() */
    int prefix_len = node->key_len - node_key_len;
    if (prefix_len > r->max_prefix_len) {
      r->max_prefix_index = index; r->max_prefix_len = prefix_len;
    }
    if (node_key_len) { index = node->sibling; continue; }
    if (node->leaf) return node->before_len + node->key_len;
    if (!l) return 0;
    key = k; len = l;
    // printf("-> '%.*s'\n", len, key);

    /* deeper down */
    if (node->left < 0) break;
    r->parent_index = index; index = node->left;
    r->max_prefix_index = -1; r->max_prefix_len = 0;
  }

  return 0;
}

int tinyradix_record(struct tinyradix *r, const char *key, int len) {
  if (!tinyradix_check(r, 1, 0)) return 0;

  if (!r->nodes_len) return tinyradix_alloc(r, NULL, key, len, 1);
  if (tinyradix_search(r, key, len)) return 1;
  // printf(": search fail ('%.*s'): valid[%d] parent[%d]\n", len, key, r->max_prefix_index, r->parent_index);
  // puts("    | valid:");
  // dump_tinyradix(r, r->max_prefix_index, 3);
  // puts("    | parent:");
  // dump_tinyradix(r, r->parent_index, 3);

  struct tinyradix_node *parent = NULL;
  if (r->max_prefix_index >= 0) {
    parent = r->nodes + r->max_prefix_index;
    // printf(": split: [%d] '%.*s' at %d\n", parent->index, parent->key_len, parent->key, r->max_prefix_len);

    /* split node */
    struct tinyradix_node *tail = tinyradix_node(r, parent->key_off + r->max_prefix_len, parent->key_len - r->max_prefix_len, parent->leaf);
    if (!tail) return 0;
    tail->left = parent->left; tail->right = parent->right; tail->sibling = parent->sibling;

    parent->left = parent->right = parent->sibling = -1;
    parent->key_len = r->max_prefix_len;
    /* the leaf prop will be set later */
    // parent->leaf = !len;
    parent->leaf = 0;
    tinyradix_attach(r, parent, tail);
  }
  else if (r->parent_index >= 0) parent = r->nodes + r->parent_index;
  else return 0;

  int before_len = parent->before_len + parent->key_len;
  key += before_len; len -= before_len;
  if (!len) { parent->leaf = 1; return 1; }
  struct tinyradix_node *node = NULL;
  if (!tinyradix_alloc(r, &node, key, len, 1)) return 0;
  tinyradix_attach(r, parent, node);
  return 1;
}


