/* rax_quart.c -- QuART's stail fast path and tail cache for Redis' rax.
 *
 * Copyright (c) 2026, Redis Ltd.
 * All rights reserved.
 *
 * See rax_quart.h for the design. quartInsertFrom() mirrors
 * raxGenericInsert() of rax.c for fixed-length keys, with two additions:
 *  - anchor tracking: while inserting, remember the deepest node on the key's
 *    path that begins at depth <= group_depth; it becomes fp when done.
 *  - fp preservation: an insert that must not move fp (OTHER) repairs fp when
 *    it reallocates the node holding fp's slot (case A) or splits the
 *    compressed node whose single child is fp (case B).
 */

#include "rax_quart.h"
#include "rax.h"
#include "rax_malloc.h"
#include "zmalloc.h"
#include <string.h>
#include <errno.h>

/* Internal rax functions used for the fast-path insertion. */
extern raxNode *raxAddChild(rax *rax, raxNode *n, unsigned char c, raxNode **childptr, raxNode ***parentlink);
extern raxNode *raxCompressNode(rax *rax, raxNode *n, unsigned char *s, size_t len, raxNode **child);
extern raxNode *raxReallocForData(rax *rax, raxNode *n, void *data);
extern raxNode *raxNewNode(rax *rax, size_t children, int datafield);
extern void raxFreeNode(rax *rax, raxNode *n);
extern void *raxGetData(raxNode *n);

/* rax node layout helpers (must match definitions in rax.c) */
#define raxPadding(nodesize) ((sizeof(void*)-(((nodesize)+4) % sizeof(void*))) & (sizeof(void*)-1))
#define raxNodeCurrentLength(n) ( \
    sizeof(raxNode)+(n)->size+ \
    raxPadding((n)->size)+ \
    ((n)->iscompr ? sizeof(raxNode*) : sizeof(raxNode*)*(n)->size)+ \
    (((n)->iskey && !(n)->isnull)*sizeof(void*)) \
)
#define raxNodeFirstChildPtr(n) ((raxNode**) ( \
    (n)->data + \
    (n)->size + \
    raxPadding((n)->size)))
#define raxNodeLastChildPtr(n) ((raxNode**) ( \
    ((char*)(n)) + \
    raxNodeCurrentLength(n) - \
    sizeof(raxNode*) - \
    (((n)->iskey && !(n)->isnull) ? sizeof(void*) : 0) \
))

static raxQuartStats g_stats; /* process-wide totals */
#define STAT(rq, field) do { (rq)->stats.field++; g_stats.field++; } while (0)

/* Does the child-pointer slot 'slot' live inside the (non-compressed) node
 * 'n'? Such a slot moves when 'n' is reallocated. */
static inline int slotInsideNode(raxNode *n, raxNode **slot) {
    if (n->iscompr) return 0;
    raxNode **base = raxNodeFirstChildPtr(n);
    return slot >= base && slot < base + n->size;
}

/* Is 'a' equal to 'b'+1, both read as big-endian integers of 'n' bytes? */
static int prefixIsSuccessor(const unsigned char *a, const unsigned char *b, size_t n) {
    unsigned char t[RAX_QUART_MAX_KEYLEN];
    memcpy(t, b, n);
    int k = (int)n - 1;
    while (k >= 0) {
        if (++t[k] != 0) break; /* no carry */
        k--;
    }
    if (k < 0) return 0; /* b was all 0xFF: no successor */
    return memcmp(a, t, n) == 0;
}

/* Insert 'key' (rq->keylen bytes) starting at the node pointed to by
 * *start_ref, which begins at key depth 'start_depth'. The bytes
 * key[0..start_depth) must be the path to that node.
 *
 *   anchor:   track the deepest node on the key's path that begins at depth
 *             <= group_depth and make it fp when done.
 *   preserve: keep the current fp valid across the node reallocations and
 *             compressed-node splits this insert performs.
 *
 * '*leaf' receives the node that holds the key's value.
 * Returns 1 if the key was inserted, 0 if it existed (value replaced, *old
 * set) or on out of memory (errno = ENOMEM). */
static int quartInsertFrom(raxQuart *rq, raxNode **start_ref, size_t start_depth,
                           const unsigned char *key, void *data, void **old,
                           int anchor, int preserve, int overwrite, raxNode **leaf)
{
    rax *rax = rq->rax;
    const size_t len = rq->keylen;
    const size_t D = rq->group_depth;
    raxNode **parentlink = start_ref;   /* slot pointing to h */
    raxNode *h = *start_ref;
    size_t i = start_depth;             /* key bytes consumed */
    size_t depth = start_depth;         /* key depth at which h begins */
    size_t j = 0;
    size_t usable;
    size_t dummy, *alloc_size = &dummy;
    if (rax->alloc_size) alloc_size = rax->alloc_size;

    raxNode **best_link = start_ref;
    size_t best_depth = start_depth;
#define TRACK(link, d) do { if (anchor && (d) <= D) { best_link = (link); best_depth = (d); } } while (0)
#define ANCHOR() do { if (anchor) { rq->fp_ref = best_link; rq->fp_depth = (int)best_depth; } } while (0)

    *leaf = NULL;

    /* Walk down while the key matches the tree. */
    while (h->size && i < len) {
        unsigned char *v = h->data;
        if (h->iscompr) {
            for (j = 0; j < h->size && i < len; j++, i++)
                if (v[j] != key[i]) break;
            if (j != h->size) break;
        } else {
            for (j = 0; j < h->size; j++)
                if (v[j] == key[i]) break;
            if (j == h->size) break;
            i++;
        }
        raxNode **children = raxNodeFirstChildPtr(h);
        if (h->iscompr) j = 0;
        raxNode *next;
        memcpy(&next, children + j, sizeof(next));
        parentlink = children + j;
        h = next;
        depth = i;
        TRACK(parentlink, depth);
        j = 0;
    }

    /* The key is already represented by node h. */
    if (i == len && (!h->iscompr || j == 0)) {
        if (h->iskey && (!h->isnull || !overwrite)) {
            if (old) *old = raxGetData(h);
            if (overwrite) raxSetData(h, data);
            *leaf = h;
            ANCHOR();
            errno = 0;
            return 0;
        }
        {
            ptrdiff_t fp_off = -1;
            if (preserve && slotInsideNode(h, rq->fp_ref))
                fp_off = rq->fp_ref - raxNodeFirstChildPtr(h);
            raxNode *newh = raxReallocForData(rax, h, data);
            if (newh == NULL) { errno = ENOMEM; return 0; }
            int was_key = h->iskey;
            h = newh;
            if (fp_off >= 0) rq->fp_ref = raxNodeFirstChildPtr(h) + fp_off;
            memcpy(parentlink, &h, sizeof(h));
            raxSetData(h, data);
            if (!was_key) rax->numele++;
            *leaf = h;
            ANCHOR();
            errno = 0;
            return was_key ? 0 : 1;
        }
    }

    if (h->iscompr && i != len) {
        /* Mismatch inside a compressed node: split it (ALGO 1 of rax.c).
         * h = [trimmed: h->data[0..j)] -> splitnode [h->data[j]] ->
         *     [postfix: h->data[j+1..)] -> next */
        raxNode **childfield = raxNodeLastChildPtr(h);
        raxNode *next;
        memcpy(&next, childfield, sizeof(next));

        size_t trimmedlen = j;
        size_t postfixlen = h->size - j - 1;
        int split_node_is_key = !trimmedlen && h->iskey && !h->isnull;
        size_t nodesize;

        raxNode *splitnode = raxNewNode(rax, 1, split_node_is_key);
        raxNode *trimmed = NULL;
        raxNode *postfix = NULL;

        if (trimmedlen) {
            nodesize = sizeof(raxNode) + trimmedlen + raxPadding(trimmedlen) + sizeof(raxNode*);
            if (h->iskey && !h->isnull) nodesize += sizeof(void*);
            trimmed = rax_malloc_usable(nodesize, &usable);
            *alloc_size += usable;
        }
        if (postfixlen) {
            nodesize = sizeof(raxNode) + postfixlen + raxPadding(postfixlen) + sizeof(raxNode*);
            postfix = rax_malloc_usable(nodesize, &usable);
            *alloc_size += usable;
        }
        if (splitnode == NULL || (trimmedlen && trimmed == NULL) || (postfixlen && postfix == NULL)) {
            raxFreeNode(rax, splitnode);
            raxFreeNode(rax, trimmed);
            raxFreeNode(rax, postfix);
            errno = ENOMEM;
            return 0;
        }

        splitnode->data[0] = h->data[j];

        if (j == 0) {
            /* splitnode replaces h in its parent's slot: same start depth. */
            if (h->iskey) {
                void *ndata = raxGetData(h);
                raxSetData(splitnode, ndata);
            }
            memcpy(parentlink, &splitnode, sizeof(splitnode));
        } else {
            trimmed->size = j;
            memcpy(trimmed->data, h->data, j);
            trimmed->iscompr = j > 1 ? 1 : 0;
            trimmed->iskey = h->iskey;
            trimmed->isnull = h->isnull;
            if (h->iskey && !h->isnull) {
                void *ndata = raxGetData(h);
                raxSetData(trimmed, ndata);
            }
            raxNode **cp = raxNodeLastChildPtr(trimmed);
            memcpy(cp, &splitnode, sizeof(splitnode));
            memcpy(parentlink, &trimmed, sizeof(trimmed));
            parentlink = cp;
            rax->numnodes++;
            TRACK(parentlink, depth + j); /* splitnode begins at depth+j */
        }

        if (postfixlen) {
            postfix->iskey = 0;
            postfix->isnull = 0;
            postfix->size = postfixlen;
            postfix->iscompr = postfixlen > 1;
            memcpy(postfix->data, h->data + j + 1, postfixlen);
            raxNode **cp = raxNodeLastChildPtr(postfix);
            memcpy(cp, &next, sizeof(next));
            rax->numnodes++;
        } else {
            postfix = next;
        }

        raxNode **splitchild = raxNodeLastChildPtr(splitnode);
        memcpy(splitchild, &postfix, sizeof(postfix));

        /* Case B: fp is h's single child and h is going away; point fp_ref
         * at the slot that now holds that child. */
        if (preserve && rq->fp_ref == childfield) {
            if (postfixlen > 0)
                rq->fp_ref = raxNodeLastChildPtr(postfix);
            else
                rq->fp_ref = splitchild;
        }

        raxFreeNode(rax, h);
        h = splitnode;
    } else if (h->iscompr && i == len) {
        /* The key ends inside a compressed node: only possible with keys of
         * different lengths. Nothing was modified yet: let rax handle it. */
        raxQuartInvalidate(rq);
        return overwrite ? raxInsert(rax, (unsigned char*)key, len, data, old)
                         : raxTryInsert(rax, (unsigned char*)key, len, data, old);
    }

    /* Insert the missing suffix starting from node h. */
    while (i < len) {
        raxNode *child;
        if (h->size == 0 && len - i > 1) {
            /* h has no children: compress the remaining bytes into it. */
            size_t comprsize = len - i;
            if (comprsize > RAX_NODE_MAX_SIZE) comprsize = RAX_NODE_MAX_SIZE;
            raxNode *newh = raxCompressNode(rax, h, (unsigned char*)key + i, comprsize, &child);
            if (newh == NULL) { errno = ENOMEM; return 0; }
            h = newh;
            memcpy(parentlink, &h, sizeof(h));
            parentlink = raxNodeLastChildPtr(h);
            i += comprsize;
        } else {
            /* Add key[i] as a new child of h. raxAddChild reallocates h,
             * which moves every slot inside it. Case A: fp_ref is one of
             * those slots; restore it by offset, accounting for the shift
             * when the new byte sorts before fp's byte. */
            ptrdiff_t fp_off = -1;
            unsigned char fp_edge_byte = 0;
            if (preserve && slotInsideNode(h, rq->fp_ref)) {
                fp_off = rq->fp_ref - raxNodeFirstChildPtr(h);
                fp_edge_byte = h->data[fp_off];
            }
            raxNode **new_parentlink;
            raxNode *newh = raxAddChild(rax, h, key[i], &child, &new_parentlink);
            if (newh == NULL) { errno = ENOMEM; return 0; }
            if (fp_off >= 0) {
                if (key[i] < fp_edge_byte) fp_off++;
                rq->fp_ref = raxNodeFirstChildPtr(newh) + fp_off;
            }
            h = newh;
            memcpy(parentlink, &h, sizeof(h));
            parentlink = new_parentlink;
            i++;
            TRACK(parentlink, i); /* the new child begins at depth i */
        }
        rax->numnodes++;
        h = child;
    }

    /* Attach the value at the leaf. */
    raxNode *newh = raxReallocForData(rax, h, data);
    if (newh == NULL) { errno = ENOMEM; return 0; }
    h = newh;
    if (!h->iskey) rax->numele++;
    raxSetData(h, data);
    memcpy(parentlink, &h, sizeof(h));
    *leaf = h;
    ANCHOR();
    errno = 0;
    return 1;
#undef TRACK
#undef ANCHOR
}

/* Remember 'leaf' as the tail if 'key' is the largest key seen. */
static inline void quartNoteLeaf(raxQuart *rq, const unsigned char *key, raxNode *leaf) {
    if (leaf == NULL) return;
    if (!rq->has_tail || memcmp(key, rq->tail_key, rq->keylen) >= 0) {
        rq->tail_node = leaf;
        memcpy(rq->tail_key, key, rq->keylen);
        rq->has_tail = 1;
    }
}

/* Drop the caches if the tree was mutated behind the wrapper's back. */
static inline void quartSync(raxQuart *rq) {
    if (rq->rax->gen != rq->gen) raxQuartInvalidate(rq);
}

static int quartInsert(raxQuart *rq, unsigned char *key, size_t keylen, void *data, void **old, int overwrite) {
    if (!rq) return 0;
    quartSync(rq);
    if ((int)keylen != rq->keylen) {
        /* Foreign key length: plain insert; the caches may now be stale. */
        int r = overwrite ? raxInsert(rq->rax, key, keylen, data, old)
                          : raxTryInsert(rq->rax, key, keylen, data, old);
        raxQuartInvalidate(rq);
        return r;
    }

    raxNode *leaf = NULL;
    int result;
    const size_t D = rq->group_depth;

    if (!rq->has_fp_key) {
        /* First key: insert from the root and anchor fp on it. */
        result = quartInsertFrom(rq, &rq->rax->head, 0, key, data, old, 1, 0, overwrite, &leaf);
        if (errno == ENOMEM) { raxQuartInvalidate(rq); return 0; }
        memcpy(rq->fp_key, key, keylen);
        rq->has_fp_key = 1;
        rq->reset_counter = RAX_QUART_RESET_THRESHOLD;
    } else if (memcmp(key, rq->fp_key, D) == 0) {
        /* FP_INSERT: same prefix group, insert starting at fp. */
        STAT(rq, fp_insert);
        rq->reset_counter = RAX_QUART_RESET_THRESHOLD;
        result = quartInsertFrom(rq, rq->fp_ref, rq->fp_depth, key, data, old, 1, 0, overwrite, &leaf);
        if (errno == ENOMEM) { raxQuartInvalidate(rq); return 0; }
        memcpy(rq->fp_key, key, keylen);
    } else if (prefixIsSuccessor(key, rq->fp_key, D) || prefixIsSuccessor(rq->fp_key, key, D)) {
        /* BRIDGE: adjacent prefix group, the stream advanced; move fp. */
        STAT(rq, bridge);
        rq->reset_counter = RAX_QUART_RESET_THRESHOLD;
        result = quartInsertFrom(rq, &rq->rax->head, 0, key, data, old, 1, 0, overwrite, &leaf);
        if (errno == ENOMEM) { raxQuartInvalidate(rq); return 0; }
        memcpy(rq->fp_key, key, keylen);
    } else if (rq->reset_counter == 0) {
        /* OTHER with the counter expired: the stream has moved; re-anchor. */
        STAT(rq, other);
        STAT(rq, reset);
        rq->reset_counter = RAX_QUART_RESET_THRESHOLD;
        result = quartInsertFrom(rq, &rq->rax->head, 0, key, data, old, 1, 0, overwrite, &leaf);
        if (errno == ENOMEM) { raxQuartInvalidate(rq); return 0; }
        memcpy(rq->fp_key, key, keylen);
    } else {
        /* OTHER: an outlier; insert from the root and keep fp. */
        STAT(rq, other);
        rq->reset_counter--;
        result = quartInsertFrom(rq, &rq->rax->head, 0, key, data, old, 0, 1, overwrite, &leaf);
        if (errno == ENOMEM) { raxQuartInvalidate(rq); return 0; }
    }
    quartNoteLeaf(rq, key, leaf);
    return result;
}

int raxQuartInsert(raxQuart *rq, unsigned char *key, size_t keylen, void *data, void **old) {
    return quartInsert(rq, key, keylen, data, old, 1);
}

int raxQuartTryInsert(raxQuart *rq, unsigned char *key, size_t keylen, void *data, void **old) {
    return quartInsert(rq, key, keylen, data, old, 0);
}

int raxQuartFind(raxQuart *rq, unsigned char *key, size_t keylen, void **value) {
    if (!rq) return 0;
    return raxFind(rq->rax, key, keylen, value);
}

int raxQuartRemove(raxQuart *rq, unsigned char *key, size_t keylen, void **old) {
    if (!rq) return 0;
    int r = raxRemove(rq->rax, key, keylen, old);
    raxQuartInvalidate(rq);
    return r;
}

int raxQuartTail(raxQuart *rq, unsigned char *key, void **data) {
    if (!rq) return 0;
    quartSync(rq);
    if (!rq->has_tail) {
        raxIterator ri;
        raxStart(&ri, rq->rax);
        raxSeek(&ri, "$", NULL, 0);
        if (raxEOF(&ri) || (int)ri.key_len != rq->keylen) {
            raxStop(&ri);
            return 0;
        }
        memcpy(rq->tail_key, ri.key, rq->keylen);
        rq->tail_node = ri.node;
        rq->has_tail = 1;
        raxStop(&ri);
        STAT(rq, tail_seek);
    } else {
        STAT(rq, tail_hit);
    }
    memcpy(key, rq->tail_key, rq->keylen);
    *data = raxGetData(rq->tail_node);
    return 1;
}

void raxQuartSetTailData(raxQuart *rq, unsigned char *key, size_t keylen, void *data) {
    if (!rq) return;
    quartSync(rq);
    if (rq->has_tail && (int)keylen == rq->keylen && memcmp(key, rq->tail_key, keylen) == 0) {
        raxSetData(rq->tail_node, data);
        return;
    }
    raxQuartInsert(rq, key, keylen, data, NULL);
}

void raxQuartInvalidate(raxQuart *rq) {
    if (!rq) return;
    rq->has_tail = 0;
    rq->tail_node = NULL;
    rq->has_fp_key = 0;
    rq->fp_ref = &rq->rax->head;
    rq->fp_depth = 0;
    rq->reset_counter = RAX_QUART_RESET_THRESHOLD;
    rq->gen = rq->rax->gen;
}

static raxQuart *quartAlloc(int keylen, int group_depth) {
    if (keylen < 1) keylen = 1;
    if (keylen > RAX_QUART_MAX_KEYLEN) keylen = RAX_QUART_MAX_KEYLEN;
    if (group_depth < 0) group_depth = 0;
    if (group_depth > keylen - 1) group_depth = keylen - 1;
    raxQuart *rq = zcalloc(sizeof(raxQuart));
    if (!rq) return NULL;
    rq->keylen = keylen;
    rq->group_depth = group_depth;
    return rq;
}

raxQuart *raxQuartNewWithMetadata(int metaSize, size_t *alloc_size, int keylen, int group_depth) {
    raxQuart *rq = quartAlloc(keylen, group_depth);
    if (!rq) return NULL;
    rq->rax = raxNewWithMetadata(metaSize, alloc_size);
    if (!rq->rax) { zfree(rq); return NULL; }
    rq->owns_rax = 1;
    raxQuartInvalidate(rq);
    return rq;
}

raxQuart *raxQuartWrap(rax *rax, int keylen, int group_depth) {
    if (!rax) return NULL;
    raxQuart *rq = quartAlloc(keylen, group_depth);
    if (!rq) return NULL;
    rq->rax = rax;
    rq->owns_rax = 0;
    raxQuartInvalidate(rq);
    return rq;
}

void raxQuartUnwrap(raxQuart *rq) {
    if (!rq) return;
    zfree(rq);
}

raxQuart *raxQuartNew(void) {
    raxQuart *rq = quartAlloc(4, 3);
    if (!rq) return NULL;
    rq->rax = raxNew();
    if (!rq->rax) { zfree(rq); return NULL; }
    rq->owns_rax = 1;
    raxQuartInvalidate(rq);
    return rq;
}

void raxQuartFree(raxQuart *rq) {
    if (!rq) return;
    if (rq->rax && rq->owns_rax) raxFree(rq->rax);
    zfree(rq);
}

uint64_t raxQuartSize(raxQuart *rq) {
    return rq ? raxSize(rq->rax) : 0;
}

void raxQuartGlobalStats(raxQuartStats *out) {
    *out = g_stats;
}

void raxQuartGlobalStatsReset(void) {
    memset(&g_stats, 0, sizeof(g_stats));
}

void raxQuartSetResetThreshold(raxQuart *rq, int threshold) {
    if (rq && threshold > 0) rq->reset_counter = threshold;
}

int raxQuartGetResetThreshold(raxQuart *rq) {
    return rq ? rq->reset_counter : -1;
}
