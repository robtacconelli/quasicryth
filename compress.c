/*
 * QTC Multi-Tiling - Compress implementation
 * Uses 64 aperiodic tilings for increased n-gram coverage.
 * Greedy non-overlapping selection, sequential AC encoding.
 */
#include "qtc.h"
#include "ht.h"
#include "ac.h"
#include "fib.h"
#include "tok.h"
#include "cb.h"
#include "md5.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <zlib.h>
#include <bzlib.h>
#include <lzma.h>

int qtc_tiling_mode = QTC_TILING_MULTI;  /* default: all 18 tilings */

/* ── Two-tier unigram split ── */
#define UNI_TIER_SPLIT 4096

/* ── Recency cache (simplified MTF) for index encoding ── */
#define RCACHE_SIZE 64
typedef struct {
    uint32_t entries[RCACHE_SIZE];
    uint32_t count;  /* how many valid entries (starts at 0, grows to RCACHE_SIZE) */
} rcache_t;

static void rcache_init(rcache_t *c) { c->count = 0; }

/* Returns position if found (0..count-1), or -1 if not found */
static int rcache_find(rcache_t *c, uint32_t val) {
    for (uint32_t i = 0; i < c->count; i++)
        if (c->entries[i] == val) return (int)i;
    return -1;
}

/* Move val to front (position 0), shifting others down */
static void rcache_use(rcache_t *c, uint32_t val) {
    int pos = rcache_find(c, val);
    if (pos >= 0) {
        memmove(&c->entries[1], &c->entries[0], (uint32_t)pos * sizeof(uint32_t));
    } else {
        uint32_t shift = (c->count < RCACHE_SIZE) ? c->count : RCACHE_SIZE - 1;
        memmove(&c->entries[1], &c->entries[0], shift * sizeof(uint32_t));
        if (c->count < RCACHE_SIZE) c->count++;
    }
    c->entries[0] = val;
}

/* ══════════════════════════════════════════════════════
 * Collect deep matches from one tiling into position-indexed arrays.
 * deep_best_lv[wpos] = best (deepest) encoding level found so far (0 = none).
 * deep_best_idx[wpos] = codebook index for that level.
 * No bigrams — those are handled by a simple linear scan later.
 * ══════════════════════════════════════════════════════ */
uint32_t collect_deep_from_tiling(
    const qtc_tile_t *tiles, uint32_t n_tiles,
    const qtc_hierarchy_t *hier, const qtc_cbs_t *cbs,
    uint32_t nw,
    uint8_t *deep_best_lv, uint32_t *deep_best_idx)
{
    int max_k = hier->n_levels - 1;
    if (max_k > QTC_MAX_HIER) max_k = QTC_MAX_HIER;
    uint32_t hits = 0;

    for (uint32_t ti = 0; ti < n_tiles; ti++) {
        if (!tiles[ti].is_L) continue;
        uint32_t wpos = tiles[ti].wpos;

        /* Walk up hierarchy, track deepest codebook match */
        int best_k = -1;
        uint32_t best_cb_idx = 0;
        uint32_t idx_k = ti;

        for (int k = 0; k < max_k; k++) {
            qtc_pmap_t *pm = hier->parent_maps[k];
            if (!pm || idx_k >= hier->level_count[k]) break;
            if (pm[idx_k].parent_idx < 0) break;
            if (pm[idx_k].pos != 0) break;

            uint32_t pidx = (uint32_t)pm[idx_k].parent_idx;
            if (!hier->levels[k + 1][pidx].is_L) break;

            int heir_k = k + 1;  /* hierarchy level */
            int gl = (heir_k <= QTC_N_LEVELS) ? QTC_HIER_WORD_LENS[heir_k] : -1;

            if (gl > 0 && wpos + (uint32_t)gl <= nw) {
                /* Verify word coverage matches expected Fibonacci length */
                uint32_t st = hier->levels[k + 1][pidx].start;
                uint32_t en = hier->levels[k + 1][pidx].end;
                uint32_t nw_cov = 0;
                for (uint32_t j = st; j < en; j++) nw_cov += tiles[j].nwords;

                if (nw_cov == (uint32_t)gl) {
                    if (k == 0 && (ti + 1 >= n_tiles || tiles[ti + 1].is_L)) {
                        idx_k = pidx;
                        continue;
                    }
                    /* Try codebook lookup */
                    if (heir_k <= QTC_N_LEVELS && cbs->ng_count[heir_k - 1] > 0) {
                        uint32_t cb_idx;
                        if (nmap_get(&cbs->ng_maps[heir_k - 1], wpos, &cb_idx)) {
                            best_k = heir_k;
                            best_cb_idx = cb_idx;
                        }
                    }
                }
            }
            idx_k = pidx;
        }

        if (best_k >= 1) {
            uint8_t enc_level = (uint8_t)(best_k + 2);
            /* Keep deepest match per position across all tilings */
            if (enc_level > deep_best_lv[wpos]) {
                deep_best_lv[wpos] = enc_level;
                deep_best_idx[wpos] = best_cb_idx;
                hits++;
            }
        }
    }
    return hits;
}

/* ══════════════════════════════════════════════════════
 * qtc_compress — Multi-tiling version
 * ══════════════════════════════════════════════════════ */
uint8_t *qtc_compress(const uint8_t *data, size_t len, size_t *out_len, bool verbose) {
    uint64_t n = (uint64_t)len;

    if (verbose) fprintf(stderr, "\n  QTC-Multi v%s -- %llu bytes\n", QTC_VERSION, (unsigned long long)n);

    /* ── Case separation ─────────────────────────── */
    uint8_t *lowered = NULL;
    uint64_t lowered_len = 0;
    qtc_token_t *tokens = NULL;
    uint32_t n_tok = 0;
    uint8_t *case_enc = NULL;
    uint32_t case_enc_len = 0;
    bool is_binary = false;

    uint64_t check_len = n < 8192 ? n : 8192;
    for (uint64_t i = 0; i < check_len; i++) {
        if (data[i] == 0) { is_binary = true; break; }
    }

    if (!is_binary) {
        n_tok = tokenize(data, n, &lowered, &lowered_len, &tokens);
        uint8_t *case_flags = (uint8_t *)malloc(n_tok);
        for (uint32_t i = 0; i < n_tok; i++) case_flags[i] = tokens[i].case_flag;
        case_enc = enc_case(case_flags, n_tok, &case_enc_len);
        free(case_flags);
        if (verbose)
            fprintf(stderr, "  [1] Case: %u tokens -> %uB\n", n_tok, case_enc_len);
    } else {
        lowered = (uint8_t *)malloc((size_t)n);
        memcpy(lowered, data, (size_t)n);
        lowered_len = n;
        if (verbose) fprintf(stderr, "  [1] Binary mode\n");
    }
    free(tokens);

    /* ── Word split ──────────────────────────────── */
    const uint8_t **word_ptrs;
    uint16_t *word_lens;
    uint32_t nw = word_split(lowered, lowered_len, &word_ptrs, &word_lens);
    if (verbose) fprintf(stderr, "  [2] Words: %u tokens\n", nw);

    /* ── Build codebooks ─────────────────────────── */
    qtc_cb_sizes_t sizes = auto_codebook_sizes(nw);
    qtc_cbs_t cbs;
    cbs_build(&cbs, word_ptrs, word_lens, nw, &sizes);
    free(word_ptrs);
    free(word_lens);

    /* Free lowered data early — all word bytes are now in cbs.pool */
    free(lowered);
    lowered = NULL;

    uint32_t cb_raw_len;
    uint8_t *cb_raw = cbs_encode(&cbs, &cb_raw_len);
    size_t cb_comp_len = lzma_stream_buffer_bound(cb_raw_len);
    uint8_t *cb_data = (uint8_t *)malloc(cb_comp_len);
    {
        size_t out_pos = 0;
        lzma_easy_buffer_encode(9 | LZMA_PRESET_EXTREME, LZMA_CHECK_NONE,
                                NULL, cb_raw, cb_raw_len,
                                cb_data, &out_pos, cb_comp_len);
        cb_comp_len = out_pos;
    }
    free(cb_raw);

    /* Free intern bmap early — no longer needed after encoding codebook.
     * Saves ~3.5GB for large files (intern hash table is way oversized). */
    cbs_free_intern(&cbs);

    if (verbose) {
        fprintf(stderr, "  [3] Codebook: %u uni, %u bi", cbs.n_uni, cbs.n_bi);
        static const char *ng_names[] = {"tri","5g","8g","13g","21g","34g","55g","89g","144g"};
        for (int lv = 0; lv < QTC_N_LEVELS; lv++)
            if (cbs.ng_count[lv]) fprintf(stderr, ", %u %s", cbs.ng_count[lv], ng_names[lv]);
        fprintf(stderr, " -> %luB (unique words: %u)\n", (unsigned long)cb_comp_len, cbs.n_unique);
    }

    const uint32_t *wids = cbs.word_ids;

    /* ── Collect deep matches from tilings ────────── */
    /* Position-indexed: deep_best_lv[wp] = deepest encoding level (3..11), 0 = none.
     * Fixed memory: nw bytes + nw*4 bytes, vs old approach that accumulated
     * billions of (wpos, cb_idx) entries into growable lists. */
    uint8_t  *deep_best_lv  = (uint8_t  *)calloc(nw, sizeof(uint8_t));
    uint32_t *deep_best_idx = (uint32_t *)calloc(nw, sizeof(uint32_t));

    int n_tilings;
    if (qtc_tiling_mode == QTC_TILING_NONE)       n_tilings = 0;
    else if (qtc_tiling_mode == QTC_TILING_PERIOD5) n_tilings = 1; /* single Period-5 tiling */
    else if (qtc_tiling_mode == QTC_TILING_FIB)   n_tilings = QTC_N_TILINGS_FIB;
    else                                           n_tilings = QTC_N_TILINGS;

    qtm_tiling_desc_t descs[QTC_N_TILINGS];
    if (qtc_tiling_mode == QTC_TILING_MULTI || qtc_tiling_mode == QTC_TILING_FIB)
        qtm_get_tiling_descs(descs);

    uint32_t total_deep_hits = 0;
    uint32_t deep_pos_after[QTC_N_TILINGS + 1]; /* cumulative deep positions after each tiling */
    memset(deep_pos_after, 0, sizeof(deep_pos_after));
    uint32_t greedy_lv_after[QTC_N_TILINGS + 1][QTM_N_ENC_LEVELS]; /* greedy level dist after each tiling */
    memset(greedy_lv_after, 0, sizeof(greedy_lv_after));
    for (int t = 0; t < n_tilings; t++) {
        uint32_t nt;
        qtc_tile_t *tiles;
        if (qtc_tiling_mode == QTC_TILING_PERIOD5)
            tiles = gen_period5_tiles(nw, &nt);
        else
            tiles = qtm_gen_tiling(&descs[t], nw, &nt);
        if (!tiles) continue;

        qtc_hierarchy_t hier;
        build_hierarchy(tiles, nt, QTC_MAX_HIER, &hier);

        total_deep_hits += collect_deep_from_tiling(
            tiles, nt, &hier, &cbs, nw, deep_best_lv, deep_best_idx);

        free_hierarchy(&hier);
        free(tiles);

        /* Count cumulative deep positions and greedy level distribution after this tiling */
        uint32_t dp = 0;
        for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++) greedy_lv_after[t + 1][lv] = 0;
        for (uint32_t i = 0; i < nw; i++) {
            if (deep_best_lv[i] >= 3) dp++;
            greedy_lv_after[t + 1][deep_best_lv[i]]++;
        }
        deep_pos_after[t + 1] = dp;
    }

    if (verbose) {
        uint32_t final_deep = (n_tilings > 0) ? deep_pos_after[n_tilings] : 0;
        const char *mode_name = (qtc_tiling_mode == QTC_TILING_NONE) ? "No-tiling" :
                               (qtc_tiling_mode == QTC_TILING_PERIOD5) ? "Period-5" :
                               (qtc_tiling_mode == QTC_TILING_FIB) ? "Fibonacci-only" : "Multi-tiling";
        fprintf(stderr, "  [4] %s: %d tilings, %u deep positions\n",
                mode_name, n_tilings, final_deep);
        /* Per-tiling breakdown using actual descriptor names */
        if (n_tilings > 1 && qtc_tiling_mode != QTC_TILING_PERIOD5) {
            uint32_t golden_dp = deep_pos_after[n_tilings <= 12 ? n_tilings : 12];
            fprintf(stderr, "      Golden-ratio (1/φ, %d tilings): %u deep positions\n",
                    n_tilings < 12 ? n_tilings : 12, golden_dp);
            /* Group remaining tilings by name (pairs share a name) */
            for (int t = 12; t < n_tilings; ) {
                const char *name = descs[t].name;
                int group_start = t;
                while (t < n_tilings && strcmp(descs[t].name, name) == 0) t++;
                int count = t - group_start;
                uint32_t delta = deep_pos_after[t] - deep_pos_after[group_start];
                fprintf(stderr, "      + %s (%d tiling%s):  %*s+%u (total %u)\n",
                        name, count, count > 1 ? "s" : "",
                        (int)(20 - strlen(name)), "",
                        delta, deep_pos_after[t]);
            }
            /* Greedy level distribution after each tiling group */
            static const char *lvnames[] = {"esc","uni","bi","tri","5g","8g","13g","21g","34g","55g","89g","144g"};
            fprintf(stderr, "      --- Greedy level distribution (after each group) ---\n");
            fprintf(stderr, "      %-14s", "Group");
            for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++)
                fprintf(stderr, " %10s", lvnames[lv]);
            fprintf(stderr, "\n");
            /* Golden */
            int gend = n_tilings <= 12 ? n_tilings : 12;
            fprintf(stderr, "      %-14s", "Golden");
            for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++)
                fprintf(stderr, " %10u", greedy_lv_after[gend][lv]);
            fprintf(stderr, "\n");
            /* Non-golden groups */
            for (int t = 12; t < n_tilings; ) {
                const char *name = descs[t].name;
                int gs = t;
                while (t < n_tilings && strcmp(descs[t].name, name) == 0) t++;
                fprintf(stderr, "      +%-13s", name);
                for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++) {
                    int32_t d = (int32_t)greedy_lv_after[t][lv] - (int32_t)greedy_lv_after[gs][lv];
                    fprintf(stderr, " %+10d", d);
                }
                fprintf(stderr, "\n");
            }
        }
    }

    /* ── Greedy non-overlapping selection ─────────── */
    /* result_level[wpos]: 0=escape, 1=unigram, 2=bigram, 3..11=deep */
    uint8_t  *result_level = (uint8_t  *)calloc(nw, sizeof(uint8_t));
    uint32_t *result_idx   = (uint32_t *)calloc(nw, sizeof(uint32_t));
    bool     *covered      = (bool     *)calloc(nw, sizeof(bool));

    /* Pass 1: deep levels (11 down to 3) — deepest wins */
    uint32_t selected_counts[QTM_N_ENC_LEVELS];
    memset(selected_counts, 0, sizeof(selected_counts));

    for (int lv = QTM_N_ENC_LEVELS - 1; lv >= 3; lv--) {
        int nwords = QTM_LEVEL_WORDS[lv];
        for (uint32_t wp = 0; wp < nw; wp++) {
            if (deep_best_lv[wp] != (uint8_t)lv) continue;
            if (wp + (uint32_t)nwords > nw) continue;

            /* Check no overlap */
            bool ok = true;
            for (int j = 0; j < nwords; j++) {
                if (covered[wp + (uint32_t)j]) { ok = false; break; }
            }
            if (!ok) continue;

            result_level[wp] = (uint8_t)lv;
            result_idx[wp]   = deep_best_idx[wp];
            for (int j = 0; j < nwords; j++) covered[wp + (uint32_t)j] = true;
            selected_counts[lv]++;
        }
    }

    free(deep_best_lv);
    free(deep_best_idx);

    /* Pass 2: bigrams — scan all uncovered consecutive pairs (skip in NONE mode) */
    if (qtc_tiling_mode != QTC_TILING_NONE) {
        for (uint32_t wp = 0; wp + 1 < nw; wp++) {
            if (covered[wp] || covered[wp + 1]) continue;
            uint32_t bi_idx;
            if (u64map_get(&cbs.bi_map, pack_bi(wids[wp], wids[wp + 1]), &bi_idx)) {
                result_level[wp] = 2;
                result_idx[wp]   = bi_idx;
                covered[wp] = true;
                covered[wp + 1] = true;
                selected_counts[2]++;
            }
        }
    }

    /* Pass 3: unigrams and escapes */
    uint32_t esc_count = 0;
    for (uint32_t wp = 0; wp < nw; wp++) {
        if (covered[wp]) continue;
        int32_t uidx = (wids[wp] < cbs.uni_rmap_sz) ? cbs.uni_rmap[wids[wp]] : -1;
        if (uidx >= 0) {
            result_level[wp] = 1;
            result_idx[wp]   = (uint32_t)uidx;
            selected_counts[1]++;
        } else {
            result_level[wp] = 0;
            esc_count++;
        }
        covered[wp] = true;
    }

    if (verbose) {
        fprintf(stderr, "  [5] Greedy selection:\n");
        static const char *lv_names[] = {
            "escape","unigram","bigram","trigram","5-gram","8-gram",
            "13-gram","21-gram","34-gram","55-gram","89-gram","144-gram"
        };
        for (int lv = QTM_N_ENC_LEVELS - 1; lv >= 0; lv--)
            if (selected_counts[lv])
                fprintf(stderr, "      %8s: %u\n", lv_names[lv], selected_counts[lv]);
        fprintf(stderr, "      escapes: %u\n", esc_count);
    }

    free(covered);

    /* ── Word-level LZ77 on the event stream ───────── */
    /* Linearize events: walk result_level/result_idx by word position,
     * building arrays indexed by EVENT number. */
    uint32_t n_events = 0;
    {
        uint32_t twp = 0;
        while (twp < nw) {
            uint8_t lv = result_level[twp];
            n_events++;
            twp += QTM_LEVEL_WORDS[lv];
        }
    }

    uint8_t  *evt_level = (uint8_t  *)malloc(n_events * sizeof(uint8_t));
    uint32_t *evt_idx   = (uint32_t *)malloc(n_events * sizeof(uint32_t));
    uint32_t *evt_wp    = (uint32_t *)malloc(n_events * sizeof(uint32_t));
    {
        uint32_t twp = 0, ei = 0;
        while (twp < nw) {
            uint8_t lv = result_level[twp];
            evt_level[ei] = lv;
            evt_idx[ei]   = result_idx[twp];
            evt_wp[ei]    = twp;
            ei++;
            twp += QTM_LEVEL_WORDS[lv];
        }
    }

    /* LZ77 hash table with 4-entry chains for better match finding */
    #define LZ_HASH_BITS 22
    #define LZ_HASH_SIZE (1u << LZ_HASH_BITS)
    #define LZ_MIN_MATCH 3
    #define LZ_MAX_MATCH 255
    #define LZ_CHAIN_LEN 4

    uint32_t *lz_ht = (uint32_t *)malloc(LZ_HASH_SIZE * LZ_CHAIN_LEN * sizeof(uint32_t));
    memset(lz_ht, 0xFF, LZ_HASH_SIZE * LZ_CHAIN_LEN * sizeof(uint32_t));

    uint8_t  *is_match  = (uint8_t  *)calloc(n_events, sizeof(uint8_t));
    uint32_t *match_off = (uint32_t *)calloc(n_events, sizeof(uint32_t));
    uint16_t *match_len = (uint16_t *)calloc(n_events, sizeof(uint16_t));

    #define LZ_HASH3(lv, idx, pos) ({ \
        uint32_t _h = (lv)[(pos)] * 2654435761u; \
        _h ^= (idx)[(pos)] * 2246822519u; \
        _h ^= (lv)[(pos)+1] * 3266489917u; \
        _h ^= (idx)[(pos)+1] * 668265263u; \
        _h ^= (lv)[(pos)+2] * 374761393u; \
        _h ^= (idx)[(pos)+2] * 2246822519u; \
        _h >> (32 - LZ_HASH_BITS); \
    })

    uint32_t lz_matches = 0;
    uint32_t lz_events_saved = 0;
    for (uint32_t ei = 0; ei + LZ_MIN_MATCH <= n_events; ei++) {
        if (is_match[ei]) continue;

        uint32_t h = LZ_HASH3(evt_level, evt_idx, ei);

        /* Search chain entries for the best (longest) match, early-exit if good enough */
        uint32_t best_ref = 0xFFFFFFFF;
        uint32_t best_mlen = 0;
        for (int ci = 0; ci < LZ_CHAIN_LEN; ci++) {
            uint32_t ref = lz_ht[h * LZ_CHAIN_LEN + ci];
            if (ref == 0xFFFFFFFF || ref >= ei) continue;
            bool prefix_ok = true;
            for (int j = 0; j < LZ_MIN_MATCH; j++) {
                if (evt_level[ref + (uint32_t)j] != evt_level[ei + (uint32_t)j] ||
                    evt_idx[ref + (uint32_t)j]   != evt_idx[ei + (uint32_t)j]) {
                    prefix_ok = false; break;
                }
            }
            if (!prefix_ok) continue;
            uint32_t mlen = LZ_MIN_MATCH;
            uint32_t max_ext = n_events - ei;
            if (max_ext > LZ_MAX_MATCH) max_ext = LZ_MAX_MATCH;
            uint32_t max_no_overlap = ei - ref;
            if (max_ext > max_no_overlap) max_ext = max_no_overlap;
            while (mlen < max_ext &&
                   evt_level[ref + mlen] == evt_level[ei + mlen] &&
                   evt_idx[ref + mlen]   == evt_idx[ei + mlen]) mlen++;
            if (mlen > best_mlen) { best_mlen = mlen; best_ref = ref; }
            if (best_mlen >= 16) break;  /* good enough — skip remaining chains */
        }

        if (best_mlen >= LZ_MIN_MATCH) {
            is_match[ei]  = 1;
            match_off[ei] = ei - best_ref;
            match_len[ei] = (uint16_t)best_mlen;
            lz_matches++;
            lz_events_saved += best_mlen;
            for (uint32_t j = 1; j < best_mlen; j++) is_match[ei + j] = 2;
            memmove(&lz_ht[h * LZ_CHAIN_LEN + 1], &lz_ht[h * LZ_CHAIN_LEN],
                    (LZ_CHAIN_LEN - 1) * sizeof(uint32_t));
            lz_ht[h * LZ_CHAIN_LEN] = ei;
            ei += best_mlen - 1;
            continue;
        }

        memmove(&lz_ht[h * LZ_CHAIN_LEN + 1], &lz_ht[h * LZ_CHAIN_LEN],
                (LZ_CHAIN_LEN - 1) * sizeof(uint32_t));
        lz_ht[h * LZ_CHAIN_LEN] = ei;
    }

    free(lz_ht);

    if (verbose) {
        fprintf(stderr, "  [5b] LZ77 events: %u total, %u matches, %u events saved\n",
                n_events, lz_matches, lz_events_saved);
    }

    /* ── Sequential AC encoding (variable-alphabet) ── */
    /* Order-2 context-conditioned level model: 12x12 models indexed by (prev_lv, prev_prev_lv) */
    qtc_vmodel_t level_models[QTM_N_ENC_LEVELS][QTM_N_ENC_LEVELS];
    for (int i = 0; i < QTM_N_ENC_LEVELS; i++)
        for (int j = 0; j < QTM_N_ENC_LEVELS; j++)
            vmodel_init(&level_models[i][j], QTM_N_ENC_LEVELS);

    /* Per-level index models conditioned on previous level */
    qtc_vmodel_t vidx[QTM_N_ENC_LEVELS][QTM_N_ENC_LEVELS];
    memset(vidx, 0, sizeof(vidx));
    /* Two-tier unigram: common (0..4095) and rare (4096..n_uni-1) */
    qtc_vmodel_t uni_tier_model[QTM_N_ENC_LEVELS];
    qtc_vmodel_t vidx_rare[QTM_N_ENC_LEVELS];
    bool use_uni_tier = (cbs.n_uni > UNI_TIER_SPLIT);
    for (int ctx = 0; ctx < QTM_N_ENC_LEVELS; ctx++) {
        uint32_t uni_sz = use_uni_tier ? UNI_TIER_SPLIT : (cbs.n_uni > 0 ? cbs.n_uni : 1);
        vmodel_init(&vidx[1][ctx], uni_sz);
        vmodel_init(&vidx[2][ctx], cbs.n_bi > 0 ? cbs.n_bi : 1);
        for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++) {
            uint32_t cnt = cbs.ng_count[lv - 3];
            vmodel_init(&vidx[lv][ctx], cnt > 0 ? cnt : 1);
        }
        vmodel_init(&uni_tier_model[ctx], 2);
        vmodel_init(&vidx_rare[ctx], use_uni_tier ? (cbs.n_uni - UNI_TIER_SPLIT) : 1);
    }

    /* Recency cache: per-level cache + hit/miss and position models */
    rcache_t rcaches[QTM_N_ENC_LEVELS];
    qtc_vmodel_t cache_hit_model[QTM_N_ENC_LEVELS];
    qtc_vmodel_t cache_pos_model[QTM_N_ENC_LEVELS];
    for (int i = 0; i < QTM_N_ENC_LEVELS; i++) {
        rcache_init(&rcaches[i]);
        vmodel_init(&cache_hit_model[i], 2);        /* 0=hit, 1=miss */
        vmodel_init(&cache_pos_model[i], RCACHE_SIZE);
    }

    /* LZ match models */
    qtc_vmodel_t match_flag_model;
    vmodel_init(&match_flag_model, 2);       /* 0=normal, 1=match */
    qtc_vmodel_t match_off_nbits_model;
    vmodel_init(&match_off_nbits_model, 32);    /* 1..32 bits, encoded as 0..31 */
    qtc_vmodel_t match_off_bit_model[8];
    for (int _i = 0; _i < 8; _i++) vmodel_init(&match_off_bit_model[_i], 2);
    qtc_vmodel_t match_len_model;
    vmodel_init(&match_len_model, 253);      /* match_len - 3, max 255-3=252 */

    qtc_encoder_t encoder;
    enc_init(&encoder);
    qtc_buf_t esc_buf;
    buf_init(&esc_buf, 4096);

    uint8_t prev_lv = 1;       /* initial context: unigram */
    uint8_t prev_prev_lv = 1;  /* initial context: unigram */
    uint32_t ei = 0;
    while (ei < n_events) {
        if (is_match[ei] == 1) {
            /* Encode match flag = 1 (match) */
            venc_sym(&encoder, &match_flag_model, 1);

            /* Encode offset with log-scale (Elias gamma-like) */
            uint32_t off = match_off[ei];
            uint32_t nbits = 1;
            { uint32_t tmp = off >> 1; while (tmp) { nbits++; tmp >>= 1; } }
            venc_sym(&encoder, &match_off_nbits_model, nbits - 1);
            for (int b = (int)nbits - 2; b >= 0; b--)
                venc_sym(&encoder, &match_off_bit_model[b < 8 ? b : 7], (off >> b) & 1);

            /* Encode length - 3 */
            venc_sym(&encoder, &match_len_model, (uint32_t)(match_len[ei] - LZ_MIN_MATCH));

            /* Advance: for each matched event, we still need to
             * collect escapes and update recency caches so decoder
             * can replay them identically. */
            uint32_t mlen = match_len[ei];
            for (uint32_t mi = 0; mi < mlen; mi++) {
                uint32_t cur = ei + mi;
                uint8_t lv = evt_level[cur];
                uint32_t cwp = evt_wp[cur];

                if (lv == 0) {
                    /* Escape words must still go to escape buffer */
                    uint32_t wid = wids[cwp];
                    uint16_t wlen = cbs.pool_lens[wid];
                    buf_write16(&esc_buf, wlen);
                    buf_append(&esc_buf, cbs.pool + cbs.pool_offs[wid], wlen);
                } else {
                    /* Update recency cache for this event */
                    rcache_use(&rcaches[lv], evt_idx[cur]);
                }

                /* Update level context */
                prev_prev_lv = prev_lv;
                prev_lv = lv;
            }
            ei += mlen;
        } else {
            /* Encode match flag = 0 (normal) */
            venc_sym(&encoder, &match_flag_model, 0);

            uint8_t lv = evt_level[ei];
            uint32_t cwp = evt_wp[ei];
            uint8_t ctx_lv = prev_lv;

            venc_sym(&encoder, &level_models[prev_lv][prev_prev_lv], (uint32_t)lv);
            prev_prev_lv = prev_lv;
            prev_lv = lv;

            switch (lv) {
            case 0: {
                /* Escape: store word bytes in escape buffer */
                uint32_t wid = wids[cwp];
                uint16_t wlen = cbs.pool_lens[wid];
                buf_write16(&esc_buf, wlen);
                buf_append(&esc_buf, cbs.pool + cbs.pool_offs[wid], wlen);
                break;
            }
            case 1:
            case 2:
            default: {
                /* Levels 1..11: encode index with recency cache, context-conditioned */
                uint32_t idx = evt_idx[ei];
                int cpos = rcache_find(&rcaches[lv], idx);
                if (cpos >= 0) {
                    venc_sym(&encoder, &cache_hit_model[lv], 0);
                    venc_sym(&encoder, &cache_pos_model[lv], (uint32_t)cpos);
                } else {
                    venc_sym(&encoder, &cache_hit_model[lv], 1);
                    if (lv == 1 && use_uni_tier) {
                        if (idx < UNI_TIER_SPLIT) {
                            venc_sym(&encoder, &uni_tier_model[ctx_lv], 0);
                            venc_sym(&encoder, &vidx[1][ctx_lv], idx);
                        } else {
                            venc_sym(&encoder, &uni_tier_model[ctx_lv], 1);
                            venc_sym(&encoder, &vidx_rare[ctx_lv], idx - UNI_TIER_SPLIT);
                        }
                    } else {
                        venc_sym(&encoder, &vidx[lv][ctx_lv], idx);
                    }
                }
                rcache_use(&rcaches[lv], idx);
                break;
            }
            }
            ei++;
        }
    }

    free(evt_level);
    free(evt_idx);
    free(evt_wp);
    free(is_match);
    free(match_off);
    free(match_len);

    uint32_t payload_len;
    uint8_t *payload = enc_finish(&encoder, &payload_len);

    /* LZMA compress escape data */
    uint8_t *esc_data = NULL;
    uint32_t esc_data_len = 0;
    if (esc_buf.len > 0) {
        size_t lzma_out_cap = lzma_stream_buffer_bound(esc_buf.len);
        esc_data = (uint8_t *)malloc(lzma_out_cap);
        size_t lzma_out_pos = 0;
        lzma_ret lr = lzma_easy_buffer_encode(
            9 | LZMA_PRESET_EXTREME, LZMA_CHECK_NONE,
            NULL, esc_buf.data, esc_buf.len,
            esc_data, &lzma_out_pos, lzma_out_cap);
        if (lr != LZMA_OK) {
            fprintf(stderr, "LZMA encode failed: %d\n", (int)lr);
        }
        esc_data_len = (uint32_t)lzma_out_pos;
    }

    if (verbose) {
        fprintf(stderr, "  [6] Payload: %uB\n", payload_len);
        fprintf(stderr, "  [7] Escapes: %u words, %uB raw -> %uB lzma\n",
                esc_count, esc_buf.len, esc_data_len);
    }

    /* ── Assemble output ─────────────────────────── */
    uint8_t flags = 0;
    if (is_binary) flags |= 0x02;
    if (qtc_tiling_mode == QTC_TILING_FIB) flags |= 0x04;

    size_t total_len = QTC_HEADER_SIZE + payload_len + case_enc_len +
                       cb_comp_len + esc_data_len + 16;
    uint8_t *out = (uint8_t *)malloc(total_len);
    size_t o = 0;

    /* Header: 45 bytes (orig_size and lowered_size are uint64) */
    memcpy(out + o, QTC_MAGIC, 4); o += 4;
    memcpy(out + o, &n, 8); o += 8;                /* uint64: original size */
    memcpy(out + o, &lowered_len, 8); o += 8;      /* uint64: lowered size */
    memcpy(out + o, &nw, 4); o += 4;
    out[o++] = flags;
    memcpy(out + o, &n_tok, 4); o += 4;
    memcpy(out + o, &payload_len, 4); o += 4;
    memcpy(out + o, &case_enc_len, 4); o += 4;
    uint32_t cb_len32 = (uint32_t)cb_comp_len;
    memcpy(out + o, &cb_len32, 4); o += 4;
    memcpy(out + o, &esc_data_len, 4); o += 4;

    /* Payload, case, codebook, escapes */
    memcpy(out + o, payload, payload_len); o += payload_len;
    if (case_enc_len) { memcpy(out + o, case_enc, case_enc_len); o += case_enc_len; }
    memcpy(out + o, cb_data, cb_comp_len); o += cb_comp_len;
    if (esc_data_len) { memcpy(out + o, esc_data, esc_data_len); o += esc_data_len; }

    /* MD5 checksum */
    uint8_t md5[16];
    md5_hash(data, (size_t)n, md5);
    memcpy(out + o, md5, 16); o += 16;

    *out_len = o;

    if (verbose) {
        double ratio = 100.0 * (double)o / (double)n;
        fprintf(stderr, "\n  ==================================================\n");
        fprintf(stderr, "  Original:   %10llu B\n", (unsigned long long)n);
        fprintf(stderr, "  Compressed: %10zu B (%.2f%%)\n", o, ratio);
        fprintf(stderr, "  --------------------------------------------------\n");
        fprintf(stderr, "  Payload:    %10u B  (sequential AC)\n", payload_len);
        fprintf(stderr, "  Escapes:    %10u B  (bz2)\n", esc_data_len);
        fprintf(stderr, "  Codebook:   %10lu B\n", (unsigned long)cb_comp_len);
        fprintf(stderr, "  Case (AC):  %10u B\n", case_enc_len);
        fprintf(stderr, "  ==================================================\n");
    }

    /* Cleanup */
    free(payload);
    free(esc_data);
    buf_free(&esc_buf);
    free(result_level);
    free(result_idx);
    for (int i = 0; i < QTM_N_ENC_LEVELS; i++) {
        for (int j = 0; j < QTM_N_ENC_LEVELS; j++) {
            vmodel_free(&level_models[i][j]);
            vmodel_free(&vidx[i][j]);
        }
        vmodel_free(&cache_hit_model[i]);
        vmodel_free(&cache_pos_model[i]);
        vmodel_free(&uni_tier_model[i]);
        vmodel_free(&vidx_rare[i]);
    }
    vmodel_free(&match_flag_model);
    vmodel_free(&match_off_nbits_model);
    for (int _i = 0; _i < 8; _i++) vmodel_free(&match_off_bit_model[_i]);
    vmodel_free(&match_len_model);
    cbs_free(&cbs);
    free(cb_data);
    free(case_enc);

    return out;
}
