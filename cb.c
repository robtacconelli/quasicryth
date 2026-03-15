/*
 * QTC - Codebook construction and serialization
 */
#include "cb.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════
 * Auto codebook sizes
 * ══════════════════════════════════════════════════════ */
qtc_cb_sizes_t auto_codebook_sizes(uint32_t nw) {
    if (nw < 5000)         return (qtc_cb_sizes_t){509, 509, 350, 100, 50, 0, 0, 0, 0, 0, 0};
    else if (nw < 50000)   return (qtc_cb_sizes_t){509, 509, 350, 200, 100, 50, 0, 0, 0, 0, 0};
    else if (nw < 200000)  return (qtc_cb_sizes_t){2000, 1500, 500, 500, 200, 100, 50, 0, 0, 0, 0};
    else if (nw < 500000)  return (qtc_cb_sizes_t){4000, 3000, 1000, 1000, 500, 300, 100, 50, 0, 0, 0};
    else if (nw < 2000000) return (qtc_cb_sizes_t){8000, 6000, 2000, 2000, 1000, 1000, 500, 200, 0, 0, 0};
    else if (nw < 10000000)return (qtc_cb_sizes_t){8000, 6000, 2000, 2000, 1000, 1000, 500, 500, 200, 0, 0};
    else                   return (qtc_cb_sizes_t){8000, 6000, 2000, 2000, 1000, 1000, 500, 500, 200, 200, 100};
}

/* ══════════════════════════════════════════════════════
 * Word interning
 * ══════════════════════════════════════════════════════ */
static void intern_init(qtc_cbs_t *cbs, uint32_t est_unique) {
    bmap_init(&cbs->intern, est_unique);
    cbs->pool_cap = est_unique * 8;
    cbs->pool = (uint8_t *)malloc(cbs->pool_cap);
    cbs->pool_size = 0;
    cbs->words_cap = est_unique;
    cbs->pool_offs = (uint32_t *)malloc(cbs->words_cap * sizeof(uint32_t));
    cbs->pool_lens = (uint16_t *)malloc(cbs->words_cap * sizeof(uint16_t));
    cbs->n_unique = 0;
}

static uint32_t intern_word(qtc_cbs_t *cbs, const uint8_t *data, uint16_t len) {
    uint32_t existing;
    if (bmap_get(&cbs->intern, data, len, &existing))
        return existing;

    uint32_t id = cbs->n_unique;
    if (id >= cbs->words_cap) {
        cbs->words_cap *= 2;
        cbs->pool_offs = realloc(cbs->pool_offs, cbs->words_cap * sizeof(uint32_t));
        cbs->pool_lens = realloc(cbs->pool_lens, cbs->words_cap * sizeof(uint16_t));
    }
    if (cbs->pool_size + len > cbs->pool_cap) {
        while (cbs->pool_size + len > cbs->pool_cap) cbs->pool_cap *= 2;
        cbs->pool = realloc(cbs->pool, cbs->pool_cap);
    }
    cbs->pool_offs[id] = cbs->pool_size;
    cbs->pool_lens[id] = len;
    memcpy(cbs->pool + cbs->pool_size, data, len);
    cbs->pool_size += len;
    cbs->n_unique++;
    bmap_put_val(&cbs->intern, data, len, id);
    return id;
}

/* ══════════════════════════════════════════════════════
 * Comparison functions for qsort
 * ══════════════════════════════════════════════════════ */
typedef struct { uint32_t key; uint32_t count; } kv32_t;
typedef struct { uint64_t key; uint32_t count; } kv64_t;
typedef struct { uint32_t start; uint32_t count; } kvng_t;

static int cmp_kv32_desc(const void *a, const void *b) {
    uint32_t ca = ((const kv32_t *)a)->count, cb = ((const kv32_t *)b)->count;
    return (ca > cb) ? -1 : (ca < cb) ? 1 : 0;
}
static int cmp_kv64_desc(const void *a, const void *b) {
    uint32_t ca = ((const kv64_t *)a)->count, cb = ((const kv64_t *)b)->count;
    return (ca > cb) ? -1 : (ca < cb) ? 1 : 0;
}
static int cmp_kvng_desc(const void *a, const void *b) {
    uint32_t ca = ((const kvng_t *)a)->count, cb = ((const kvng_t *)b)->count;
    return (ca > cb) ? -1 : (ca < cb) ? 1 : 0;
}

/* ══════════════════════════════════════════════════════
 * Build codebooks
 * ══════════════════════════════════════════════════════ */
void cbs_build(qtc_cbs_t *cbs,
               const uint8_t **word_ptrs, const uint16_t *word_lens,
               uint32_t n_words, const qtc_cb_sizes_t *sizes) {
    static const uint8_t ng_lens[QTC_N_LEVELS] = {3, 5, 8, 13, 21, 34, 55, 89, 144};

    memset(cbs, 0, sizeof(*cbs));
    cbs->n_words = n_words;

    /* --- Intern all words --- */
    intern_init(cbs, n_words / 4 + 256);
    cbs->word_ids = (uint32_t *)malloc(n_words * sizeof(uint32_t));
    for (uint32_t i = 0; i < n_words; i++)
        cbs->word_ids[i] = intern_word(cbs, word_ptrs[i], word_lens[i]);

    uint32_t n_unique = cbs->n_unique;
    const uint32_t *wids = cbs->word_ids;

    /* ── Unigram codebook ─────────────────────────── */
    uint32_t *uni_freq = (uint32_t *)calloc(n_unique, sizeof(uint32_t));
    for (uint32_t i = 0; i < n_words; i++) uni_freq[wids[i]]++;

    kv32_t *uni_ents = (kv32_t *)malloc(n_unique * sizeof(kv32_t));
    for (uint32_t i = 0; i < n_unique; i++) {
        uni_ents[i].key = i;
        uni_ents[i].count = uni_freq[i];
    }
    qsort(uni_ents, n_unique, sizeof(kv32_t), cmp_kv32_desc);
    free(uni_freq);

    cbs->n_uni = sizes->uni < n_unique ? sizes->uni : n_unique;
    cbs->uni_wids = (uint32_t *)malloc(cbs->n_uni * sizeof(uint32_t));
    for (uint32_t i = 0; i < cbs->n_uni; i++)
        cbs->uni_wids[i] = uni_ents[i].key;
    free(uni_ents);

    /* Build uni reverse map: word_id -> codebook index */
    cbs->uni_rmap_sz = n_unique;
    cbs->uni_rmap = (int32_t *)malloc(n_unique * sizeof(int32_t));
    for (uint32_t i = 0; i < n_unique; i++) cbs->uni_rmap[i] = -1;
    for (uint32_t i = 0; i < cbs->n_uni; i++)
        cbs->uni_rmap[cbs->uni_wids[i]] = (int32_t)i;

    /* ── Bigram codebook ──────────────────────────── */
    {
        uint32_t bi_est = n_words < 1000000 ? n_words : 1000000;
        qtc_u64map_t bi_freq;
        u64map_init(&bi_freq, bi_est);
        for (uint32_t i = 0; i + 1 < n_words; i++)
            u64map_inc(&bi_freq, pack_bi(wids[i], wids[i + 1]));

        /* Collect candidates where both words in uni_map */
        kv64_t *bi_ents = (kv64_t *)malloc(bi_freq.count * sizeof(kv64_t));
        uint32_t n_cand = 0;
        for (uint32_t j = 0; j < bi_freq.cap; j++) {
            if (!bi_freq.key[j]) continue;
            uint64_t pk = bi_freq.key[j];
            uint32_t w1 = (uint32_t)(pk >> 32) - 1;
            uint32_t w2 = (uint32_t)(pk & 0xFFFFFFFF) - 1;
            if (w1 < n_unique && cbs->uni_rmap[w1] >= 0 &&
                w2 < n_unique && cbs->uni_rmap[w2] >= 0) {
                bi_ents[n_cand].key = pk;
                bi_ents[n_cand].count = bi_freq.value[j];
                n_cand++;
            }
        }
        qsort(bi_ents, n_cand, sizeof(kv64_t), cmp_kv64_desc);
        u64map_free(&bi_freq);

        cbs->n_bi = sizes->bi < n_cand ? sizes->bi : n_cand;
        cbs->bi_wids = (uint32_t *)malloc((cbs->n_bi * 2 + 1) * sizeof(uint32_t));
        u64map_init(&cbs->bi_map, cbs->n_bi);
        for (uint32_t i = 0; i < cbs->n_bi; i++) {
            uint64_t pk = bi_ents[i].key;
            cbs->bi_wids[2 * i] = (uint32_t)(pk >> 32) - 1;
            cbs->bi_wids[2 * i + 1] = (uint32_t)(pk & 0xFFFFFFFF) - 1;
            u64map_set(&cbs->bi_map, pk, i);
        }
        free(bi_ents);
    }

    /* ── N-gram codebooks (tri through 55g) ───────── */
    const uint32_t max_counts[QTC_N_LEVELS] = {
        sizes->tri, sizes->fg, sizes->eg, sizes->tg,
        sizes->vg, sizes->tfg, sizes->ffg, sizes->efg, sizes->ofg
    };

    for (int lv = 0; lv < QTC_N_LEVELS; lv++) {
        uint32_t max_n = max_counts[lv];
        uint8_t ng = ng_lens[lv];
        cbs->ng_count[lv] = 0;
        cbs->ng_wids[lv] = NULL;
        memset(&cbs->ng_maps[lv], 0, sizeof(cbs->ng_maps[lv]));

        if (max_n == 0 || n_words < ng) continue;

        /* Frequency counting with singleton pruning for large n-grams */
        uint32_t n_iter = n_words - ng + 1;
        uint32_t prune_interval = (ng >= 55) ? 200000 : (ng >= 13) ? 500000 : 0;
        qtc_nmap_t freq;
        nmap_init(&freq, ng, wids, n_iter < 500000 ? n_iter : 500000);

        for (uint32_t i = 0; i < n_iter; i++) {
            nmap_inc(&freq, i);
            if (prune_interval && ((i + 1) % prune_interval == 0))
                nmap_prune(&freq, 2);
        }

        /* Extract top-N (count >= 2, all words in uni_map) */
        kvng_t *ng_ents = (kvng_t *)malloc(freq.count * sizeof(kvng_t));
        uint32_t n_cand = 0;
        for (uint32_t j = 0; j < freq.cap; j++) {
            if (!freq.hash[j] || freq.value[j] < 2) continue;
            uint32_t st = freq.start[j];
            /* Check all words in unigram map */
            bool ok = true;
            for (uint8_t k = 0; k < ng; k++) {
                if (wids[st + k] >= n_unique || cbs->uni_rmap[wids[st + k]] < 0)
                    { ok = false; break; }
            }
            if (ok) {
                ng_ents[n_cand].start = st;
                ng_ents[n_cand].count = freq.value[j];
                n_cand++;
            }
        }
        qsort(ng_ents, n_cand, sizeof(kvng_t), cmp_kvng_desc);

        uint32_t take = max_n < n_cand ? max_n : n_cand;

        /* Store word_id tuples + build lookup map
         * The lookup map references the original wids array. During encoding,
         * we query wids[wpos:wpos+n] which is in the same array, so
         * nmap_eq comparisons work correctly. */
        cbs->ng_wids[lv] = (uint32_t *)malloc(take * ng * sizeof(uint32_t));
        nmap_init(&cbs->ng_maps[lv], ng, wids, take * 4);

        /* First, build a set of taken entries by their content hash
         * to deduplicate (different start positions, same content) */
        uint32_t kept = 0;
        for (uint32_t i = 0; i < take; i++) {
            uint32_t st = ng_ents[i].start;
            /* Check not duplicate content (nmap_get returns first match) */
            uint32_t existing;
            if (nmap_get(&cbs->ng_maps[lv], st, &existing))
                continue;  /* already have this n-gram */
            memcpy(&cbs->ng_wids[lv][kept * ng], &wids[st], ng * sizeof(uint32_t));
            nmap_set(&cbs->ng_maps[lv], st, kept);
            kept++;
        }
        cbs->ng_count[lv] = kept;

        free(ng_ents);
        nmap_free(&freq);
    }
}

/* ══════════════════════════════════════════════════════
 * Serialize codebooks
 * ══════════════════════════════════════════════════════ */
uint8_t *cbs_encode(const qtc_cbs_t *cbs, uint32_t *out_len) {
    static const uint8_t ng_lens[QTC_N_LEVELS] = {3, 5, 8, 13, 21, 34, 55, 89, 144};
    qtc_buf_t b;
    buf_init(&b, 65536);

    /* (2 + N_LEVELS) x uint16 counts */
    buf_write16(&b, (uint16_t)cbs->n_uni);
    buf_write16(&b, (uint16_t)cbs->n_bi);
    for (int lv = 0; lv < QTC_N_LEVELS; lv++)
        buf_write16(&b, (uint16_t)cbs->ng_count[lv]);

    /* Unigrams: length-prefixed byte sequences */
    for (uint32_t i = 0; i < cbs->n_uni; i++) {
        uint32_t wid = cbs->uni_wids[i];
        uint16_t wlen = cbs->pool_lens[wid];
        uint8_t slen = (wlen < 255) ? (uint8_t)wlen : 255;
        buf_push(&b, slen);
        buf_append(&b, cbs->pool + cbs->pool_offs[wid], slen);
    }

    /* Bigrams: pairs of unigram indices (uint16) */
    for (uint32_t i = 0; i < cbs->n_bi; i++) {
        uint32_t w1 = cbs->bi_wids[2 * i];
        uint32_t w2 = cbs->bi_wids[2 * i + 1];
        int32_t i1 = (w1 < cbs->uni_rmap_sz) ? cbs->uni_rmap[w1] : -1;
        int32_t i2 = (w2 < cbs->uni_rmap_sz) ? cbs->uni_rmap[w2] : -1;
        buf_write16(&b, (i1 >= 0) ? (uint16_t)i1 : 0xFFFF);
        buf_write16(&b, (i2 >= 0) ? (uint16_t)i2 : 0xFFFF);
    }

    /* N-gram codebooks: tuples of unigram indices */
    for (int lv = 0; lv < QTC_N_LEVELS; lv++) {
        uint8_t ng = ng_lens[lv];
        for (uint32_t i = 0; i < cbs->ng_count[lv]; i++) {
            for (uint8_t k = 0; k < ng; k++) {
                uint32_t wid = cbs->ng_wids[lv][i * ng + k];
                int32_t ui = (wid < cbs->uni_rmap_sz) ? cbs->uni_rmap[wid] : -1;
                buf_write16(&b, (ui >= 0) ? (uint16_t)ui : 0xFFFF);
            }
        }
    }

    *out_len = b.len;
    return b.data;
}

/* ══════════════════════════════════════════════════════
 * Deserialize codebooks (for decompression)
 * ══════════════════════════════════════════════════════ */
void cbs_decode(qtc_cbs_t *cbs, const uint8_t *data, uint32_t off, uint32_t *new_off) {
    static const uint8_t ng_lens[QTC_N_LEVELS] = {3, 5, 8, 13, 21, 34, 55, 89, 144};
    memset(cbs, 0, sizeof(*cbs));

    /* Read (2 + N_LEVELS) uint16 counts */
    uint16_t counts[2 + QTC_N_LEVELS];
    uint32_t hdr_sz = (2 + QTC_N_LEVELS) * 2;
    memcpy(counts, &data[off], hdr_sz); off += hdr_sz;
    cbs->n_uni = counts[0];
    cbs->n_bi = counts[1];
    for (int lv = 0; lv < QTC_N_LEVELS; lv++)
        cbs->ng_count[lv] = counts[2 + lv];

    /* Init intern */
    intern_init(cbs, cbs->n_uni + 256);

    /* Read unigrams */
    cbs->uni_wids = (uint32_t *)malloc(cbs->n_uni * sizeof(uint32_t));
    for (uint32_t i = 0; i < cbs->n_uni; i++) {
        uint8_t wlen = data[off++];
        cbs->uni_wids[i] = intern_word(cbs, &data[off], wlen);
        off += wlen;
    }

    /* Read bigrams as word_id pairs */
    cbs->bi_wids = (uint32_t *)malloc((cbs->n_bi * 2 + 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < cbs->n_bi; i++) {
        uint16_t i1, i2;
        memcpy(&i1, &data[off], 2); off += 2;
        memcpy(&i2, &data[off], 2); off += 2;
        cbs->bi_wids[2 * i] = (i1 < cbs->n_uni) ? cbs->uni_wids[i1] : 0;
        cbs->bi_wids[2 * i + 1] = (i2 < cbs->n_uni) ? cbs->uni_wids[i2] : 0;
    }

    /* Read n-grams as word_id tuples */
    for (int lv = 0; lv < QTC_N_LEVELS; lv++) {
        uint8_t ng = ng_lens[lv];
        uint32_t count = cbs->ng_count[lv];
        if (count == 0) { cbs->ng_wids[lv] = NULL; continue; }
        cbs->ng_wids[lv] = (uint32_t *)malloc(count * ng * sizeof(uint32_t));
        for (uint32_t i = 0; i < count; i++) {
            for (uint8_t k = 0; k < ng; k++) {
                uint16_t ui;
                memcpy(&ui, &data[off], 2); off += 2;
                cbs->ng_wids[lv][i * ng + k] =
                    (ui < cbs->n_uni) ? cbs->uni_wids[ui] : 0;
            }
        }
    }

    *new_off = off;
}

/* ══════════════════════════════════════════════════════
 * Free
 * ══════════════════════════════════════════════════════ */
void cbs_free(qtc_cbs_t *cbs) {
    bmap_free(&cbs->intern);
    free(cbs->pool);
    free(cbs->pool_offs);
    free(cbs->pool_lens);
    free(cbs->word_ids);
    free(cbs->uni_wids);
    free(cbs->uni_rmap);
    free(cbs->bi_wids);
    u64map_free(&cbs->bi_map);
    for (int lv = 0; lv < QTC_N_LEVELS; lv++) {
        free(cbs->ng_wids[lv]);
        nmap_free(&cbs->ng_maps[lv]);
    }
    memset(cbs, 0, sizeof(*cbs));
}
