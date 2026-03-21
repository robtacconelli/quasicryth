/*
 * QTC - Fibonacci quasicrystal tiling, hierarchy, deep positions
 * Extended for multi-tiling: 64 aperiodic structures
 */
#include "fib.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const double INV_PHI = 1.0 / ((1.0 + sqrt(5.0)) / 2.0);

/* Forward declarations for special tiling generators */
qtc_tile_t *gen_random_qc_tiles(uint32_t n_words, uint32_t seed, uint32_t *out_n_tiles);

/* Noble-5: CF = [0; 1,1,1,1,2, 1,1,1,...] = 62*(99-sqrt(5))/9796
 * Hierarchy reaches level 5 (21-gram) before collapsing. */
static double noble5_alpha(void) {
    return 62.0 * (99.0 - sqrt(5.0)) / 9796.0;
}

/* ══════════════════════════════════════════════════════
 * Common: convert raw L/S symbols to tiles
 * Fixes adjacent-S violations (merge SS → L)
 * ══════════════════════════════════════════════════════ */
static qtc_tile_t *symbols_to_tiles(bool *symbols, uint32_t n_sym, uint32_t *out_n_tiles) {
    /* Fix adjacent S violations */
    bool *fixed = (bool *)malloc((n_sym + 1) * sizeof(bool));
    uint32_t n_fixed = 0;
    uint32_t i = 0;
    while (i < n_sym) {
        if (i + 1 < n_sym && !symbols[i] && !symbols[i + 1]) {
            fixed[n_fixed++] = true;  /* merge 2 S into 1 L */
            i += 2;
        } else {
            fixed[n_fixed++] = symbols[i];
            i++;
        }
    }

    /* Convert to tiles */
    qtc_tile_t *tiles = (qtc_tile_t *)malloc(n_fixed * sizeof(qtc_tile_t));
    uint32_t wpos = 0;
    for (uint32_t j = 0; j < n_fixed; j++) {
        tiles[j].wpos = wpos;
        tiles[j].nwords = fixed[j] ? 2 : 1;
        tiles[j].is_L = fixed[j];
        wpos += tiles[j].nwords;
    }
    free(fixed);

    *out_n_tiles = n_fixed;
    return tiles;
}

/* Generate raw L/S symbols from cut-and-project with given alpha */
static bool *gen_cap_symbols(uint32_t n_words, double alpha, double phase,
                              uint32_t *out_n_sym) {
    uint32_t sym_cap = n_words + 16;
    bool *symbols = (bool *)malloc(sym_cap * sizeof(bool));
    uint32_t n_sym = 0;
    uint32_t total = 0;
    uint32_t k = 0;

    while (total < n_words) {
        bool is_L = (int)floor((k + 1) * alpha + phase) -
                    (int)floor(k * alpha + phase) == 1;
        uint32_t consume = is_L ? 2 : 1;
        if (total + consume > n_words) {
            if (is_L && total + 1 <= n_words) {
                if (n_sym >= sym_cap) { sym_cap *= 2; symbols = realloc(symbols, sym_cap * sizeof(bool)); }
                symbols[n_sym++] = false;
                total += 1;
            }
            break;
        }
        if (n_sym >= sym_cap) { sym_cap *= 2; symbols = realloc(symbols, sym_cap * sizeof(bool)); }
        symbols[n_sym++] = is_L;
        total += consume;
        k++;
    }
    while (total < n_words) {
        if (n_sym >= sym_cap) { sym_cap *= 2; symbols = realloc(symbols, sym_cap * sizeof(bool)); }
        symbols[n_sym++] = false;
        total++;
    }

    *out_n_sym = n_sym;
    return symbols;
}

/* ══════════════════════════════════════════════════════
 * QC Word Tiling — original Fibonacci (alpha = 1/phi)
 * ══════════════════════════════════════════════════════ */
qtc_tile_t *qc_word_tiling(uint32_t n_words, double phase, uint32_t *out_n_tiles) {
    return qc_word_tiling_alpha(n_words, INV_PHI, phase, out_n_tiles);
}

/* ══════════════════════════════════════════════════════
 * General cut-and-project tiling
 * ══════════════════════════════════════════════════════ */
qtc_tile_t *qc_word_tiling_alpha(uint32_t n_words, double alpha, double phase,
                                  uint32_t *out_n_tiles) {
    uint32_t n_sym;
    bool *symbols = gen_cap_symbols(n_words, alpha, phase, &n_sym);
    qtc_tile_t *tiles = symbols_to_tiles(symbols, n_sym, out_n_tiles);
    free(symbols);
    return tiles;
}

/* ══════════════════════════════════════════════════════
 * Thue-Morse tiling: T(k) = popcount(k) mod 2
 * ══════════════════════════════════════════════════════ */
qtc_tile_t *gen_thue_morse_tiles(uint32_t n_words, uint32_t *out_n_tiles) {
    uint32_t sym_cap = n_words + 16;
    bool *symbols = (bool *)malloc(sym_cap * sizeof(bool));
    uint32_t n_sym = 0, total = 0, k = 0;

    while (total < n_words) {
        bool is_L = (__builtin_popcount(k) & 1) != 0;
        uint32_t consume = is_L ? 2 : 1;
        if (total + consume > n_words) {
            symbols[n_sym++] = false;
            total++;
            break;
        }
        if (n_sym >= sym_cap) { sym_cap *= 2; symbols = realloc(symbols, sym_cap * sizeof(bool)); }
        symbols[n_sym++] = is_L;
        total += consume;
        k++;
    }
    while (total < n_words) {
        if (n_sym >= sym_cap) { sym_cap *= 2; symbols = realloc(symbols, sym_cap * sizeof(bool)); }
        symbols[n_sym++] = false;
        total++;
    }

    qtc_tile_t *tiles = symbols_to_tiles(symbols, n_sym, out_n_tiles);
    free(symbols);
    return tiles;
}

/* ══════════════════════════════════════════════════════
 * Rudin-Shapiro tiling
 * R(k) = number of "11" pairs in binary(k), mod 2
 * ══════════════════════════════════════════════════════ */
static int count_11_pairs(uint32_t k) {
    int count = 0;
    uint32_t prev = k & 1;
    k >>= 1;
    while (k) {
        uint32_t cur = k & 1;
        if (prev && cur) count++;
        prev = cur;
        k >>= 1;
    }
    return count;
}

qtc_tile_t *gen_rudin_shapiro_tiles(uint32_t n_words, uint32_t *out_n_tiles) {
    uint32_t sym_cap = n_words + 16;
    bool *symbols = (bool *)malloc(sym_cap * sizeof(bool));
    uint32_t n_sym = 0, total = 0, k = 0;

    while (total < n_words) {
        bool is_L = (count_11_pairs(k) & 1) != 0;
        uint32_t consume = is_L ? 2 : 1;
        if (total + consume > n_words) {
            symbols[n_sym++] = false;
            total++;
            break;
        }
        if (n_sym >= sym_cap) { sym_cap *= 2; symbols = realloc(symbols, sym_cap * sizeof(bool)); }
        symbols[n_sym++] = is_L;
        total += consume;
        k++;
    }
    while (total < n_words) {
        if (n_sym >= sym_cap) { sym_cap *= 2; symbols = realloc(symbols, sym_cap * sizeof(bool)); }
        symbols[n_sym++] = false;
        total++;
    }

    qtc_tile_t *tiles = symbols_to_tiles(symbols, n_sym, out_n_tiles);
    free(symbols);
    return tiles;
}

/* ══════════════════════════════════════════════════════
 * Period-doubling tiling: substitution 1→10, 0→11
 * Start with "1". Generate enough iterations, then read off.
 * ══════════════════════════════════════════════════════ */
qtc_tile_t *gen_period_doubling_tiles(uint32_t n_words, uint32_t *out_n_tiles) {
    /* We need enough symbols. Each iteration roughly doubles the length.
     * Start with "1" and iterate until we have enough symbols. */
    uint32_t need = n_words + 16;  /* upper bound on symbols needed */
    uint32_t cap = 4;
    uint8_t *seq = (uint8_t *)malloc(cap);
    seq[0] = 1;
    uint32_t seq_len = 1;

    while (seq_len < need) {
        uint32_t new_cap = seq_len * 2 + 4;
        if (new_cap < cap) new_cap = cap * 2;
        uint8_t *new_seq = (uint8_t *)malloc(new_cap);
        uint32_t ni = 0;
        for (uint32_t i = 0; i < seq_len && ni + 2 <= new_cap; i++) {
            if (seq[i] == 1) {
                new_seq[ni++] = 1;
                new_seq[ni++] = 0;
            } else {
                new_seq[ni++] = 1;
                new_seq[ni++] = 1;
            }
        }
        free(seq);
        seq = new_seq;
        seq_len = ni;
        cap = new_cap;
    }

    /* Convert to L/S symbols and tiles */
    uint32_t sym_cap = n_words + 16;
    bool *symbols = (bool *)malloc(sym_cap * sizeof(bool));
    uint32_t n_sym = 0, total = 0, k = 0;

    while (total < n_words && k < seq_len) {
        bool is_L = (seq[k] == 1);
        uint32_t consume = is_L ? 2 : 1;
        if (total + consume > n_words) {
            symbols[n_sym++] = false;
            total++;
            break;
        }
        if (n_sym >= sym_cap) { sym_cap *= 2; symbols = realloc(symbols, sym_cap * sizeof(bool)); }
        symbols[n_sym++] = is_L;
        total += consume;
        k++;
    }
    while (total < n_words) {
        if (n_sym >= sym_cap) { sym_cap *= 2; symbols = realloc(symbols, sym_cap * sizeof(bool)); }
        symbols[n_sym++] = false;
        total++;
    }
    free(seq);

    qtc_tile_t *tiles = symbols_to_tiles(symbols, n_sym, out_n_tiles);
    free(symbols);
    return tiles;
}

/* ══════════════════════════════════════════════════════
 * Optimized 18-tiling set
 *
 * Tier 1 (0-11):  12 golden-ratio phases — only tilings reaching 34g+/89g/144g
 *                  Phases spaced by golden ratio for maximal uniformity.
 * Tier 2 (12-13): 2 sqrt(58)-7 phases — closest quadratic irrational to 1/φ,
 *                  CF=[0;1,1,1,1,1,1,14,...], reaches 55g.
 * Tier 3 (14-15): 2 noble-5 phases — CF=[0;1,1,1,1,2,1̄], reaches 21g.
 * Tier 4 (16-17): 2 sqrt(13)-3 phases — broadest 3g-8g coverage,
 *                  CF=[0;1,1,1,1,6,...], L density 0.606 for diverse positions.
 * ══════════════════════════════════════════════════════ */
void qtm_get_tiling_descs(qtm_tiling_desc_t descs[QTC_N_TILINGS]) {
    double phi_inv = INV_PHI;                       /* 0.6180339887... */
    double a_sqrt58 = sqrt(58.0) - 7.0;            /* 0.6157731059... */
    double a_noble5 = noble5_alpha();               /* 0.6124299495... */
    double a_sqrt13 = sqrt(13.0) - 3.0;            /* 0.6055512755... */

    /* Tier 1: 12 golden-ratio phases, golden-spaced */
    for (int i = 0; i < 12; i++) {
        descs[i].alpha = phi_inv;
        descs[i].phase = fmod(i * phi_inv, 1.0);
        descs[i].name = "golden";
    }

    /* Tier 2: 2 sqrt(58)-7 phases */
    descs[12].alpha = a_sqrt58; descs[12].phase = 0.0;  descs[12].name = "sqrt58";
    descs[13].alpha = a_sqrt58; descs[13].phase = 0.5;  descs[13].name = "sqrt58";

    /* Tier 3: 2 noble-5 phases */
    descs[14].alpha = a_noble5; descs[14].phase = 0.0;  descs[14].name = "noble5";
    descs[15].alpha = a_noble5; descs[15].phase = 0.5;  descs[15].name = "noble5";

    /* Tier 4: 2 sqrt(13)-3 phases */
    descs[16].alpha = a_sqrt13; descs[16].phase = 0.0;  descs[16].name = "sqrt13";
    descs[17].alpha = a_sqrt13; descs[17].phase = 0.5;  descs[17].name = "sqrt13";

    /* Tier 5: Optimized alphas from iterative greedy scan on enwik8.
     * Ranked by incremental compression gain when added to golden baseline.
     * Top pick 0.502 is far below golden — massive tri/5g volume upgrade.
     * Near-golden picks (0.617-0.622) upgrade 21g/34g/55g matches. */

    /* #1: Far-out alpha — huge tri/5g gain (196KB on enwik8) */
    descs[18].alpha = 0.502;  descs[18].phase = 0.0;  descs[18].name = "opt-0.502";
    descs[19].alpha = 0.502;  descs[19].phase = 0.5;  descs[19].name = "opt-0.502";

    /* #2-#5: Near-golden — 34g/55g/89g upgrades */
    descs[20].alpha = 0.6190; descs[20].phase = 0.0;  descs[20].name = "opt-0.619";
    descs[21].alpha = 0.6190; descs[21].phase = 0.5;  descs[21].name = "opt-0.619";

    descs[22].alpha = 0.6170; descs[22].phase = 0.0;  descs[22].name = "opt-0.617";
    descs[23].alpha = 0.6170; descs[23].phase = 0.5;  descs[23].name = "opt-0.617";

    descs[24].alpha = 0.6160; descs[24].phase = 0.0;  descs[24].name = "opt-0.616";
    descs[25].alpha = 0.6160; descs[25].phase = 0.5;  descs[25].name = "opt-0.616";

    descs[26].alpha = 0.6200; descs[26].phase = 0.0;  descs[26].name = "opt-0.620";
    descs[27].alpha = 0.6200; descs[27].phase = 0.5;  descs[27].name = "opt-0.620";

    /* #6-#10: Mid-value alphas — 8g/21g upgrades */
    descs[28].alpha = 0.6140; descs[28].phase = 0.0;  descs[28].name = "opt-0.614";
    descs[29].alpha = 0.6140; descs[29].phase = 0.5;  descs[29].name = "opt-0.614";

    descs[30].alpha = 0.6210; descs[30].phase = 0.0;  descs[30].name = "opt-0.621";
    descs[31].alpha = 0.6210; descs[31].phase = 0.5;  descs[31].name = "opt-0.621";

    descs[32].alpha = 0.6220; descs[32].phase = 0.0;  descs[32].name = "opt-0.622";
    descs[33].alpha = 0.6220; descs[33].phase = 0.5;  descs[33].name = "opt-0.622";

    descs[34].alpha = 0.6120; descs[34].phase = 0.0;  descs[34].name = "opt-0.612";
    descs[35].alpha = 0.6120; descs[35].phase = 0.5;  descs[35].name = "opt-0.612";
}

qtc_tile_t *qtm_gen_tiling(const qtm_tiling_desc_t *desc, uint32_t n_words,
                            uint32_t *out_n_tiles) {
    return qc_word_tiling_alpha(n_words, desc->alpha, desc->phase, out_n_tiles);
}

/* ══════════════════════════════════════════════════════
 * Verification
 * ══════════════════════════════════════════════════════ */
bool verify_no_adjacent_S(const qtc_tile_t *tiles, uint32_t n_tiles) {
    for (uint32_t i = 0; i + 1 < n_tiles; i++)
        if (!tiles[i].is_L && !tiles[i + 1].is_L) return false;
    return true;
}

/* ══════════════════════════════════════════════════════
 * Substitution Hierarchy
 * ══════════════════════════════════════════════════════ */
int build_hierarchy(const qtc_tile_t *tiles, uint32_t n_tiles,
                    int max_levels, qtc_hierarchy_t *hier) {
    memset(hier, 0, sizeof(*hier));

    /* Level 0: copy from tiles */
    hier->level_count[0] = n_tiles;
    hier->levels[0] = (qtc_hlevel_t *)malloc(n_tiles * sizeof(qtc_hlevel_t));
    for (uint32_t i = 0; i < n_tiles; i++) {
        hier->levels[0][i].start = i;
        hier->levels[0][i].end = i + 1;
        hier->levels[0][i].is_L = tiles[i].is_L;
    }
    hier->n_levels = 1;

    for (int lvl = 0; lvl < max_levels; lvl++) {
        uint32_t prev_count = hier->level_count[lvl];
        qtc_hlevel_t *prev = hier->levels[lvl];
        if (prev_count < 2) break;

        qtc_pmap_t *pmap = (qtc_pmap_t *)malloc(prev_count * sizeof(qtc_pmap_t));
        for (uint32_t j = 0; j < prev_count; j++) {
            pmap[j].parent_idx = -1; pmap[j].pos = -1;
        }
        qtc_hlevel_t *cur = (qtc_hlevel_t *)malloc(prev_count * sizeof(qtc_hlevel_t));
        uint32_t ci = 0;
        uint32_t i = 0;

        while (i < prev_count) {
            if (i + 1 < prev_count && prev[i].is_L && !prev[i + 1].is_L) {
                cur[ci].start = prev[i].start;
                cur[ci].end = prev[i + 1].end;
                cur[ci].is_L = true;
                pmap[i].parent_idx = (int32_t)ci;
                pmap[i].pos = 0;
                pmap[i + 1].parent_idx = (int32_t)ci;
                pmap[i + 1].pos = 1;
                i += 2;
            } else {
                cur[ci].start = prev[i].start;
                cur[ci].end = prev[i].end;
                cur[ci].is_L = false;
                pmap[i].parent_idx = (int32_t)ci;
                pmap[i].pos = 0;
                i++;
            }
            ci++;
        }

        hier->parent_maps[lvl] = pmap;
        hier->levels[lvl + 1] = cur;
        hier->level_count[lvl + 1] = ci;
        hier->n_levels++;
    }

    return hier->n_levels;
}

void free_hierarchy(qtc_hierarchy_t *hier) {
    for (int i = 0; i <= QTC_MAX_HIER; i++) {
        free(hier->levels[i]);
        hier->levels[i] = NULL;
    }
    for (int i = 0; i < QTC_MAX_HIER; i++) {
        free(hier->parent_maps[i]);
        hier->parent_maps[i] = NULL;
    }
}

/* ══════════════════════════════════════════════════════
 * Hierarchy Context
 * ══════════════════════════════════════════════════════ */
uint8_t get_hier_ctx(uint32_t tile_idx, const qtc_hierarchy_t *hier) {
    uint8_t h = hier->levels[0][tile_idx].is_L ? 1 : 0;
    uint32_t idx = tile_idx;
    int depth = hier->n_levels - 1;
    if (depth > 3) depth = 3;

    for (int k = 0; k < depth; k++) {
        qtc_pmap_t *pm = hier->parent_maps[k];
        if (!pm || idx >= hier->level_count[k]) break;
        if (pm[idx].parent_idx < 0) break;
        uint32_t pidx = (uint32_t)pm[idx].parent_idx;
        int8_t pos = pm[idx].pos;
        uint8_t parent_L = hier->levels[k + 1][pidx].is_L ? 1 : 0;
        h = ((h * 5) + pos * 3 + parent_L) & 0x07;
        idx = pidx;
    }
    return h;
}

/* ══════════════════════════════════════════════════════
 * Deep Position Detection
 * ══════════════════════════════════════════════════════ */
qtc_deep_t detect_deep_positions(const qtc_tile_t *tiles, uint32_t n_tiles,
                                  const qtc_hierarchy_t *hier) {
    qtc_deep_t dp;
    int max_k = hier->n_levels - 1;
    if (max_k > QTC_MAX_HIER) max_k = QTC_MAX_HIER;
    dp.max_k = max_k;

    dp.can = (bool **)calloc(max_k + 1, sizeof(bool *));
    dp.skip = (uint32_t **)calloc(max_k + 1, sizeof(uint32_t *));
    for (int k = 1; k <= max_k; k++) {
        dp.can[k] = (bool *)calloc(n_tiles, sizeof(bool));
        dp.skip[k] = (uint32_t *)calloc(n_tiles, sizeof(uint32_t));
    }

    for (uint32_t ti = 0; ti < n_tiles; ti++) {
        if (!tiles[ti].is_L) continue;
        uint32_t idx_k = ti;

        for (int k = 0; k < max_k; k++) {
            qtc_pmap_t *pm = hier->parent_maps[k];
            if (!pm || idx_k >= hier->level_count[k]) break;
            if (pm[idx_k].parent_idx < 0) break;
            if (pm[idx_k].pos != 0) break;

            uint32_t pidx = (uint32_t)pm[idx_k].parent_idx;
            if (!hier->levels[k + 1][pidx].is_L) break;

            uint32_t st = hier->levels[k + 1][pidx].start;
            uint32_t en = hier->levels[k + 1][pidx].end;

            uint32_t nw_cov = 0;
            for (uint32_t j = st; j < en; j++) nw_cov += tiles[j].nwords;

            int expected = (k + 1 < QTC_N_LEVELS + 1) ? QTC_HIER_WORD_LENS[k + 1] : -1;
            if (expected > 0 && nw_cov == (uint32_t)expected) {
                if (k == 0 && (ti + 1 >= n_tiles || tiles[ti + 1].is_L)) {
                    idx_k = pidx;
                    continue;
                }
                dp.can[k + 1][ti] = true;
                dp.skip[k + 1][ti] = en - st - 1;
            }
            idx_k = pidx;
        }
    }

    return dp;
}

/* Randomized quasicrystal: golden tiling with ~5% random L<->S swaps */
qtc_tile_t *gen_random_qc_tiles(uint32_t n_words, uint32_t seed, uint32_t *out_n_tiles) {
    /* Start from golden tiling phase=0 */
    uint32_t nt;
    qtc_tile_t *tiles = qc_word_tiling(n_words, 0.0, &nt);
    if (!tiles) { *out_n_tiles = 0; return NULL; }
    /* Simple xorshift RNG */
    uint32_t rng = seed ? seed : 0xDEADBEEF;
    #define RNG_NEXT(r) ((r) ^= (r) << 13, (r) ^= (r) >> 17, (r) ^= (r) << 5)
    /* Swap ~5% of tiles, preserving no-adjacent-S */
    for (uint32_t i = 1; i + 1 < nt; i++) {
        RNG_NEXT(rng);
        if ((rng & 0x1F) != 0) continue; /* ~3% swap rate */
        /* Only swap if neighbors allow it */
        if (tiles[i].is_L && !tiles[i-1].is_L) continue; /* would create SS */
        if (tiles[i].is_L && !tiles[i+1].is_L) continue;
        if (!tiles[i].is_L && tiles[i-1].is_L && tiles[i+1].is_L) {
            /* S→L is always safe */
            tiles[i].is_L = true;
            tiles[i].nwords = 2;
        } else if (tiles[i].is_L && tiles[i-1].is_L && tiles[i+1].is_L) {
            /* L→S is safe if both neighbors are L */
            tiles[i].is_L = false;
            tiles[i].nwords = 1;
        }
    }
    /* Recompute wpos */
    uint32_t wpos = 0;
    for (uint32_t i = 0; i < nt; i++) {
        tiles[i].wpos = wpos;
        wpos += tiles[i].nwords;
    }
    /* Trim to n_words */
    uint32_t final_nt = 0;
    for (uint32_t i = 0; i < nt; i++) {
        if (tiles[i].wpos + tiles[i].nwords > n_words) break;
        final_nt = i + 1;
    }
    *out_n_tiles = final_nt;
    #undef RNG_NEXT
    return tiles;
}

/* Sanddrift Sequence: substitution L→LSSL, S→SLS, governed by √2
 * freq(L) = √2-1 ≈ 0.4142, freq(S) = 2-√2 ≈ 0.5858
 * LL forbidden, SSS forbidden. Novel quasi-Sturmian structure. */
qtc_tile_t *gen_sanddrift_tiles(uint32_t n_words, uint32_t *out_n_tiles) {
    uint32_t need = n_words + 16;
    uint32_t cap = 8;
    uint8_t *seq = (uint8_t *)malloc(cap);
    seq[0] = 1; /* L=1, S=0 */
    uint32_t seq_len = 1;

    while (seq_len < need) {
        uint32_t new_cap = seq_len * 4 + 16;
        uint8_t *new_seq = (uint8_t *)malloc(new_cap);
        uint32_t ni = 0;
        for (uint32_t i = 0; i < seq_len && ni + 4 <= new_cap; i++) {
            if (seq[i] == 1) { /* L → LSSL */
                new_seq[ni++] = 1; new_seq[ni++] = 0;
                new_seq[ni++] = 0; new_seq[ni++] = 1;
            } else { /* S → SLS */
                new_seq[ni++] = 0; new_seq[ni++] = 1; new_seq[ni++] = 0;
            }
        }
        free(seq);
        seq = new_seq; seq_len = ni; cap = new_cap;
    }

    /* Convert to tiles directly (Sanddrift has LL forbidden, so no SS-merge needed) */
    qtc_tile_t *tiles = (qtc_tile_t *)malloc(need * sizeof(qtc_tile_t));
    uint32_t nt = 0, wpos = 0, k = 0;
    while (wpos < n_words && k < seq_len) {
        bool is_L = (seq[k] == 1);
        uint32_t consume = is_L ? 2 : 1;
        if (wpos + consume > n_words) { is_L = false; consume = 1; }
        tiles[nt].wpos = wpos;
        tiles[nt].nwords = (uint8_t)consume;
        tiles[nt].is_L = is_L;
        nt++; wpos += consume; k++;
    }
    free(seq);
    *out_n_tiles = nt;
    return tiles;
}

/* Period-5 tiling (LLSLS repeated) for A/B testing */
qtc_tile_t *gen_period5_tiles(uint32_t n_words, uint32_t *out_n_tiles) {
    static const bool pattern[5] = {true, true, false, true, false}; /* LLSLS */
    uint32_t cap = n_words;
    qtc_tile_t *tiles = (qtc_tile_t *)malloc(cap * sizeof(qtc_tile_t));
    uint32_t nt = 0, wpos = 0, k = 0;
    while (wpos < n_words) {
        bool is_L = pattern[k % 5];
        uint32_t consume = is_L ? 2 : 1;
        if (wpos + consume > n_words) { is_L = false; consume = 1; }
        tiles[nt].wpos = wpos;
        tiles[nt].nwords = (uint8_t)consume;
        tiles[nt].is_L = is_L;
        nt++; wpos += consume; k++;
    }
    *out_n_tiles = nt;
    return tiles;
}

void free_deep(qtc_deep_t *dp, uint32_t n_tiles) {
    (void)n_tiles;
    if (dp->can) {
        for (int k = 0; k <= dp->max_k; k++) {
            free(dp->can[k]);
            free(dp->skip[k]);
        }
        free(dp->can);
        free(dp->skip);
    }
    dp->can = NULL; dp->skip = NULL;
}
