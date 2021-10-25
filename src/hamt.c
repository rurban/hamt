#include "hamt.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

/* Pointer tagging */
#define HAMT_TAG_MASK 0x3 /* last two bits */
#define HAMT_TAG_VALUE 0x1
#define tagged(__p) (HamtNode *)((uintptr_t)__p | HAMT_TAG_VALUE)
#define untagged(__p) (HamtNode *)((uintptr_t)__p & ~HAMT_TAG_MASK)
#define is_value(__p) (((uintptr_t)__p & HAMT_TAG_MASK) == HAMT_TAG_VALUE)

/* Bit fiddling */
#define index_clear_bit(_index, _n) _index & ~(1 << _n)
#define index_set_bit(_index, _n) _index | (1 << _n)

/* Node data structure */
typedef struct HamtNode {
    union {
        struct {
            void *value; /* tagged pointer */
            void *key;
        } kv;
        struct {
            struct HamtNode *ptr;
            uint32_t index;
        } table;
    } as;
} HamtNode;

/* Opaque user-facing implementation */
struct HamtImpl {
    struct HamtNode *root;
    size_t size;
    HamtKeyHashFn key_hash;
    HamtCmpFn key_cmp;
    struct HamtAllocator *ator;
};

/* Hashing w/ state management */
typedef struct Hash {
    const void *key;
    HamtKeyHashFn hash_fn;
    uint32_t hash;
    size_t depth;
    size_t shift;
} Hash;

/* Search results */
typedef enum {
    SEARCH_SUCCESS,
    SEARCH_FAIL_NOTFOUND,
    SEARCH_FAIL_KEYMISMATCH
} SearchStatus;

typedef struct SearchResult {
    SearchStatus status;
    HamtNode *anchor;
    HamtNode *value;
    Hash hash;
} SearchResult;

/* Removal results */
typedef enum { REMOVE_SUCCESS, REMOVE_GATHERED, REMOVE_NOTFOUND } RemoveStatus;

typedef struct RemoveResult {
    RemoveStatus status;
    void *value;
} RemoveResult;

static inline Hash hash_step(const Hash h)
{
    Hash hash = {.key = h.key,
                 .hash_fn = h.hash_fn,
                 .hash = h.hash,
                 .depth = h.depth + 1,
                 .shift = h.shift + 5};
    if (hash.shift > 30) {
        hash.hash = hash.hash_fn(hash.key, hash.depth / 5);
        hash.shift = 0;
    }
    return hash;
}

static inline uint32_t hash_get_index(const Hash *h)
{
    return (h->hash >> h->shift) & 0x1f;
}

static int get_popcount(uint32_t n) { return __builtin_popcount(n); }

static int get_pos(uint32_t sparse_index, uint32_t bitmap)
{
    return get_popcount(bitmap & ((1 << sparse_index) - 1));
}

static inline bool has_index(const HamtNode *anchor, size_t index)
{
    assert(anchor && "anchor must not be NULL");
    assert(index < 32 && "index must not be larger than 31");
    return anchor->as.table.index & (1 << index);
}

HAMT hamt_create(HamtKeyHashFn key_hash, HamtCmpFn key_cmp,
                 struct HamtAllocator *ator)
{
    struct HamtImpl *trie = hamt_alloc(ator, sizeof(struct HamtImpl));
    trie->root = hamt_alloc(ator, sizeof(HamtNode));
    memset(trie->root, 0, sizeof(HamtNode));
    trie->size = 0;
    trie->key_hash = key_hash;
    trie->key_cmp = key_cmp;
    trie->ator = ator;
    return trie;
}

static SearchResult search(HamtNode *anchor, Hash hash, HamtCmpFn cmp_eq,
                           const void *key)
{
    assert(!is_value(anchor->as.kv.value) &&
           "Invariant: search requires an internal node");
    /* determine the expected index in table */
    uint32_t expected_index = hash_get_index(&hash);
    /* check if the expected index is set */
    if (has_index(anchor, expected_index)) {
        /* if yes, get the compact index to address the array */
        int pos = get_pos(expected_index, anchor->as.table.index);
        /* index into the table and check what type of entry we're looking at */
        HamtNode *next = &anchor->as.table.ptr[pos];
        if (is_value(next->as.kv.value)) {
            if ((*cmp_eq)(key, next->as.kv.key) == 0) {
                /* keys match */
                SearchResult result = {.status = SEARCH_SUCCESS,
                                       .anchor = anchor,
                                       .value = next,
                                       .hash = hash};
                return result;
            }
            /* not found: same hash but different key */
            SearchResult result = {.status = SEARCH_FAIL_KEYMISMATCH,
                                   .anchor = anchor,
                                   .value = next,
                                   .hash = hash};
            return result;
        } else {
            /* For table entries, recurse to the next level */
            assert(next->as.table.ptr != NULL &&
                   "invariant: table ptrs must not be NULL");
            return search(next, hash_step(hash), cmp_eq, key);
        }
    }
    /* expected index is not set, terminate search */
    SearchResult result = {.status = SEARCH_FAIL_NOTFOUND,
                           .anchor = anchor,
                           .value = NULL,
                           .hash = hash};
    return result;
}

HamtNode *table_allocate(struct HamtAllocator *ator, size_t size)
{
    return (HamtNode *)hamt_alloc(ator, size * sizeof(HamtNode));
}

void table_free(struct HamtAllocator *ator, HamtNode *ptr, size_t size)
{
    hamt_free(ator, ptr);
}

HamtNode *table_extend(struct HamtAllocator *ator, HamtNode *anchor,
                       size_t n_rows, uint32_t index, uint32_t pos)
{
    HamtNode *new_table = table_allocate(ator, n_rows + 1);
    if (!new_table)
        return NULL;
    if (n_rows > 0) {
        /* copy over table */
        memcpy(&new_table[0], &anchor->as.table.ptr[0], pos * sizeof(HamtNode));
        /* note: this works since (n_rows - pos) == 0 for cases
         * where we're adding the new k/v pair at the end (i.e. memcpy(a, b, 0)
         * is a nop) */
        memcpy(&new_table[pos + 1], &anchor->as.table.ptr[pos],
               (n_rows - pos) * sizeof(HamtNode));
    }
    assert(!is_value(anchor->as.kv.value) && "URGS");
    table_free(ator, anchor->as.table.ptr, n_rows);
    anchor->as.table.ptr = new_table;
    anchor->as.table.index |= (1 << index);
    return anchor;
}

HamtNode *table_shrink(struct HamtAllocator *ator, HamtNode *anchor,
                       size_t n_rows, uint32_t index, uint32_t pos)
{
    /* debug assertions */
    assert(anchor && "Anchor cannot be NULL");
    assert(!is_value(anchor->as.kv.value) &&
           "Invariant: shrinking a table requires an internal node");

    HamtNode *new_table = NULL;
    uint32_t new_index = 0;
    if (n_rows > 0) {
        new_table = table_allocate(ator, n_rows - 1);
        if (!new_table)
            return NULL;
        new_index = anchor->as.table.index & ~(1 << index);
        memcpy(&new_table[0], &anchor->as.table.ptr[0], pos * sizeof(HamtNode));
        memcpy(&new_table[pos], &anchor->as.table.ptr[pos + 1],
               (n_rows - pos - 1) * sizeof(HamtNode));
    }
    table_free(ator, anchor->as.table.ptr, n_rows);
    anchor->as.table.index = new_index;
    anchor->as.table.ptr = new_table;
    return anchor;
}

HamtNode *table_gather(struct HamtAllocator *ator, HamtNode *anchor,
                       uint32_t pos)
{
    /* debug assertions */
    assert(anchor && "Anchor cannot be NULL");
    assert(!is_value(anchor->as.kv.value) &&
           "Invariant: gathering a table requires an internal anchor");
    assert((get_popcount(anchor->as.table.index) == 2) &&
           "Table must have size 2 to gather");
    assert((pos == 0 || pos == 1) && "pos must be 0 or 1");

    HamtNode *table = anchor->as.table.ptr;
    anchor->as.kv.key = table[pos].as.kv.key;
    anchor->as.kv.value = table[pos].as.kv.value; /* already tagged */
    table_free(ator, table, 2);
    return anchor;
}

static const HamtNode *insert_kv(HAMT h, HamtNode *anchor, Hash hash, void *key,
                                 void *value)
{
    /* calculate position in new table */
    uint32_t ix = hash_get_index(&hash);
    uint32_t new_index = anchor->as.table.index | (1 << ix);
    int pos = get_pos(ix, new_index);
    /* extend table */
    size_t n_rows = get_popcount(anchor->as.table.index);
    anchor = table_extend(h->ator, anchor, n_rows, ix, pos);
    if (!anchor)
        return NULL;
    HamtNode *new_table = anchor->as.table.ptr;
    /* set new k/v pair */
    new_table[pos].as.kv.key = key;
    new_table[pos].as.kv.value = tagged(value);
    /* return a pointer to the inserted k/v pair */
    return &new_table[pos];
}

static const HamtNode *insert_table(HAMT h, HamtNode *anchor, Hash hash,
                                    void *key, void *value)
{
    /* FIXME: check for alloc failure and bail out correctly (deleting the
     *        incomplete subtree */

    /* Collect everything we know about the existing value */
    Hash x_hash = {.key = anchor->as.kv.key,
                   .hash_fn = hash.hash_fn,
                   .hash = hash.hash_fn(anchor->as.kv.key, hash.depth / 5),
                   .depth = hash.depth,
                   .shift = hash.shift};
    void *x_value = anchor->as.kv.value; /* tagged (!) value ptr */
    /* increase depth until the hashes diverge, building a list
     * of tables along the way */
    Hash next_hash = hash_step(hash);
    Hash x_next_hash = hash_step(x_hash);
    uint32_t next_index = hash_get_index(&next_hash);
    uint32_t x_next_index = hash_get_index(&x_next_hash);
    while (x_next_index == next_index) {
        anchor->as.table.ptr = table_allocate(h->ator, 1);
        anchor->as.table.index = (1 << next_index);
        next_hash = hash_step(next_hash);
        x_next_hash = hash_step(x_next_hash);
        next_index = hash_get_index(&next_hash);
        x_next_index = hash_get_index(&x_next_hash);
        anchor = anchor->as.table.ptr;
    }
    /* the hashes are different, let's allocate a table with two
     * entries to store the existing and new values */
    anchor->as.table.ptr = table_allocate(h->ator, 2);
    anchor->as.table.index = (1 << next_index) | (1 << x_next_index);
    /* determine the proper position in the allocated table */
    int x_pos = get_pos(x_next_index, anchor->as.table.index);
    int pos = get_pos(next_index, anchor->as.table.index);
    /* fill in the existing value; no need to tag the value pointer
     * since it is already tagged. */
    anchor->as.table.ptr[x_pos].as.kv.key = (void *)x_hash.key;
    anchor->as.table.ptr[x_pos].as.kv.value = x_value;
    /* fill in the new key/value pair, tagging the pointer to the
     * new value to mark it as a value ptr */
    anchor->as.table.ptr[pos].as.kv.key = key;
    anchor->as.table.ptr[pos].as.kv.value = tagged(value);

    return &anchor->as.table.ptr[pos];
}

static const HamtNode *set(HAMT h, HamtNode *anchor, HamtKeyHashFn hash_fn,
                           HamtCmpFn cmp_fn, void *key, void *value)
{
    Hash hash = {.key = key,
                 .hash_fn = hash_fn,
                 .hash = hash_fn(key, 0),
                 .depth = 0,
                 .shift = 0};
    SearchResult sr = search(anchor, hash, cmp_fn, key);
    const HamtNode *inserted;
    switch (sr.status) {
    case SEARCH_SUCCESS:
        sr.value->as.kv.value = tagged(value);
        inserted = sr.value;
        break;
    case SEARCH_FAIL_NOTFOUND:
        if ((inserted = insert_kv(h, sr.anchor, sr.hash, key, value)) != NULL) {
            h->size += 1;
        }
        break;
    case SEARCH_FAIL_KEYMISMATCH:
        if ((inserted = insert_table(h, sr.value, sr.hash, key, value)) !=
            NULL) {
            h->size += 1;
        }
        break;
    }
    return inserted;
}

const void *hamt_get(const HAMT trie, void *key)
{
    Hash hash = {.key = key,
                 .hash_fn = trie->key_hash,
                 .hash = trie->key_hash(key, 0),
                 .depth = 0,
                 .shift = 0};
    SearchResult sr = search(trie->root, hash, trie->key_cmp, key);
    if (sr.status == SEARCH_SUCCESS) {
        return untagged(sr.value->as.kv.value);
    }
    return NULL;
}

const void *hamt_set(HAMT trie, void *key, void *value)
{
    const HamtNode *n =
        set(trie, trie->root, trie->key_hash, trie->key_cmp, key, value);
    return n->as.kv.value;
}

static RemoveResult rem(HAMT h, HamtNode *root, HamtNode *anchor, Hash hash,
                        HamtCmpFn cmp_eq, const void *key)
{
    assert(!is_value(anchor->as.kv.value) &&
           "Invariant: removal requires an internal node");
    /* determine the expected index in table */
    uint32_t expected_index = hash_get_index(&hash);
    /* check if the expected index is set */
    if (has_index(anchor, expected_index)) {
        /* if yes, get the compact index to address the array */
        int pos = get_pos(expected_index, anchor->as.table.index);
        /* index into the table and check what type of entry we're looking at */
        HamtNode *next = &anchor->as.table.ptr[pos];
        if (is_value(next->as.kv.value)) {
            if ((*cmp_eq)(key, next->as.kv.key) == 0) {
                uint32_t n_rows = get_popcount(anchor->as.table.index);
                void *value = next->as.kv.value;
                /* We shrink tables while they have more than 2 rows and switch
                 * to gathering the subtrie otherwise. The exception is when we
                 * are at the root, where we must shrink the table to one or
                 * zero.
                 */
                if (n_rows > 2 || (n_rows >= 1 && root->as.table.ptr == next)) {
                    anchor = table_shrink(h->ator, anchor, n_rows,
                                          expected_index, pos);
                } else if (n_rows == 2) {
                    /* gather, dropping the current row */
                    anchor = table_gather(h->ator, anchor, !pos);
                    return (RemoveResult){.status = REMOVE_GATHERED,
                                          .value = value};
                }
                return (RemoveResult){.status = REMOVE_SUCCESS, .value = value};
            }
            /* not found: same hash but different key */
            return (RemoveResult){.status = REMOVE_NOTFOUND, .value = NULL};
        } else {
            /* For table entries, recurse to the next level */
            assert(next->as.table.ptr != NULL &&
                   "invariant: table ptrs must not be NULL");
            RemoveResult result =
                rem(h, root, next, hash_step(hash), cmp_eq, key);
            if (next != root->as.table.ptr &&
                result.status == REMOVE_GATHERED) {
                /* remove dangling internal nodes: check if we need to
                 * propagate the gathering of the key-value entry */
                int n_rows = get_popcount(anchor->as.table.index);
                if (n_rows == 1) {
                    anchor = table_gather(h->ator, anchor, 0);
                    return (RemoveResult){.status = REMOVE_GATHERED,
                                          .value = result.value};
                }
            }
            return (RemoveResult){.status = REMOVE_SUCCESS,
                                  .value = result.value};
        }
    }
    return (RemoveResult){.status = REMOVE_NOTFOUND, .value = NULL};
}

void *hamt_remove(HAMT trie, void *key)
{
    Hash hash = {.key = key,
                 .hash_fn = trie->key_hash,
                 .hash = trie->key_hash(key, 0),
                 .depth = 0,
                 .shift = 0};
    RemoveResult rr =
        rem(trie, trie->root, trie->root, hash, trie->key_cmp, key);
    if (rr.status == REMOVE_SUCCESS || rr.status == REMOVE_GATHERED) {
        trie->size -= 1;
        return untagged(rr.value);
    }
    return NULL;
}

void delete (HAMT h, HamtNode *anchor)
{
    assert(!is_value(anchor->as.kv.value) &&
           "delete requires an internal node");
    size_t n = get_popcount(anchor->as.table.index);
    for (size_t i = 0; i < n; ++i) {
        if (!is_value(anchor->as.table.ptr[i].as.kv.value)) {
            delete (h, &anchor->as.table.ptr[i]);
        }
    }
    table_free(h->ator, anchor->as.table.ptr, n);
}

void hamt_delete(HAMT trie)
{
    delete (trie, trie->root);
    hamt_free(trie->ator, trie->root);
    hamt_free(trie->ator, trie);
}

size_t hamt_size(const HAMT trie) { return trie->size; }

struct HamtIteratorItem {
    HamtNode *anchor;
    size_t pos;
    struct HamtIteratorItem *next;
};

struct HamtIteratorImpl {
    HAMT trie;
    HamtNode *cur;
    struct HamtIteratorItem *head, *tail;
};

static struct HamtIteratorItem *iterator_push_item(HamtIterator it,
                                                   HamtNode *anchor, size_t pos)
{
    /* append at the end */
    struct HamtIteratorItem *new_item = malloc(sizeof(struct HamtIteratorItem));
    if (new_item) {
        new_item->anchor = anchor;
        new_item->pos = pos;
        new_item->next = NULL;
        if (it->tail) {
            it->tail->next = new_item;
        } else {
            /* first insert */
            it->tail = it->head = new_item;
        }
    }
    return new_item;
}

static struct HamtIteratorItem *iterator_peek_item(HamtIterator it)
{
    return it->head;
}

static struct HamtIteratorItem *iterator_pop_item(HamtIterator it)
{
    /* pop from front */
    struct HamtIteratorItem *top = it->head;
    it->head = it->head->next;
    return top;
}

HamtIterator hamt_it_create(const HAMT trie)
{
    struct HamtIteratorImpl *it =
        hamt_alloc(trie->ator, sizeof(struct HamtIteratorImpl));
    it->trie = trie;
    it->cur = NULL;
    it->head = it->tail = NULL;
    iterator_push_item(it, trie->root, 0);
    it->head = it->tail;
    hamt_it_next(it);
    return it;
}

void hamt_it_delete(const HAMT trie, HamtIterator it)
{
    struct HamtIteratorItem *p = it->head;
    struct HamtIteratorItem *tmp;
    while (p) {
        tmp = p;
        p = p->next;
        hamt_free(trie->ator, tmp);
    }
    hamt_free(trie->ator, it);
}

#define TABLE(a) a->as.table.ptr
#define INDEX(a) a->as.table.index
#define VALUE(a) a->as.kv.value
#define KEY(a) a->as.kv.key

inline bool hamt_it_valid(HamtIterator it) { return it->cur != NULL; }

HamtIterator hamt_it_next(HamtIterator it)
{
    struct HamtIteratorItem *p;
    while (it && (p = iterator_peek_item(it)) != NULL) {
        int n_rows = get_popcount(INDEX(p->anchor));
        // printf("anchor size: %i; p->pos: %lu\n", n_rows, p->pos);
        for (int i = p->pos; i < n_rows; ++i) {
            // printf("pos: %i of %i\n", i, n_rows);
            HamtNode *cur = &TABLE(p->anchor)[i];
            if (is_value(VALUE(cur))) {
                if (i < n_rows - 1) {
                    p->pos = i + 1;
                } else {
                    iterator_pop_item(it);
                }
                it->cur = cur;
                return it;
            }
            /* cur is a pointer to a subtable */
            iterator_push_item(it, cur, 0);
        }
        iterator_pop_item(it);
    }
    it->cur = NULL;
    return it;
}

const void *hamt_it_get_key(HamtIterator it)
{
    if (it->cur) {
        return KEY(it->cur);
    }
    return NULL;
}

const void *hamt_it_get_value(HamtIterator it)
{
    if (it->cur) {
        return untagged(VALUE(it->cur));
    }
    return NULL;
}
