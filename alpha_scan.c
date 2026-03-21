/*
 * Alpha Scanner: find optimal irrational alpha values for cut-and-project tilings
 * that maximize deep n-gram coverage (especially high levels: 55g, 89g, 144g).
 *
 * Usage: alpha_scan <file> [alpha_min alpha_max alpha_step]
 * Default range: 0.580 to 0.660 in steps of 0.0005
 */
#include "qtc.h"
#include "ht.h"
#include "ac.h"
#include "fib.h"
#include "tok.h"
#include "cb.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Count deep hits per level for a single tiling on pre-built codebook */
static void count_deep_hits(
    uint32_t nw, const qtc_cbs_t *cbs,
    double alpha, double phase,
    uint32_t hits_out[QTM_N_ENC_LEVELS],  /* hits per encoding level 3..11 */
    uint32_t *total_deep_positions)
{
    memset(hits_out, 0, QTM_N_ENC_LEVELS * sizeof(uint32_t));
    *total_deep_positions = 0;

    uint32_t nt;
    qtc_tile_t *tiles = qc_word_tiling_alpha(nw, alpha, phase, &nt);
    if (!tiles) return;

    qtc_hierarchy_t hier;
    build_hierarchy(tiles, nt, QTC_MAX_HIER, &hier);

    /* Position-indexed deep matches */
    uint8_t  *deep_best_lv  = (uint8_t  *)calloc(nw, sizeof(uint8_t));
    uint32_t *deep_best_idx = (uint32_t *)calloc(nw, sizeof(uint32_t));

    /* Reuse the collect_deep_from_tiling logic inline */
    int max_k = hier.n_levels - 1;
    if (max_k > QTC_MAX_HIER) max_k = QTC_MAX_HIER;

    for (uint32_t ti = 0; ti < nt; ti++) {
        if (!tiles[ti].is_L) continue;
        uint32_t wpos = tiles[ti].wpos;
        int best_k = -1;
        uint32_t idx_k = ti;

        for (int k = 0; k < max_k; k++) {
            qtc_pmap_t *pm = hier.parent_maps[k];
            if (!pm || idx_k >= hier.level_count[k]) break;
            if (pm[idx_k].parent_idx < 0) break;
            if (pm[idx_k].pos != 0) break;
            uint32_t pidx = (uint32_t)pm[idx_k].parent_idx;
            if (!hier.levels[k + 1][pidx].is_L) break;
            int heir_k = k + 1;
            int gl = (heir_k <= QTC_N_LEVELS) ? QTC_HIER_WORD_LENS[heir_k] : -1;
            if (gl > 0 && wpos + (uint32_t)gl <= nw) {
                uint32_t st = hier.levels[k + 1][pidx].start;
                uint32_t en = hier.levels[k + 1][pidx].end;
                uint32_t nw_cov = 0;
                for (uint32_t j = st; j < en; j++) nw_cov += tiles[j].nwords;
                if (nw_cov == (uint32_t)gl) {
                    if (k == 0 && (ti + 1 >= nt || tiles[ti + 1].is_L)) {
                        idx_k = pidx; continue;
                    }
                    if (heir_k <= QTC_N_LEVELS && cbs->ng_count[heir_k - 1] > 0) {
                        uint32_t cb_idx;
                        if (nmap_get(&cbs->ng_maps[heir_k - 1], wpos, &cb_idx)) {
                            best_k = heir_k;
                        }
                    }
                }
            }
            idx_k = pidx;
        }

        if (best_k >= 1) {
            uint8_t enc_level = (uint8_t)(best_k + 2);
            if (enc_level > deep_best_lv[wpos]) {
                deep_best_lv[wpos] = enc_level;
            }
        }
    }

    /* Count per-level */
    for (uint32_t i = 0; i < nw; i++) {
        if (deep_best_lv[i] >= 3) {
            hits_out[deep_best_lv[i]]++;
            (*total_deep_positions)++;
        }
    }

    free(deep_best_lv);
    free(deep_best_idx);
    free_hierarchy(&hier);
    free(tiles);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: alpha_scan <file> [alpha_min alpha_max alpha_step]\n");
        return 1;
    }

    const char *fname = argv[1];
    double alpha_min  = (argc > 2) ? atof(argv[2]) : 0.580;
    double alpha_max  = (argc > 3) ? atof(argv[3]) : 0.660;
    double alpha_step = (argc > 4) ? atof(argv[4]) : 0.0005;

    /* Read file */
    FILE *f = fopen(fname, "rb");
    if (!f) { perror(fname); return 1; }
    fseek(f, 0, SEEK_END);
    size_t fsize = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(fsize);
    fread(data, 1, fsize, f);
    fclose(f);

    /* Tokenize + word split */
    uint8_t *lowered; uint64_t lowered_len;
    qtc_token_t *tokens;
    uint32_t n_tok = tokenize(data, fsize, &lowered, &lowered_len, &tokens);
    free(tokens); free(data);

    const uint8_t **word_ptrs; uint16_t *word_lens;
    uint32_t nw = word_split(lowered, lowered_len, &word_ptrs, &word_lens);

    /* Build codebooks */
    qtc_cb_sizes_t sizes = auto_codebook_sizes(nw);
    qtc_cbs_t cbs;
    cbs_build(&cbs, word_ptrs, word_lens, nw, &sizes);
    free(word_ptrs); free(word_lens); free(lowered);

    fprintf(stderr, "File: %s (%zu B, %u words, %u uni, %u bi)\n",
            fname, fsize, nw, cbs.n_uni, cbs.n_bi);
    fprintf(stderr, "Scanning alpha %.4f to %.4f step %.4f (2 phases each)\n\n",
            alpha_min, alpha_max, alpha_step);

    /* Header */
    static const char *level_names[] = {
        "", "", "", "tri", "5g", "8g", "13g", "21g", "34g", "55g", "89g", "144g"
    };

    printf("alpha      total   ");
    for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++)
        printf(" %6s", level_names[lv]);
    /* Weighted score: deeper levels worth exponentially more */
    printf("  weighted_score\n");

    double best_alpha = 0;
    double best_score = 0;
    uint32_t best_hits[QTM_N_ENC_LEVELS];

    double t0 = now_sec();
    int n_scanned = 0;

    for (double alpha = alpha_min; alpha <= alpha_max + 1e-9; alpha += alpha_step) {
        /* Test with 2 phases */
        uint32_t combined_hits[QTM_N_ENC_LEVELS];
        memset(combined_hits, 0, sizeof(combined_hits));
        uint32_t total_dp = 0;

        for (int ph = 0; ph < 2; ph++) {
            double phase = ph * 0.5;
            uint32_t hits[QTM_N_ENC_LEVELS];
            uint32_t dp;
            count_deep_hits(nw, &cbs, alpha, phase, hits, &dp);
            for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++)
                combined_hits[lv] += hits[lv];
            total_dp += dp;
        }

        /* Weighted score: prioritize high n-grams heavily */
        /* Weight = words_per_match^2 to strongly prefer deep levels */
        static const double weights[] = {
            0,0,0,
            9,    /* tri=3: 3^2 */
            25,   /* 5g: 5^2 */
            64,   /* 8g: 8^2 */
            169,  /* 13g: 13^2 */
            441,  /* 21g: 21^2 */
            1156, /* 34g: 34^2 */
            3025, /* 55g: 55^2 */
            7921, /* 89g: 89^2 */
            20736 /* 144g: 144^2 */
        };
        double score = 0;
        for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++)
            score += combined_hits[lv] * weights[lv];

        printf("%.4f  %8u", alpha, total_dp);
        for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++)
            printf(" %6u", combined_hits[lv]);
        printf("  %.0f", score);
        if (score > best_score) {
            printf("  *BEST*");
            best_score = score;
            best_alpha = alpha;
            memcpy(best_hits, combined_hits, sizeof(best_hits));
        }
        printf("\n");
        n_scanned++;
    }

    double elapsed = now_sec() - t0;
    fprintf(stderr, "\nScanned %d alpha values in %.1fs\n", n_scanned, elapsed);
    fprintf(stderr, "Best alpha: %.4f (weighted score: %.0f)\n", best_alpha, best_score);
    fprintf(stderr, "Best hits: ");
    for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++)
        if (best_hits[lv]) fprintf(stderr, "%s=%u ", level_names[lv], best_hits[lv]);
    fprintf(stderr, "\n");

    /* Also show top-20 alphas for high n-grams specifically */
    fprintf(stderr, "\nRe-scanning for top alphas by 55g+89g+144g hits...\n");

    cbs_free(&cbs);
    return 0;
}
