/*
 * QTC - Hash table implementations
 */
#include "ht.h"
#include <stdio.h>

static uint32_t next_pow2(uint32_t v) {
    v--; v|=v>>1; v|=v>>2; v|=v>>4; v|=v>>8; v|=v>>16; return v+1;
}

/* ══════════════════════════════════════════════════════
 * Byte-keyed map
 * ══════════════════════════════════════════════════════ */
void bmap_init(qtc_bmap_t *m, uint32_t est) {
    m->cap = next_pow2(est < 16 ? 16 : est * 2);
    m->count = 0;
    m->hash    = (uint32_t *)calloc(m->cap, sizeof(uint32_t));
    m->key_off = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
    m->key_len = (uint16_t *)malloc(m->cap * sizeof(uint16_t));
    m->value   = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
    m->pool_cap = est * 8 + 256;
    m->pool_sz = 0;
    m->pool = (uint8_t *)malloc(m->pool_cap);
}

static void bmap_grow(qtc_bmap_t *m);

bool bmap_get(const qtc_bmap_t *m, const uint8_t *key, uint16_t klen, uint32_t *val) {
    uint32_t h = fnv32(key, klen);
    uint32_t mask = m->cap - 1;
    uint32_t i = h & mask;
    while (m->hash[i]) {
        if (m->hash[i] == h && m->key_len[i] == klen &&
            memcmp(m->pool + m->key_off[i], key, klen) == 0) {
            *val = m->value[i];
            return true;
        }
        i = (i + 1) & mask;
    }
    return false;
}

uint32_t bmap_put_val(qtc_bmap_t *m, const uint8_t *key, uint16_t klen, uint32_t val) {
    if (m->count * 4 >= m->cap * 3) bmap_grow(m);  /* 75% load */
    uint32_t h = fnv32(key, klen);
    uint32_t mask = m->cap - 1;
    uint32_t i = h & mask;
    while (m->hash[i]) {
        if (m->hash[i] == h && m->key_len[i] == klen &&
            memcmp(m->pool + m->key_off[i], key, klen) == 0) {
            m->value[i] = val;
            return val;
        }
        i = (i + 1) & mask;
    }
    /* Insert new */
    if (m->pool_sz + klen > m->pool_cap) {
        while (m->pool_sz + klen > m->pool_cap) m->pool_cap *= 2;
        m->pool = (uint8_t *)realloc(m->pool, m->pool_cap);
    }
    m->hash[i] = h;
    m->key_off[i] = m->pool_sz;
    m->key_len[i] = klen;
    m->value[i] = val;
    memcpy(m->pool + m->pool_sz, key, klen);
    m->pool_sz += klen;
    m->count++;
    return val;
}

static void bmap_grow(qtc_bmap_t *m) {
    uint32_t old_cap = m->cap;
    uint32_t *old_hash = m->hash, *old_ko = m->key_off, *old_val = m->value;
    uint16_t *old_kl = m->key_len;

    m->cap = old_cap * 2;
    m->hash    = (uint32_t *)calloc(m->cap, sizeof(uint32_t));
    m->key_off = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
    m->key_len = (uint16_t *)malloc(m->cap * sizeof(uint16_t));
    m->value   = (uint32_t *)malloc(m->cap * sizeof(uint32_t));

    uint32_t mask = m->cap - 1;
    for (uint32_t j = 0; j < old_cap; j++) {
        if (!old_hash[j]) continue;
        uint32_t i = old_hash[j] & mask;
        while (m->hash[i]) i = (i + 1) & mask;
        m->hash[i] = old_hash[j];
        m->key_off[i] = old_ko[j];
        m->key_len[i] = old_kl[j];
        m->value[i] = old_val[j];
    }
    free(old_hash); free(old_ko); free(old_kl); free(old_val);
}

void bmap_free(qtc_bmap_t *m) {
    free(m->hash); free(m->key_off); free(m->key_len); free(m->value);
    free(m->pool);
    memset(m, 0, sizeof(*m));
}

/* ══════════════════════════════════════════════════════
 * uint64-keyed map
 * ══════════════════════════════════════════════════════ */
void u64map_init(qtc_u64map_t *m, uint32_t est) {
    m->cap = next_pow2(est < 16 ? 16 : est * 2);
    m->count = 0;
    m->key   = (uint64_t *)calloc(m->cap, sizeof(uint64_t));
    m->value = (uint32_t *)calloc(m->cap, sizeof(uint32_t));
}

static void u64map_grow(qtc_u64map_t *m) {
    uint32_t old_cap = m->cap;
    uint64_t *old_key = m->key;
    uint32_t *old_val = m->value;
    m->cap = old_cap * 2;
    m->key   = (uint64_t *)calloc(m->cap, sizeof(uint64_t));
    m->value = (uint32_t *)calloc(m->cap, sizeof(uint32_t));
    uint32_t mask = m->cap - 1;
    for (uint32_t j = 0; j < old_cap; j++) {
        if (!old_key[j]) continue;
        uint32_t h = (uint32_t)(old_key[j] * 11400714819323198485ULL >> 32);
        uint32_t i = h & mask;
        while (m->key[i]) i = (i + 1) & mask;
        m->key[i] = old_key[j];
        m->value[i] = old_val[j];
    }
    free(old_key); free(old_val);
}

bool u64map_get(const qtc_u64map_t *m, uint64_t key, uint32_t *val) {
    if (!key) return false;  /* 0 is sentinel */
    uint32_t h = (uint32_t)(key * 11400714819323198485ULL >> 32);
    uint32_t mask = m->cap - 1;
    uint32_t i = h & mask;
    while (m->key[i]) {
        if (m->key[i] == key) { *val = m->value[i]; return true; }
        i = (i + 1) & mask;
    }
    return false;
}

void u64map_set(qtc_u64map_t *m, uint64_t key, uint32_t val) {
    if (!key) return;
    if (m->count * 4 >= m->cap * 3) u64map_grow(m);
    uint32_t h = (uint32_t)(key * 11400714819323198485ULL >> 32);
    uint32_t mask = m->cap - 1;
    uint32_t i = h & mask;
    while (m->key[i]) {
        if (m->key[i] == key) { m->value[i] = val; return; }
        i = (i + 1) & mask;
    }
    m->key[i] = key; m->value[i] = val; m->count++;
}

uint32_t u64map_inc(qtc_u64map_t *m, uint64_t key) {
    if (!key) return 0;
    if (m->count * 4 >= m->cap * 3) u64map_grow(m);
    uint32_t h = (uint32_t)(key * 11400714819323198485ULL >> 32);
    uint32_t mask = m->cap - 1;
    uint32_t i = h & mask;
    while (m->key[i]) {
        if (m->key[i] == key) return ++m->value[i];
        i = (i + 1) & mask;
    }
    m->key[i] = key; m->value[i] = 1; m->count++;
    return 1;
}

void u64map_free(qtc_u64map_t *m) {
    free(m->key); free(m->value);
    memset(m, 0, sizeof(*m));
}

/* ══════════════════════════════════════════════════════
 * N-gram map (start-index based)
 * ══════════════════════════════════════════════════════ */
void nmap_init(qtc_nmap_t *m, uint8_t n, const uint32_t *wids, uint32_t est) {
    m->n = n;
    m->wids = wids;
    m->cap = next_pow2(est < 16 ? 16 : est * 2);
    m->count = 0;
    m->hash  = (uint64_t *)calloc(m->cap, sizeof(uint64_t));
    m->start = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
    m->value = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
}

static void nmap_grow(qtc_nmap_t *m) {
    uint32_t old_cap = m->cap;
    uint64_t *oh = m->hash; uint32_t *os = m->start, *ov = m->value;
    m->cap = old_cap * 2;
    m->hash  = (uint64_t *)calloc(m->cap, sizeof(uint64_t));
    m->start = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
    m->value = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
    uint32_t mask = m->cap - 1;
    for (uint32_t j = 0; j < old_cap; j++) {
        if (!oh[j]) continue;
        uint32_t i = (uint32_t)(oh[j] >> 32) & mask;
        while (m->hash[i]) i = (i + 1) & mask;
        m->hash[i] = oh[j]; m->start[i] = os[j]; m->value[i] = ov[j];
    }
    free(oh); free(os); free(ov);
}

bool nmap_get(const qtc_nmap_t *m, uint32_t si, uint32_t *val) {
    uint64_t h = nmap_hash(m->wids, si, m->n);
    uint32_t mask = m->cap - 1;
    uint32_t i = (uint32_t)(h >> 32) & mask;
    while (m->hash[i]) {
        if (m->hash[i] == h && nmap_eq(m->wids, m->start[i], si, m->n)) {
            *val = m->value[i]; return true;
        }
        i = (i + 1) & mask;
    }
    return false;
}

void nmap_set(qtc_nmap_t *m, uint32_t si, uint32_t val) {
    if (m->count * 4 >= m->cap * 3) nmap_grow(m);
    uint64_t h = nmap_hash(m->wids, si, m->n);
    uint32_t mask = m->cap - 1;
    uint32_t i = (uint32_t)(h >> 32) & mask;
    while (m->hash[i]) {
        if (m->hash[i] == h && nmap_eq(m->wids, m->start[i], si, m->n)) {
            m->value[i] = val; return;
        }
        i = (i + 1) & mask;
    }
    m->hash[i] = h; m->start[i] = si; m->value[i] = val; m->count++;
}

uint32_t nmap_inc(qtc_nmap_t *m, uint32_t si) {
    if (m->count * 4 >= m->cap * 3) nmap_grow(m);
    uint64_t h = nmap_hash(m->wids, si, m->n);
    uint32_t mask = m->cap - 1;
    uint32_t i = (uint32_t)(h >> 32) & mask;
    while (m->hash[i]) {
        if (m->hash[i] == h && nmap_eq(m->wids, m->start[i], si, m->n)) {
            return ++m->value[i];
        }
        i = (i + 1) & mask;
    }
    m->hash[i] = h; m->start[i] = si; m->value[i] = 1; m->count++;
    return 1;
}

void nmap_prune(qtc_nmap_t *m, uint32_t min_val) {
    uint32_t old_cap = m->cap;
    uint64_t *oh = m->hash; uint32_t *os = m->start, *ov = m->value;
    /* Keep same capacity */
    m->hash  = (uint64_t *)calloc(m->cap, sizeof(uint64_t));
    m->start = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
    m->value = (uint32_t *)malloc(m->cap * sizeof(uint32_t));
    m->count = 0;
    uint32_t mask = m->cap - 1;
    for (uint32_t j = 0; j < old_cap; j++) {
        if (!oh[j] || ov[j] < min_val) continue;
        uint32_t i = (uint32_t)(oh[j] >> 32) & mask;
        while (m->hash[i]) i = (i + 1) & mask;
        m->hash[i] = oh[j]; m->start[i] = os[j]; m->value[i] = ov[j];
        m->count++;
    }
    free(oh); free(os); free(ov);
}

void nmap_free(qtc_nmap_t *m) {
    free(m->hash); free(m->start); free(m->value);
    memset(m, 0, sizeof(*m));
}
