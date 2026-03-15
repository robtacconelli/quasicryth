/*
 * QTC - Fibonacci quasicrystal tiling, hierarchy, deep positions
 */
#include "fib.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const double INV_PHI = 1.0 / ((1.0 + sqrt(5.0)) / 2.0);

/* ══════════════════════════════════════════════════════
 * QC Word Tiling (cut-and-project)
 * ══════════════════════════════════════════════════════ */
qtc_tile_t *qc_word_tiling(uint32_t n_words, double phase, uint32_t *out_n_tiles) {
    /* Pass 1: Generate symbols */
    uint32_t sym_cap = n_words + 16;
    bool *symbols = (bool *)malloc(sym_cap * sizeof(bool));
    uint32_t n_sym = 0;
    uint32_t total = 0;
    uint32_t k = 0;

    while (total < n_words) {
        bool is_L = (int)floor((k + 1 + phase) * INV_PHI) -
                    (int)floor((k + phase) * INV_PHI) == 1;
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

    /* Pass 2: Fix matching rule violations (merge adjacent S pairs into L) */
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
    free(symbols);

    /* Pass 3: Convert to tiles */
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

        /* Allocate parent map and new level */
        qtc_pmap_t *pmap = (qtc_pmap_t *)malloc(prev_count * sizeof(qtc_pmap_t));
        for (uint32_t j = 0; j < prev_count; j++) {
            pmap[j].parent_idx = -1; pmap[j].pos = -1;
        }
        qtc_hlevel_t *cur = (qtc_hlevel_t *)malloc(prev_count * sizeof(qtc_hlevel_t));
        uint32_t ci = 0;
        uint32_t i = 0;

        while (i < prev_count) {
            if (i + 1 < prev_count && prev[i].is_L && !prev[i + 1].is_L) {
                /* L + S -> super-L */
                cur[ci].start = prev[i].start;
                cur[ci].end = prev[i + 1].end;
                cur[ci].is_L = true;
                pmap[i].parent_idx = (int32_t)ci;
                pmap[i].pos = 0;
                pmap[i + 1].parent_idx = (int32_t)ci;
                pmap[i + 1].pos = 1;
                i += 2;
            } else {
                /* isolated -> super-S */
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

    /* Allocate can[][] and skip[][] for levels 1..max_k */
    dp.can = (bool **)calloc(max_k + 1, sizeof(bool *));
    dp.skip = (uint32_t **)calloc(max_k + 1, sizeof(uint32_t *));
    for (int k = 1; k <= max_k; k++) {
        dp.can[k] = (bool *)calloc(n_tiles, sizeof(bool));
        dp.skip[k] = (uint32_t *)calloc(n_tiles, sizeof(uint32_t));
    }

    for (uint32_t ti = 0; ti < n_tiles; ti++) {
        if (!tiles[ti].is_L) continue;  /* only L tiles */
        uint32_t idx_k = ti;

        for (int k = 0; k < max_k; k++) {
            qtc_pmap_t *pm = hier->parent_maps[k];
            if (!pm || idx_k >= hier->level_count[k]) break;
            if (pm[idx_k].parent_idx < 0) break;
            if (pm[idx_k].pos != 0) break;

            uint32_t pidx = (uint32_t)pm[idx_k].parent_idx;
            if (!hier->levels[k + 1][pidx].is_L) break;

            /* Valid first child of a super-L at level k+1 */
            uint32_t st = hier->levels[k + 1][pidx].start;
            uint32_t en = hier->levels[k + 1][pidx].end;

            /* Count words covered */
            uint32_t nw_cov = 0;
            for (uint32_t j = st; j < en; j++) nw_cov += tiles[j].nwords;

            int expected = (k + 1 < QTC_N_LEVELS + 1) ? QTC_HIER_WORD_LENS[k + 1] : -1;
            if (expected > 0 && nw_cov == (uint32_t)expected) {
                /* For trigram (level 1 = k+1=1, k=0), require next tile is S */
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
