/* rax_quart.h -- QuART's stail fast path and tail cache for Redis' rax.
 *
 * Copyright (c) 2026, Redis Ltd.
 * All rights reserved.
 *
 * A raxQuart wraps an unmodified rax that holds fixed-length keys and keeps
 * two cached pointers into it:
 *
 *  fp   -- QuART's cached insertion point, governed by the stail policy.
 *          It is the parent's pointer slot of the node that begins at depth
 *          <= group_depth on the path of fp_key (the last key inserted
 *          through the fast path), plus that node's start depth. Every key
 *          is classified by its prefix group, the first group_depth bytes,
 *          against fp_key:
 *            FP_INSERT  same group          -> insert starting at fp
 *            BRIDGE     adjacent group (+-1) -> insert from the root, move fp
 *            OTHER      anything else       -> insert from the root, keep fp;
 *                                              after RAX_QUART_RESET_THRESHOLD
 *                                              consecutive outliers fp is
 *                                              re-anchored on the current key
 *          group_depth = keylen-1 reproduces QuART_stail for 32-bit keys
 *          (256-key groups). For the 16-byte stream IDs (8-byte millisecond
 *          time, 8-byte sequence) group_depth = 7 groups the IDs of one
 *          256-millisecond window.
 *
 *  tail -- the leaf holding the maximum key. A caller that appends at the
 *          tail (a stream) gets the tail key and value without a traversal
 *          and can replace the value in place (raxQuartSetTailData).
 *
 * The wrapper owns every mutation of the tree. A mutation done on the
 * underlying rax directly (raxRemove, defrag) must be followed by
 * raxQuartInvalidate(), after which both caches are rebuilt lazily.
 */

#ifndef RAX_QUART_H
#define RAX_QUART_H

#include <stdint.h>
#include <stddef.h>
#include "rax.h"

#define RAX_QUART_MAX_KEYLEN 16
#define RAX_QUART_RESET_THRESHOLD 16

typedef struct raxQuartStats {
    uint64_t fp_insert;   /* keys inserted starting at fp */
    uint64_t bridge;      /* keys in the adjacent group: from root, fp moved */
    uint64_t other;       /* outliers: from root, fp kept */
    uint64_t reset;       /* outliers that re-anchored fp (counter expired) */
    uint64_t tail_hit;    /* raxQuartTail() answered from the cache */
    uint64_t tail_seek;   /* raxQuartTail() had to seek the tree */
} raxQuartStats;

typedef struct raxQuart {
    rax *rax;                   /* Underlying rax tree. */
    int keylen;                 /* Fixed key length, 1..RAX_QUART_MAX_KEYLEN. */
    int group_depth;            /* Bytes [0,group_depth) identify the prefix group. */
    int reset_counter;          /* Outliers left before fp is re-anchored. */
    int has_fp_key;             /* fp_key/fp_ref are valid. */
    unsigned char fp_key[RAX_QUART_MAX_KEYLEN]; /* Key of the last fast-path insert. */
    raxNode **fp_ref;           /* Parent's pointer slot of the fp node. */
    int fp_depth;               /* Key depth at which the fp node begins. */
    int has_tail;               /* tail_node/tail_key are valid. */
    raxNode *tail_node;         /* Node holding the maximum key. */
    unsigned char tail_key[RAX_QUART_MAX_KEYLEN];
    uint64_t gen;               /* rax->gen when the caches were last known valid. */
    int owns_rax;               /* raxQuartFree() frees the rax too. */
    raxQuartStats stats;
} raxQuart;

/* 4-byte keys, group_depth 3: QuART_stail on 32-bit integers. */
raxQuart *raxQuartNew(void);
raxQuart *raxQuartNewWithMetadata(int metaSize, size_t *alloc_size, int keylen, int group_depth);
void raxQuartFree(raxQuart *rq);
/* Wrap a rax owned by someone else (its mutations outside the wrapper are
 * detected through rax->gen); raxQuartUnwrap() frees only the wrapper. */
raxQuart *raxQuartWrap(rax *rax, int keylen, int group_depth);
void raxQuartUnwrap(raxQuart *rq);

/* Same contract as raxInsert()/raxFind()/raxRemove(). A key whose length is
 * not the configured one is passed to rax directly. */
int raxQuartInsert(raxQuart *rq, unsigned char *key, size_t keylen, void *data, void **old);
int raxQuartTryInsert(raxQuart *rq, unsigned char *key, size_t keylen, void *data, void **old); /* no overwrite */
int raxQuartFind(raxQuart *rq, unsigned char *key, size_t keylen, void **value);
int raxQuartRemove(raxQuart *rq, unsigned char *key, size_t keylen, void **old);
uint64_t raxQuartSize(raxQuart *rq);

/* Tail cache. raxQuartTail() copies the maximum key into 'key' (keylen
 * bytes) and its value into '*data'; returns 0 if the tree is empty.
 * raxQuartSetTailData() replaces the value of 'key' in place when it is the
 * cached tail, and falls back to a normal insert otherwise. */
int raxQuartTail(raxQuart *rq, unsigned char *key, void **data);
void raxQuartSetTailData(raxQuart *rq, unsigned char *key, size_t keylen, void *data);
void raxQuartInvalidate(raxQuart *rq);

/* Process-wide counters summed over all raxQuart instances. */
void raxQuartGlobalStats(raxQuartStats *out);
void raxQuartGlobalStatsReset(void);

/* Kept for the standalone harness. */
void raxQuartSetResetThreshold(raxQuart *rq, int threshold);
int raxQuartGetResetThreshold(raxQuart *rq);

#endif /* RAX_QUART_H */
