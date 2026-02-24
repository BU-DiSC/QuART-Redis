/* Redis rax with QuART optimizations: bridge detection, reset counter, and bidirectional support
 *
 * Implementation of fast-path optimizations on top of standard rax.
 */

#include "server.h"
#include "rax_quart.h"
#include "rax.h"
#include "rax_malloc.h"
#include "zmalloc.h"
#include <string.h>
#include <errno.h>

/* Default reset threshold */
#define DEFAULT_RESET_THRESHOLD 300

/* Declare internal rax functions we need for fp-based insertion */
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

/* Fast-path insertion starting from fp_ref (QuART-style).
 * Inserts starting at rq->fp_ref/rq->fp_depth instead of from the root.
 * This is a specialized version of raxInsert() for fixed 4-byte keys.
 *
 * Returns 1 if inserted, 0 if key existed or on OOM (errno=ENOMEM on OOM).
 */
static int raxQuartFastPathInsert(raxQuart *rq, unsigned char *key_bytes, void *data) {
    if (!rq || !rq->rax || !rq->fp_ref || !*rq->fp_ref) {
        errno = 0;
        return 0;
    }

    rax *rax = rq->rax;
    raxNode *h = *rq->fp_ref;
    raxNode **parentlink = rq->fp_ref;
    size_t len = 4;
    size_t i = (rq->fp_depth < 0) ? 0 : (size_t)rq->fp_depth;
    size_t j = 0;
    size_t usable;
    size_t dummy, *alloc_size = &dummy;
    if (rax->alloc_size) alloc_size = rax->alloc_size;

    /* Walk from the cached node following the remaining bytes. */
    while (h->size && i < len) {
        unsigned char *v = h->data;

        if (h->iscompr) {
            for (j = 0; j < h->size && i < len; j++, i++) {
                if (v[j] != key_bytes[i]) break;
            }
            if (j != h->size) break;
        } else {
            for (j = 0; j < h->size; j++) {
                if (v[j] == key_bytes[i]) break;
            }
            if (j == h->size) break;
            i++;
        }

        raxNode **children = raxNodeFirstChildPtr(h);
        if (h->iscompr) j = 0;
        raxNode *next;
        memcpy(&next, children + j, sizeof(next));
        parentlink = children + j;
        h = next;
        j = 0;
    }

    /* Key exists / can be represented by current node. */
    if (i == len && (!h->iscompr || j == 0)) {
        if (!h->iskey) {
            raxNode *newh = raxReallocForData(rax, h, data);
            if (newh == NULL) {
                errno = ENOMEM;
                return 0;
            }
            h = newh;
            memcpy(parentlink, &h, sizeof(h));
            raxSetData(h, data);
            rax->numele++;
            errno = 0;
            return 1;
        }

        /* Existing key: overwrite to match raxInsert() semantics. */
        raxSetData(h, data);
        errno = 0;
        return 0;
    }

    /* Split compressed node on mismatch (ALGO 1 in raxGenericInsert). */
    if (h->iscompr && i != len) {
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

        raxFreeNode(rax, h);
        h = splitnode;
    } else if (h->iscompr && i == len) {
        /* Fixed-length 4-byte keys should never end inside a longer compressed node.
         * If it happens (fp_depth misuse), bail out safely. */
        errno = 0;
        return 0;
    }

    /* Insert the missing suffix starting from node h (like raxGenericInsert). */
    while (i < len) {
        raxNode *child;

        if (h->size == 0 && len - i > 1) {
            size_t comprsize = len - i;
            if (comprsize > RAX_NODE_MAX_SIZE) comprsize = RAX_NODE_MAX_SIZE;
            raxNode *newh = raxCompressNode(rax, h, key_bytes + i, comprsize, &child);
            if (newh == NULL) {
                errno = ENOMEM;
                return 0;
            }
            h = newh;
            memcpy(parentlink, &h, sizeof(h));
            parentlink = raxNodeLastChildPtr(h);
            i += comprsize;
        } else {
            raxNode **new_parentlink;
            raxNode *newh = raxAddChild(rax, h, key_bytes[i], &child, &new_parentlink);
            if (newh == NULL) {
                errno = ENOMEM;
                return 0;
            }
            h = newh;
            memcpy(parentlink, &h, sizeof(h));
            parentlink = new_parentlink;
            i++;
        }
        rax->numnodes++;
        h = child;
    }

    raxNode *newh = raxReallocForData(rax, h, data);
    if (newh == NULL) {
        errno = ENOMEM;
        return 0;
    }
    h = newh;
    if (!h->iskey) rax->numele++;
    raxSetData(h, data);
    memcpy(parentlink, &h, sizeof(h));
    errno = 0;
    return 1;
}

/* Convert 32-bit key to byte array (big-endian) */
static inline void keyToBytes(uint32_t key, unsigned char *bytes) {
    bytes[0] = (key >> 24) & 0xFF;
    bytes[1] = (key >> 16) & 0xFF;
    bytes[2] = (key >> 8) & 0xFF;
    bytes[3] = key & 0xFF;
}

/* Extract byte at position from 32-bit key */
static inline uint8_t getKeyByte(uint32_t key, int pos) {
    return (key >> (24 - pos * 8)) & 0xFF;
}

/* Update fp_ref/fp_depth to a good fp node for this key.
 *
 * QuART semantics: fp points to the deepest node on the fp path (ideally the
 * node that begins exactly at depth 3 for 4-byte keys, so the last byte can be
 * inserted as an edge from that node). If depth 3 lies inside a compressed
 * node, we keep fp at the deepest node start <= 3.
 */
static void updateFpRefToKey(raxQuart *rq, unsigned char *key_bytes) {
    if (!rq || !rq->rax || !rq->rax->head) return;
    
    raxNode *h = rq->rax->head;
    raxNode **parentlink = &rq->rax->head;

    int depth = 0;            /* How many key bytes consumed. */
    int node_depth = 0;       /* Depth where current node begins. */
    raxNode **best_link = parentlink;
    int best_depth = 0;

    /* Walk the tree following key_bytes. Track the deepest node start <= 3. */
    while (h && depth < 4) {
        if (node_depth <= 3 && node_depth >= best_depth) {
            best_link = parentlink;
            best_depth = node_depth;
        }

        if (node_depth == 3) break; /* Ideal fp node. */

        if (h->iscompr) {
            unsigned char *v = h->data;
            int match_len = 0;
            while (match_len < (int)h->size && depth < 4 && v[match_len] == key_bytes[depth]) {
                match_len++;
                depth++;
                if (depth == 3) {
                    /* Depth 3 is inside this compressed node: keep fp at its start. */
                    break;
                }
            }
            if (match_len < (int)h->size && depth < 3) break; /* Mismatch before fp point. */
            if (depth >= 3) break;

            raxNode **cp = raxNodeFirstChildPtr(h);
            if (!cp || !*cp) break;
            parentlink = cp;
            h = *cp;
            node_depth = depth;
        } else {
            unsigned char *v = h->data;
            int found = -1;
            for (int k = 0; k < (int)h->size; k++) {
                if (v[k] == key_bytes[depth]) {
                    found = k;
                    break;
                }
            }
            if (found < 0) break;
            depth++;
            raxNode **children = raxNodeFirstChildPtr(h);
            if (!children || !children[found]) break;
            parentlink = &children[found];
            h = children[found];
            node_depth = depth;
        }
    }

    rq->fp_ref = best_link;
    rq->fp_depth = best_depth;
}

raxQuart *raxQuartNew(void) {
    raxQuart *rq = zmalloc(sizeof(raxQuart));
    if (!rq) return NULL;
    
    rq->rax = raxNew();
    if (!rq->rax) {
        zfree(rq);
        return NULL;
    }
    
    rq->last_key = 0;
    rq->fp_ref = &rq->rax->head;  /* Initially points to root */
    rq->fp_depth = 0;              /* Start at depth 0 */
    rq->reset_counter = DEFAULT_RESET_THRESHOLD;
    rq->direction = 1;  /* Start with forward direction */
    rq->fp_inserts = 0;
    rq->regular_inserts = 0;
    rq->bridge_detected = 0;
    rq->resets = 0;
    rq->has_last_key = 0;
    
    return rq;
}

int raxQuartInsert(raxQuart *rq, uint32_t key, void *data) {
    if (!rq) return 0;
    
    unsigned char key_bytes[4];
    keyToBytes(key, key_bytes);
    
    /* First insertion - always insert normally and change fp (like QuART: insert_recursive_change_fp) */
    if (!rq->has_last_key) {
        int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
        if (result) {
            rq->last_key = key;
            rq->has_last_key = 1;
            /* Change fp to the newly inserted key */
            updateFpRefToKey(rq, key_bytes);
            rq->regular_inserts++;
        }
        return result;
    }
    
    /* QuART_stail_reset_bidir logic: byte-by-byte comparison with last_key */
    uint32_t leafValue = rq->last_key;
    
    if (rq->direction) {
        /* FORWARD DIRECTION: Check each byte (excluding last byte) */
        for (int i = 0; i < 3; i++) {
            uint8_t leafByte = getKeyByte(leafValue, i);
            uint8_t keyByte = getKeyByte(key, i);
            
            if (keyByte == leafByte) {
                /* Bytes match, continue to next byte */
                continue;
            }
            else if (keyByte < leafByte) {
                /* Key byte < leaf byte: preserve fp, decrement counter (not on fp path) */
                rq->reset_counter--;
                rq->regular_inserts++;
                int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
                /* Preserve fp: do not update rq->last_key, but refresh fp_ref in case of reallocations. */
                unsigned char leaf_bytes[4];
                keyToBytes(rq->last_key, leaf_bytes);
                updateFpRefToKey(rq, leaf_bytes);
                return result;
            }
            else {
                /* Key byte > leaf byte: check for bridge or reset */
                
                /* Check if this is a bridge value */
                int is_bridge = 0;
                if (i == 0) {
                    /* Bridge at byte 0: key[0]=leaf[0]+1, key[1]=0, key[2]=0, leaf[1]=255, leaf[2]=255 */
                    is_bridge = (keyByte == leafByte + 1) &&
                                (getKeyByte(key, 1) == 0) &&
                                (getKeyByte(key, 2) == 0) &&
                                (getKeyByte(leafValue, 1) == 255) &&
                                (getKeyByte(leafValue, 2) == 255);
                } else if (i == 1) {
                    /* Bridge at byte 1: key[0]=leaf[0], key[1]=leaf[1]+1, key[2]=0, leaf[2]=255 */
                    is_bridge = (keyByte == leafByte + 1) &&
                                (getKeyByte(key, 2) == 0) &&
                                (getKeyByte(key, 0) == getKeyByte(leafValue, 0)) &&
                                (getKeyByte(leafValue, 2) == 255);
                } else if (i == 2) {
                    /* Bridge at byte 2: key[0]=leaf[0], key[1]=leaf[1], key[2]=leaf[2]+1 */
                    is_bridge = (keyByte == leafByte + 1) &&
                                (getKeyByte(key, 0) == getKeyByte(leafValue, 0)) &&
                                (getKeyByte(key, 1) == getKeyByte(leafValue, 1));
                }
                
                if (is_bridge) {
                    /* Bridge detected: reset counter, reset fp to root, then change fp to new key */
                    rq->reset_counter = DEFAULT_RESET_THRESHOLD;
                    rq->bridge_detected++;
                    rq->regular_inserts++;
                    int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
                    if (errno != ENOMEM) {
                        /* Change fp to the bridge destination key (even if it already existed). */
                        updateFpRefToKey(rq, key_bytes);
                        rq->last_key = key;
                    }
                    return result;
                }
                /* Check if counter expired */
                else if (rq->reset_counter <= 0) {
                    /* Force fp change and reset */
                    rq->reset_counter = DEFAULT_RESET_THRESHOLD;
                    rq->resets++;
                    rq->fp_ref = &rq->rax->head;
                    rq->fp_depth = 0;
                    rq->direction = 1;  /* Reset to forward */
                    rq->regular_inserts++;
                    int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
                    if (errno != ENOMEM) {
                        updateFpRefToKey(rq, key_bytes);
                        rq->last_key = key;
                    }
                    return result;
                }
                else {
                    /* Not a bridge and counter not expired: preserve fp, decrement counter */
                    rq->reset_counter--;
                    rq->regular_inserts++;
                    int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
                    unsigned char leaf_bytes[4];
                    keyToBytes(rq->last_key, leaf_bytes);
                    updateFpRefToKey(rq, leaf_bytes);
                    return result;
                }
            }
        }
        
        /* All first 3 bytes match - check last byte for direction change */
        uint8_t lastLeafByte = leafValue & 0xFF;
        uint8_t lastKeyByte = key & 0xFF;
        if (lastKeyByte < lastLeafByte) {
            /* Direction change: switch to backward */
            rq->direction = 0;
            /* Update last_key's last byte for backward tracking */
            rq->last_key = (leafValue & 0xFFFFFF00) | lastKeyByte;
        }
    }
    else {
        /* BACKWARD DIRECTION: Check each byte (excluding last byte) */
        for (int i = 0; i < 3; i++) {
            uint8_t leafByte = getKeyByte(leafValue, i);
            uint8_t keyByte = getKeyByte(key, i);
            
            if (keyByte == leafByte) {
                /* Bytes match, continue to next byte */
                continue;
            }
            else if (keyByte > leafByte) {
                /* Key byte > leaf byte: preserve fp, decrement counter (going wrong direction) */
                rq->reset_counter--;
                rq->regular_inserts++;
                int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
                unsigned char leaf_bytes[4];
                keyToBytes(rq->last_key, leaf_bytes);
                updateFpRefToKey(rq, leaf_bytes);
                return result;
            }
            else {
                /* Key byte < leaf byte: check for backward bridge or reset */
                
                /* Check if this is a backward bridge value */
                int is_bridge = 0;
                if (i == 0) {
                    /* Backward bridge at byte 0: leaf[0]=key[0]+1, leaf[1]=0, leaf[2]=0, key[1]=255, key[2]=255 */
                    is_bridge = (leafByte == keyByte + 1) &&
                                (getKeyByte(leafValue, 1) == 0) &&
                                (getKeyByte(leafValue, 2) == 0) &&
                                (getKeyByte(key, 1) == 255) &&
                                (getKeyByte(key, 2) == 255);
                } else if (i == 1) {
                    /* Backward bridge at byte 1: key[0]=leaf[0], leaf[1]=key[1]+1, leaf[2]=0, key[2]=255 */
                    is_bridge = (leafByte == keyByte + 1) &&
                                (getKeyByte(leafValue, 2) == 0) &&
                                (getKeyByte(key, 0) == getKeyByte(leafValue, 0)) &&
                                (getKeyByte(key, 2) == 255);
                } else if (i == 2) {
                    /* Backward bridge at byte 2: key[0]=leaf[0], key[1]=leaf[1], leaf[2]=key[2]+1 */
                    is_bridge = (leafByte == keyByte + 1) &&
                                (getKeyByte(key, 0) == getKeyByte(leafValue, 0)) &&
                                (getKeyByte(key, 1) == getKeyByte(leafValue, 1));
                }
                
                if (is_bridge) {
                    /* Bridge detected: reset counter, reset fp, change fp */
                    rq->reset_counter = DEFAULT_RESET_THRESHOLD;
                    rq->bridge_detected++;
                    rq->fp_ref = &rq->rax->head;
                    rq->fp_depth = 0;
                    rq->regular_inserts++;
                    int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
                    if (errno != ENOMEM) {
                        updateFpRefToKey(rq, key_bytes);
                        rq->last_key = key;
                    }
                    return result;
                }
                /* Check if counter expired */
                else if (rq->reset_counter <= 0) {
                    /* Force fp change and reset */
                    rq->reset_counter = DEFAULT_RESET_THRESHOLD;
                    rq->resets++;
                    rq->fp_ref = &rq->rax->head;
                    rq->fp_depth = 0;
                    rq->direction = 1;  /* Reset to forward */
                    rq->regular_inserts++;
                    int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
                    if (errno != ENOMEM) {
                        updateFpRefToKey(rq, key_bytes);
                        rq->last_key = key;
                    }
                    return result;
                }
                else {
                    /* Not a bridge and counter not expired: preserve fp, decrement counter */
                    rq->reset_counter--;
                    rq->regular_inserts++;
                    int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
                    unsigned char leaf_bytes[4];
                    keyToBytes(rq->last_key, leaf_bytes);
                    updateFpRefToKey(rq, leaf_bytes);
                    return result;
                }
            }
        }
        
        /* All first 3 bytes match - check last byte for direction change */
        uint8_t lastLeafByte = leafValue & 0xFF;
        uint8_t lastKeyByte = key & 0xFF;
        if (lastKeyByte > lastLeafByte) {
            /* Direction change: switch to forward */
            rq->direction = 1;
            /* Update last_key's last byte for forward tracking */
            rq->last_key = (leafValue & 0xFFFFFF00) | lastKeyByte;
        }
    }
    
    /* If we reach here, it means fp insert will happen (all bytes sequential) */
    /* This is the fast-path: reset counter and count as fp_insert */
    rq->reset_counter = DEFAULT_RESET_THRESHOLD;
    rq->fp_inserts++;

    /* Real fast-path insertion from the fp node (no detection-only fallback). */
    int result = raxQuartFastPathInsert(rq, key_bytes, data);
    if (errno == ENOMEM) return 0;

    /* Fast-path updates fp_leaf to the new key (even if it already existed). */
    rq->last_key = key;
    updateFpRefToKey(rq, key_bytes);
    return result;
}

int raxQuartFind(raxQuart *rq, uint32_t key, void **value) {
    if (!rq) return 0;
    
    unsigned char key_bytes[4];
    keyToBytes(key, key_bytes);
    
    return raxFind(rq->rax, key_bytes, 4, value);
}

int raxQuartRemove(raxQuart *rq, uint32_t key, void **old) {
    if (!rq) return 0;
    
    unsigned char key_bytes[4];
    keyToBytes(key, key_bytes);
    
    return raxRemove(rq->rax, key_bytes, 4, old);
}

void raxQuartFree(raxQuart *rq) {
    if (!rq) return;
    
    if (rq->rax) raxFree(rq->rax);
    zfree(rq);
}

uint64_t raxQuartSize(raxQuart *rq) {
    return rq ? raxSize(rq->rax) : 0;
}

void raxQuartSetResetThreshold(raxQuart *rq, int threshold) {
    if (rq && threshold > 0) {
        rq->reset_counter = threshold;
    }
}

int raxQuartGetResetThreshold(raxQuart *rq) {
    return rq ? rq->reset_counter : -1;
}

void raxQuartSetDirection(raxQuart *rq, int direction) {
    if (rq) {
        rq->direction = direction ? 1 : 0;
    }
}

int raxQuartGetDirection(raxQuart *rq) {
    return rq ? rq->direction : -1;
}

void raxQuartGetStats(raxQuart *rq, 
                      uint64_t *fp_inserts,
                      uint64_t *regular_inserts, 
                      uint64_t *bridges,
                      uint64_t *resets) {
    if (!rq) return;
    
    if (fp_inserts) *fp_inserts = rq->fp_inserts;
    if (regular_inserts) *regular_inserts = rq->regular_inserts;
    if (bridges) *bridges = rq->bridge_detected;
    if (resets) *resets = rq->resets;
}
