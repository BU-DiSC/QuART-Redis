/* Redis rax with QuART optimizations: bridge detection, reset counter, and bidirectional support
 *
 * Copyright (c) 2026, Redis Ltd.
 * All rights reserved.
 *
 * This extends rax with fast-path optimizations:
 * - Bridge detection at byte boundaries
 * - Reset counter for workload adaptation
 * - Bidirectional insertion tracking
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
    int direction;               /* 1=forward/ascending, 0=backward/descending */
    uint64_t fp_inserts;         /* Fast-path insertions count */
    uint64_t regular_inserts;    /* Regular insertions count */
    uint64_t bridge_detected;    /* Number of bridges detected */
    uint64_t resets;             /* Number of resets triggered */
    int has_last_key;            /* Whether last_key is valid */
    raxNode **fp_ref;            /* Fast-path reference node pointer */
    int fp_depth;                /* Depth of fp_ref in the tree */
} raxQuart;

/* Create a new rax with QuART optimizations */
raxQuart *raxQuartNew(void);

/* Insert with bridge detection and reset logic */
int raxQuartInsert(raxQuart *rq, uint32_t key, void *data);

/* Standard rax operations */
int raxQuartFind(raxQuart *rq, uint32_t key, void **value);
int raxQuartRemove(raxQuart *rq, uint32_t key, void **old);
void raxQuartFree(raxQuart *rq);
uint64_t raxQuartSize(raxQuart *rq);

/* Configuration */
void raxQuartSetResetThreshold(raxQuart *rq, int threshold);
int raxQuartGetResetThreshold(raxQuart *rq);
void raxQuartSetDirection(raxQuart *rq, int direction);
int raxQuartGetDirection(raxQuart *rq);

/* Statistics */
void raxQuartGetStats(raxQuart *rq, 
                       uint64_t *fp_inserts,
                       uint64_t *regular_inserts, 
                       uint64_t *bridges,
                       uint64_t *resets);

#endif /* RAX_QUART_H */
