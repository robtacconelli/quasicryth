/*
 * QTC - Hash table primitives
 * Three variants: byte-keyed (word interning), u64-keyed (bigrams),
 * n-gram (start-index approach for trigrams through 55-grams).
 */
#ifndef QTC_HT_H
#define QTC_HT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ── FNV-1a hash ─────────────────────────────────────── */
static inline uint32_t fnv32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
    return h | 1;  /* nonzero sentinel */
}
static inline uint64_t fnv64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h | 1;
}

/* ── Byte-keyed map (word interning) ─────────────────── */
typedef struct {
    uint32_t *hash;      /* 0 = empty slot */
    uint32_t *key_off;
    uint16_t *key_len;
    uint32_t *value;
    uint32_t  cap;       /* power of 2 */
    uint32_t  count;
    uint8_t  *pool;      /* key byte storage */
    uint32_t  pool_sz, pool_cap;
} qtc_bmap_t;

void     bmap_init(qtc_bmap_t *m, uint32_t est);
bool     bmap_get(const qtc_bmap_t *m, const uint8_t *key, uint16_t klen, uint32_t *val);
uint32_t bmap_put_val(qtc_bmap_t *m, const uint8_t *key, uint16_t klen, uint32_t val);
void     bmap_free(qtc_bmap_t *m);

/* ── uint64-keyed map (bigram lookup) ────────────────── */
typedef struct {
    uint64_t *key;       /* 0 = empty slot */
    uint32_t *value;
    uint32_t  cap;
    uint32_t  count;
} qtc_u64map_t;

void     u64map_init(qtc_u64map_t *m, uint32_t est);
bool     u64map_get(const qtc_u64map_t *m, uint64_t key, uint32_t *val);
void     u64map_set(qtc_u64map_t *m, uint64_t key, uint32_t val);
/* Increment value (insert with 1 if new). Returns new count. */
uint32_t u64map_inc(qtc_u64map_t *m, uint64_t key);
void     u64map_free(qtc_u64map_t *m);

static inline uint64_t pack_bi(uint32_t a, uint32_t b) {
    return ((uint64_t)(a + 1) << 32) | (uint64_t)(b + 1);  /* +1 so pack(0,0) != 0 */
}

/* ── N-gram map (start-index approach) ───────────────── */
typedef struct {
    uint64_t *hash;      /* 0 = empty */
    uint32_t *start;     /* index into word_ids array */
    uint32_t *value;
    uint32_t  cap;
    uint32_t  count;
    uint8_t   n;         /* n-gram length */
    const uint32_t *wids;/* pointer to word_ids (not owned) */
} qtc_nmap_t;

void     nmap_init(qtc_nmap_t *m, uint8_t n, const uint32_t *wids, uint32_t est);
bool     nmap_get(const qtc_nmap_t *m, uint32_t start_idx, uint32_t *val);
void     nmap_set(qtc_nmap_t *m, uint32_t start_idx, uint32_t val);
uint32_t nmap_inc(qtc_nmap_t *m, uint32_t start_idx);
/* Prune entries with value < min_val. Rebuilds table. */
void     nmap_prune(qtc_nmap_t *m, uint32_t min_val);
void     nmap_free(qtc_nmap_t *m);

/* Compute hash for n words starting at wids[start] */
static inline uint64_t nmap_hash(const uint32_t *wids, uint32_t start, uint8_t n) {
    return fnv64(&wids[start], n * sizeof(uint32_t));
}
/* Compare n words starting at positions a and b */
static inline bool nmap_eq(const uint32_t *wids, uint32_t a, uint32_t b, uint8_t n) {
    return memcmp(&wids[a], &wids[b], n * sizeof(uint32_t)) == 0;
}

/* ── Dynamic byte buffer ─────────────────────────────── */
typedef struct {
    uint8_t  *data;
    uint32_t  len, cap;
} qtc_buf_t;

static inline void buf_init(qtc_buf_t *b, uint32_t cap) {
    b->data = (uint8_t *)malloc(cap); b->len = 0; b->cap = cap;
}
static inline void buf_ensure(qtc_buf_t *b, uint32_t need) {
    if (b->len + need > b->cap) {
        while (b->len + need > b->cap) b->cap *= 2;
        b->data = (uint8_t *)realloc(b->data, b->cap);
    }
}
static inline void buf_push(qtc_buf_t *b, uint8_t v) {
    buf_ensure(b, 1); b->data[b->len++] = v;
}
static inline void buf_append(qtc_buf_t *b, const uint8_t *d, uint32_t n) {
    buf_ensure(b, n); memcpy(b->data + b->len, d, n); b->len += n;
}
static inline void buf_write16(qtc_buf_t *b, uint16_t v) {
    buf_ensure(b, 2); memcpy(b->data + b->len, &v, 2); b->len += 2;
}
static inline void buf_write32(qtc_buf_t *b, uint32_t v) {
    buf_ensure(b, 4); memcpy(b->data + b->len, &v, 4); b->len += 4;
}
static inline void buf_free(qtc_buf_t *b) { free(b->data); b->data = NULL; }

#endif /* QTC_HT_H */
