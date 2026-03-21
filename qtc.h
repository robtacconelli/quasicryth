/*
 * QTC v5.5 Multi-Tiling - Quasicrystalline Tiling Compressor
 * Uses 64 aperiodic tilings for increased n-gram coverage
 */
#ifndef QTC_H
#define QTC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Constants ─────────────────────────────────────── */
#define QTC_MAGIC       "QM56"
#define QTC_VERSION     "5.6.0"
#define QTC_MAX_HIER    10      /* levels 0..9 */
#define QTC_N_LEVELS    9       /* hierarchy levels 1..9 (above level-0) */
#define QTC_ESC         255
#define QTC_EXT         254
#define QTC_HEADER_SIZE 45  /* 37 + 4 (orig_size u32->u64) + 4 (lowered_size u32->u64) */
#define QTC_N_TILINGS       36  /* 12 golden + 24 optimized (9 alphas × 2 phases + 6 original) */
#define QTC_N_TILINGS_FIB   12  /* fibonacci-only: 12 golden-ratio phases */

/* Tiling mode */
#define QTC_TILING_MULTI    0   /* all 18 tilings (default) */
#define QTC_TILING_FIB      1   /* fibonacci-only (12 golden-ratio phases) */
#define QTC_TILING_NONE     2   /* no tiling — unigrams + escapes only (A/B baseline) */
#define QTC_TILING_PERIOD5  3   /* period-5 (LLSLS) tiling — A/B periodic baseline */

extern int qtc_tiling_mode;   /* set before compress/decompress */

/* Fibonacci phrase lengths per hierarchy level (level 0..9) */
static const int QTC_HIER_WORD_LENS[QTC_MAX_HIER] = {2, 3, 5, 8, 13, 21, 34, 55, 89, 144};

/* Encoding levels: 0=escape, 1=unigram, 2=bigram, 3=trigram, ..., 11=144-gram */
#define QTM_N_ENC_LEVELS 12

/* Words per encoding level */
static const int QTM_LEVEL_WORDS[QTM_N_ENC_LEVELS] = {
    1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144
};

/* Codebook size tiers: [uni, bi, tri, 5g, 8g, 13g, 21g, 34g, 55g, 89g, 144g] */
#define QTC_N_CODEBOOKS 11

/* ── Word token ────────────────────────────────────── */
typedef struct {
    const uint8_t  *data;   /* pointer into original buffer (NOT owned) */
    uint16_t        len;
} qtc_word_t;

/* ── Tile ──────────────────────────────────────────── */
typedef struct {
    uint32_t    wpos;       /* word position */
    uint8_t     nwords;     /* 1 (S) or 2 (L) */
    bool        is_L;
} qtc_tile_t;

/* ── Hierarchy level entry ─────────────────────────── */
typedef struct {
    uint32_t    start;      /* start tile index in level below */
    uint32_t    end;        /* end tile index (exclusive) */
    bool        is_L;       /* super-L or super-S */
} qtc_hlevel_t;

/* ── Parent map entry ──────────────────────────────── */
typedef struct {
    int32_t     parent_idx; /* -1 if no parent */
    int8_t      pos;        /* position within parent (0 or 1) */
} qtc_pmap_t;

/* ── Hierarchy structure ───────────────────────────── */
typedef struct {
    qtc_hlevel_t   *levels[QTC_MAX_HIER + 1];   /* levels[0] = tile-level */
    uint32_t        level_count[QTC_MAX_HIER + 1];
    qtc_pmap_t     *parent_maps[QTC_MAX_HIER];  /* parent_maps[k] for level k->k+1 */
    int             n_levels;                    /* how many levels built */
} qtc_hierarchy_t;

/* ── Deep position detection result ────────────────── */
typedef struct {
    bool       **can;       /* can[level][tile_idx] */
    uint32_t   **skip;      /* skip[level][tile_idx] */
    int          max_k;     /* max usable hierarchy level */
} qtc_deep_t;

/* ── Codebook ──────────────────────────────────────── */
typedef struct {
    /* Word storage: flat buffer of all word bytes */
    uint8_t    *word_pool;
    uint32_t    word_pool_size;
    uint32_t    word_pool_cap;

    /* Unigram codebook */
    uint32_t   *uni_offsets;    /* offset into word_pool */
    uint16_t   *uni_lens;       /* length of each word */
    uint32_t    n_uni;

    /* Bigram codebook: pairs of unigram indices */
    uint32_t   *bi_idx;         /* bi_idx[i*2], bi_idx[i*2+1] */
    uint32_t    n_bi;

    /* N-gram codebooks: arrays of unigram index tuples */
    /* level k (1..9) stores tuples of QTC_HIER_WORD_LENS[k] indices */
    uint32_t   *ngram_idx[QTC_N_LEVELS];    /* flat array of indices */
    uint32_t    ngram_count[QTC_N_LEVELS];   /* number of n-grams */
} qtc_codebook_t;

/* ── Compressed file structure ─────────────────────── */
typedef struct {
    uint64_t    original_size;
    uint64_t    lowered_size;
    uint32_t    n_words;
    uint8_t     flags;
    uint32_t    n_tokens;
    uint32_t    payload_size;
    uint32_t    case_size;
    uint32_t    codebook_size;
    uint32_t    escape_size;
} qtc_header_t;

/* ── Public API ────────────────────────────────────── */

/* Compress data, returns malloc'd buffer. Caller must free(). */
uint8_t *qtc_compress(const uint8_t *data, size_t len, size_t *out_len, bool verbose);

/* Decompress data, returns malloc'd buffer. Caller must free(). */
uint8_t *qtc_decompress(const uint8_t *data, size_t len, size_t *out_len, bool verbose);

#endif /* QTC_H */
