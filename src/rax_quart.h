/* Redis rax with QuART optimizations: bridge detection and reset counter
 *
 * Copyright (c) 2026, Redis Ltd.
 * All rights reserved.
 *
 * This extends rax with fast-path optimizations:
 * - Bridge detection at byte boundaries
 * - Reset counter for workload adaptation
 */

#ifndef RAX_QUART_H
#define RAX_QUART_H

#include "rax.h"
#include <stdint.h>

/* Extended rax structure with fast-path state */
typedef struct raxQuart {
    rax *rax;                    /* Underlying rax tree */
    uint32_t last_key;           /* Last inserted key for fast-path tracking */
    int reset_counter;           /* Counter for reset mechanism (default: 300) */
    int has_last_key;            /* Whether last_key is valid */
    raxNode **fp_ref;            /* Fast-path reference node pointer */
    int fp_depth;                /* Depth of fp_ref in the tree */
} raxQuart;

/* Create a new rax with QuART optimizations */
raxQuart *raxQuartNew(void);
raxQuart *raxQuartNewWithMetadata(int metaSize, size_t *alloc_size);

/* Insert with bridge detection and reset logic */
int raxQuartInsert(raxQuart *rq, unsigned char *key, size_t keylen, void *data, void **old);

/* Standard rax operations */
int raxQuartFind(raxQuart *rq, unsigned char *key, size_t keylen, void **value);
int raxQuartRemove(raxQuart *rq, unsigned char *key, size_t keylen, void **old);
void raxQuartFree(raxQuart *rq);
uint64_t raxQuartSize(raxQuart *rq);

/* Configuration */
void raxQuartSetResetThreshold(raxQuart *rq, int threshold);
int raxQuartGetResetThreshold(raxQuart *rq);
#endif /* RAX_QUART_H */
