/*
 * QTC - Codebook construction, serialization, deserialization
 */
#ifndef QTC_CB_H
#define QTC_CB_H

#include <stdint.h>
#include <stdbool.h>
#include "qtc.h"
#include "ht.h"

/* ── Codebook sizes per tier ──────────────────────── */
typedef struct {
    uint32_t uni, bi, tri, fg, eg, tg, vg, tfg, ffg, efg, ofg;
} qtc_cb_sizes_t;

qtc_cb_sizes_t auto_codebook_sizes(uint32_t n_words);

/* ── Codebook data ────────────────────────────────── */
typedef struct {
    /* Word interning table */
    qtc_bmap_t intern;       /* bytes -> word_id */
    uint8_t   *pool;         /* all word bytes */
    uint64_t  *pool_offs;    /* offset of each unique word in pool */
    uint16_t  *pool_lens;    /* length of each unique word */
    uint32_t   n_unique;     /* number of unique words */
    uint64_t   pool_size;
    uint64_t   pool_cap;
    uint32_t   words_cap;

    /* Word IDs array (words[i] -> word_id) */
    uint32_t  *word_ids;
    uint32_t   n_words;

    /* Unigram codebook */
    uint32_t  *uni_wids;     /* word_ids of unigram entries */
    uint32_t   n_uni;
    int32_t   *uni_rmap;     /* word_id -> codebook_index (-1 if absent) */
    uint32_t   uni_rmap_sz;

    /* Bigram codebook */
    uint32_t  *bi_wids;      /* pairs: [2*i], [2*i+1] */
    uint32_t   n_bi;
    qtc_u64map_t bi_map;     /* pack_bi(wid1,wid2) -> codebook_index */

    /* N-gram codebooks (levels 1..7 = tri through 55g) */
    /* Each stored as flat word_id arrays + lookup maps */
    uint32_t  *ng_wids[QTC_N_LEVELS];    /* ng_wids[k][i * ng_len + j] */
    uint32_t   ng_count[QTC_N_LEVELS];
    qtc_nmap_t ng_maps[QTC_N_LEVELS];

} qtc_cbs_t;

/* ── Build codebooks from words ───────────────────── */
/* word_ptrs[i] / word_lens[i] describe each word.
 * Allocates and fills cbs. Caller must call cbs_free(). */
void cbs_build(qtc_cbs_t *cbs,
               const uint8_t **word_ptrs, const uint16_t *word_lens,
               uint32_t n_words, const qtc_cb_sizes_t *sizes);

/* ── Serialize codebooks ──────────────────────────── */
uint8_t *cbs_encode(const qtc_cbs_t *cbs, uint32_t *out_len);

/* ── Deserialize codebooks ────────────────────────── */
/* Reads codebook from data[off:]. Sets *new_off.
 * Only fills uni/bi/ng_wids and counts, pool, etc.
 * No lookup maps are built (not needed for decompress). */
void cbs_decode(qtc_cbs_t *cbs, const uint8_t *data, uint32_t off, uint32_t *new_off);

/* Free just the intern bmap (hash table) early to reclaim memory.
 * Keeps pool, pool_offs, pool_lens, word_ids, and codebook data intact. */
void cbs_free_intern(qtc_cbs_t *cbs);

void cbs_free(qtc_cbs_t *cbs);

#endif /* QTC_CB_H */
