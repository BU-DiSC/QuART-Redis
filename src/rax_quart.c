/* Redis rax with QuART optimizations: bridge detection, reset counter, and bidirectional support
 *
 * Implementation of fast-path optimizations on top of standard rax.
 */

#include "server.h"
#include "rax_quart.h"
#include "rax.h"
#include "zmalloc.h"
#include <string.h>
#include <errno.h>

/* Default reset threshold */
#define DEFAULT_RESET_THRESHOLD 300

/* Declare internal rax functions we need for fp-based insertion */
extern raxNode *raxAddChild(rax *rax, raxNode *n, unsigned char c, raxNode **childptr, raxNode ***parentlink);
extern raxNode *raxCompressNode(rax *rax, raxNode *n, unsigned char *s, size_t len, raxNode **child);
extern raxNode *raxNewNode(rax *rax, size_t children, int datafield);
extern raxNode *raxReallocForData(rax *rax, raxNode *n, void *data);
extern size_t raxLowWalk(rax *rax, unsigned char *s, size_t len, raxNode **stopnode, raxNode ***plink, int *splitpos, void *ts);

/* Helper to get padding for a node */
static inline size_t raxNodePadding(size_t size) {
    return sizeof(void*) - (size+sizeof(raxNode)) % sizeof(void*);
}

/* Helper to get first child pointer */
static inline raxNode **raxNodeFirstChildPtr(raxNode *n) {
    return (raxNode**)((char*)n + sizeof(raxNode) + n->size + raxNodePadding(n->size));
}

/* Helper to get last child pointer */
static inline raxNode **raxNodeLastChildPtr(raxNode *n) {
    raxNode **cp = raxNodeFirstChildPtr(n);
    if (n->iscompr) return cp;
    return cp + (n->size - 1);
}

/* Helper to allocate a new node */
static inline raxNode *raxQuartNewNode(rax *rax, size_t children, int datafield) {
    size_t nodesize = sizeof(raxNode) + children + raxNodePadding(children) +
                      sizeof(raxNode*) * children;
    if (datafield) nodesize += sizeof(void*);
    raxNode *node = zmalloc(nodesize);
    if (node == NULL) return NULL;
    node->iskey = 0;
    node->isnull = 0;
    node->iscompr = 0;
    node->size = children;
    if (rax->alloc_size) *rax->alloc_size += nodesize;
    return node;
}

/* Helper to set data in a node */
static inline void raxQuartSetData(raxNode *n, void *data) {
    n->iskey = 1;
    n->isnull = 0;
    void **ndata = (void**)
        ((char*)n + sizeof(*n) + n->size + raxNodePadding(n->size) +
         sizeof(raxNode*) * (n->iscompr ? 1 : n->size));
    memcpy(ndata, &data, sizeof(data));
}

/* Fast-path insertion starting from fp_ref
 * Like QuART's stail insert: if at depth 3, directly insert leaf into fp node
 * Returns 1 on success, 0 on failure (fall back to regular insert) */
static int raxQuartFastPathInsert(raxQuart *rq, unsigned char *key_bytes, void *data) {
    raxNode *h = *rq->fp_ref;
    raxNode **parentlink = rq->fp_ref;
    int depth = rq->fp_depth;
    rax *rax = rq->rax;
    
    /* Walk from fp_ref to find insertion point, similar to raxLowWalk but starting from fp */
    while (h && depth < 4) {
        unsigned char target = key_bytes[depth];
        
        if (h->iscompr) {
            /* Compressed node - check if we match the prefix */
            int match_len = 0;
            unsigned char *v = h->data;
            
            while (match_len < h->size && depth + match_len < 4) {
                if (v[match_len] != key_bytes[depth + match_len]) {
                    /* Mismatch in compressed node - need split, fall back */
                    return 0;
                }
                match_len++;
            }
            
            depth += match_len;
            
            if (match_len < h->size) {
                /* Stopped in middle of compressed node - fall back */
                return 0;
            }
            
            /* Move to child of compressed node */
            raxNode **children = raxNodeFirstChildPtr(h);
            parentlink = children;
            h = *children;
            rq->fp_ref = children;
            rq->fp_depth = depth;
        } else {
            /* Non-compressed node */
            
            /* Special case: if we're at depth 3 and this is a non-compressed node,
             * we can directly insert the final byte as a new leaf child.
             * This mirrors QuART's direct insertion at fp_depth == maxPrefixLength - 1 */
            if (depth == 3) {
                /* Check if the child already exists */
                unsigned char *v = h->data;
                for (int j = 0; j < h->size; j++) {
                    if (v[j] == target) {
                        /* Child exists, check if it needs data update */
                        raxNode **children = raxNodeFirstChildPtr(h);
                        raxNode *existing = children[j];
                        if (existing->iskey) {
                            /* Key already exists - fall back */
                            return 0;
                        }
                        /* Mark as key and set data */
                        existing = raxReallocForData(rax, existing, data);
                        if (existing == NULL) return 0;
                        raxSetData(existing, data);
                        memcpy(&children[j], &existing, sizeof(existing));
                        rax->numele++;
                        
                        /* Update fp to point to this child */
                        rq->fp_ref = &children[j];
                        rq->fp_depth = 4;
                        return 1; /* SUCCESS! */
                    }
                }
                
                /* Child doesn't exist - add it using raxAddChild */
                raxNode *child;
                raxNode **new_parentlink;
                raxNode *newh = raxAddChild(rax, h, target, &child, &new_parentlink);
                if (newh == NULL) return 0;
                
                /* Update parent to point to the reallocated node */
                memcpy(parentlink, &newh, sizeof(newh));
                h = newh;
                
                /* Now child is the new leaf node - set its data */
                child = raxReallocForData(rax, child, data);
                if (child == NULL) return 0;
                raxSetData(child, data);
                memcpy(new_parentlink, &child, sizeof(child));
                
                rax->numnodes++;
                rax->numele++;
                
                /* Update fp to point to the new child */
                rq->fp_ref = new_parentlink;
                rq->fp_depth = 4;
                
                return 1; /* SUCCESS! */
            }
            
            /* Find matching child for navigation (depth < 3) */
            unsigned char *v = h->data;
            int found = -1;
            for (int j = 0; j < h->size; j++) {
                if (v[j] == target) {
                    found = j;
                    break;
                }
            }
            
            if (found >= 0) {
                /* Found matching child - navigate to it */
                depth++;
                raxNode **children = raxNodeFirstChildPtr(h);
                parentlink = &children[found];
                rq->fp_ref = parentlink;
                rq->fp_depth = depth;
                h = children[found];
            } else {
                /* No matching child and depth < 3 - need to build path, fall back */
                return 0;
            }
        }
    }
    
    /* If we're here, we've navigated the full path but couldn't insert directly
     * Fall back to regular insert */
    return 0;
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

/* Check if key is a forward bridge at byte position i */
static inline int isForwardBridge(uint32_t key, uint32_t last_key, int i) {
    uint8_t key_byte = getKeyByte(key, i);
    uint8_t last_byte = getKeyByte(last_key, i);
    
    if (i == 0) {
        /* Bridge at byte 0: key[0] = last[0]+1, key[1]=0, key[2]=0, last[1]=255, last[2]=255 */
        return (key_byte == last_byte + 1) &&
               (getKeyByte(key, 1) == 0) &&
               (getKeyByte(key, 2) == 0) &&
               (getKeyByte(last_key, 1) == 255) &&
               (getKeyByte(last_key, 2) == 255);
    } else if (i == 1) {
        /* Bridge at byte 1: key[0]=last[0], key[1]=last[1]+1, key[2]=0, last[2]=255 */
        return (getKeyByte(key, 0) == getKeyByte(last_key, 0)) &&
               (key_byte == last_byte + 1) &&
               (getKeyByte(key, 2) == 0) &&
               (getKeyByte(last_key, 2) == 255);
    } else if (i == 2) {
        /* Bridge at byte 2: key[0]=last[0], key[1]=last[1], key[2]=last[2]+1 */
        return (getKeyByte(key, 0) == getKeyByte(last_key, 0)) &&
               (getKeyByte(key, 1) == getKeyByte(last_key, 1)) &&
               (key_byte == last_byte + 1);
    }
    return 0;
}

/* Check if key is a backward bridge at byte position i */
static inline int isBackwardBridge(uint32_t key, uint32_t last_key, int i) {
    uint8_t key_byte = getKeyByte(key, i);
    uint8_t last_byte = getKeyByte(last_key, i);
    
    if (i == 0) {
        /* Bridge at byte 0: last[0] = key[0]+1, last[1]=0, last[2]=0, key[1]=255, key[2]=255 */
        return (last_byte == key_byte + 1) &&
               (getKeyByte(last_key, 1) == 0) &&
               (getKeyByte(last_key, 2) == 0) &&
               (getKeyByte(key, 1) == 255) &&
               (getKeyByte(key, 2) == 255);
    } else if (i == 1) {
        /* Bridge at byte 1: key[0]=last[0], last[1]=key[1]+1, last[2]=0, key[2]=255 */
        return (getKeyByte(key, 0) == getKeyByte(last_key, 0)) &&
               (last_byte == key_byte + 1) &&
               (getKeyByte(last_key, 2) == 0) &&
               (getKeyByte(key, 2) == 255);
    } else if (i == 2) {
        /* Bridge at byte 2: key[0]=last[0], key[1]=last[1], last[2]=key[2]+1 */
        return (getKeyByte(key, 0) == getKeyByte(last_key, 0)) &&
               (getKeyByte(key, 1) == getKeyByte(last_key, 1)) &&
               (last_byte == key_byte + 1);
    }
    return 0;
}

/* Check if this is a sequential fast-path insertion */
static int isFastPathInsert(raxQuart *rq, uint32_t key) {
    if (!rq->has_last_key) return 0;
    
    uint32_t last = rq->last_key;
    
    /* Check direction */
    if (rq->direction) {
        /* Forward direction: check for sequential or bridge */
        if (key == last + 1) return 1;  /* Perfect sequential */
        
        /* Check for bridge at each byte level */
        for (int i = 0; i < 3; i++) {
            /* First check if bytes match up to position i */
            int bytes_match = 1;
            for (int j = 0; j < i; j++) {
                if (getKeyByte(key, j) != getKeyByte(last, j)) {
                    bytes_match = 0;
                    break;
                }
            }
            if (!bytes_match) continue;
            
            /* Check if this is a bridge */
            if (isForwardBridge(key, last, i)) {
                rq->bridge_detected++;
                return 1;
            }
        }
    } else {
        /* Backward direction: check for descending sequential or bridge */
        if (key == last - 1) return 1;  /* Perfect sequential */
        
        /* Check for backward bridge at each byte level */
        for (int i = 0; i < 3; i++) {
            int bytes_match = 1;
            for (int j = 0; j < i; j++) {
                if (getKeyByte(key, j) != getKeyByte(last, j)) {
                    bytes_match = 0;
                    break;
                }
            }
            if (!bytes_match) continue;
            
            if (isBackwardBridge(key, last, i)) {
                rq->bridge_detected++;
                return 1;
            }
        }
    }
    
    return 0;
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
    
    /* First insertion - always insert normally and set up fp */
    if (!rq->has_last_key) {
        int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
        if (result) {
            rq->last_key = key;
            rq->has_last_key = 1;
            rq->fp_ref = &rq->rax->head;
            rq->fp_depth = 0;
            rq->regular_inserts++;
        }
        return result;
    }
    
    /* Check if this is a fast-path insertion */
    int is_fp = isFastPathInsert(rq, key);
    
    if (is_fp) {
        /* Fast path: sequential or bridge insertion */
        rq->fp_inserts++;
        rq->reset_counter = DEFAULT_RESET_THRESHOLD;  /* Reset counter on fast-path */
        
        /* Try to insert starting from fp_ref */
        int fp_success = raxQuartFastPathInsert(rq, key_bytes, data);
        
        if (fp_success) {
            /* Fast-path insertion succeeded */
            rq->last_key = key;
            return 1;
        }
        
        /* Fast-path insertion failed or needs regular insert to complete
         * Do regular insert but we've already updated fp_ref to a better position */
        int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
        rq->last_key = key;
        return result;
    } else {
        /* Not a fast-path insertion */
        rq->regular_inserts++;
        rq->reset_counter--;
        
        /* Check if we need to reset the fast-path */
        if (rq->reset_counter <= 0) {
            rq->reset_counter = DEFAULT_RESET_THRESHOLD;
            rq->resets++;
            /* Reset direction to forward and fp to root */
            rq->direction = 1;
            rq->fp_ref = &rq->rax->head;
            rq->fp_depth = 0;
        }
        
        /* Update direction based on key comparison */
        if (rq->has_last_key) {
            /* Check last byte to determine direction */
            uint8_t last_byte3 = rq->last_key & 0xFF;
            uint8_t key_byte3 = key & 0xFF;
            
            /* Check if higher bytes match */
            if ((rq->last_key >> 8) == (key >> 8)) {
                if (key_byte3 < last_byte3) {
                    rq->direction = 0;  /* Switch to backward */
                } else if (key_byte3 > last_byte3) {
                    rq->direction = 1;  /* Switch to forward */
                }
            }
        }
        
        /* Perform regular insertion */
        int result = raxInsert(rq->rax, key_bytes, 4, data, NULL);
        rq->last_key = key;
        return result;
    }
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
