/*
 * Alpha Greedy Scanner: measure incremental impact of each alpha on greedy selection.
 * For each candidate alpha, adds it to the 12 golden-ratio baseline and runs full
 * greedy selection, reporting per-level count changes.
 *
 * Usage: alpha_greedy_scan <file> [alpha_min alpha_max alpha_step]
 */
#include "qtc.h"
#include "ht.h"
#include "ac.h"
#include "fib.h"
#include "tok.h"
#include "cb.h"
extern uint32_t collect_deep_from_tiling(const qtc_tile_t *tiles, uint32_t n_tiles, const qtc_hierarchy_t *hier, const qtc_cbs_t *cbs, uint32_t nw, uint8_t *deep_best_lv, uint32_t *deep_best_idx);
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

/* Run full greedy selection and return per-level event counts */
static void run_greedy(
    uint32_t nw, const uint32_t *wids,
    const qtc_cbs_t *cbs,
    uint8_t *deep_best_lv, uint32_t *deep_best_idx,
    uint32_t counts_out[QTM_N_ENC_LEVELS])
{
    memset(counts_out, 0, QTM_N_ENC_LEVELS * sizeof(uint32_t));

    uint8_t  *result_level = (uint8_t  *)calloc(nw, sizeof(uint8_t));
    bool     *covered      = (bool     *)calloc(nw, sizeof(bool));

    /* Pass 1: deep levels (11 down to 3) */
    for (int lv = QTM_N_ENC_LEVELS - 1; lv >= 3; lv--) {
        int nwords = QTM_LEVEL_WORDS[lv];
        for (uint32_t wp = 0; wp < nw; wp++) {
            if (deep_best_lv[wp] != (uint8_t)lv) continue;
            if (wp + (uint32_t)nwords > nw) continue;
            bool ok = true;
            for (int j = 0; j < nwords; j++)
                if (covered[wp + (uint32_t)j]) { ok = false; break; }
            if (!ok) continue;
            result_level[wp] = (uint8_t)lv;
            for (int j = 0; j < nwords; j++) covered[wp + (uint32_t)j] = true;
            counts_out[lv]++;
        }
    }

    /* Pass 2: bigrams */
    for (uint32_t wp = 0; wp + 1 < nw; wp++) {
        if (covered[wp] || covered[wp + 1]) continue;
        uint32_t bi_idx;
        if (u64map_get(&cbs->bi_map, pack_bi(wids[wp], wids[wp + 1]), &bi_idx)) {
            covered[wp] = true; covered[wp + 1] = true;
            counts_out[2]++;
        }
    }

    /* Pass 3: unigrams and escapes */
    for (uint32_t wp = 0; wp < nw; wp++) {
        if (covered[wp]) continue;
        int32_t uidx = (wids[wp] < cbs->uni_rmap_sz) ? cbs->uni_rmap[wids[wp]] : -1;
        if (uidx >= 0) counts_out[1]++;
        else counts_out[0]++;
        covered[wp] = true;
    }

    free(result_level);
    free(covered);
}

/* Compute word-weighted compression score:
 * Lower is better. Each event costs ~15 bits for AC coding.
 * But a 144-gram event covers 144 words, a unigram covers 1.
 * Score = total events (fewer events = better compression) */
static double compression_score(const uint32_t counts[QTM_N_ENC_LEVELS]) {
    double score = 0;
    /* Approximate bits per event by level */
    static const double bits_per_event[] = {
        40,   /* escape */
        14,   /* unigram */
        13,   /* bigram */
        12, 14, 15, 15, 16, 16, 17, 17, 18  /* tri through 144g */
    };
    for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++)
        score += counts[lv] * bits_per_event[lv];
    return score;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: alpha_greedy_scan <file> [alpha_min alpha_max alpha_step]\n");
        return 1;
    }

    const char *fname = argv[1];
    double alpha_min  = (argc > 2) ? atof(argv[2]) : 0.590;
    double alpha_max  = (argc > 3) ? atof(argv[3]) : 0.650;
    double alpha_step = (argc > 4) ? atof(argv[4]) : 0.0002;

    /* Read file */
    FILE *f = fopen(fname, "rb");
    if (!f) { perror(fname); return 1; }
    fseek(f, 0, SEEK_END);
    size_t fsize = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(fsize);
    fread(data, 1, fsize, f);
    fclose(f);

    /* Tokenize */
    uint8_t *lowered; uint64_t lowered_len;
    qtc_token_t *tokens;
    tokenize(data, fsize, &lowered, &lowered_len, &tokens);
    free(tokens); free(data);

    const uint8_t **word_ptrs; uint16_t *word_lens;
    uint32_t nw = word_split(lowered, lowered_len, &word_ptrs, &word_lens);

    /* Build codebooks */
    qtc_cb_sizes_t sizes = auto_codebook_sizes(nw);
    qtc_cbs_t cbs;
    cbs_build(&cbs, word_ptrs, word_lens, nw, &sizes);
    free(word_ptrs); free(word_lens); free(lowered);

    const uint32_t *wids = cbs.word_ids;

    fprintf(stderr, "File: %s (%zu B, %u words)\n", fname, fsize, nw);

    /* ── Step 1: Compute golden-ratio baseline (12 phases) ── */
    fprintf(stderr, "Computing golden-ratio baseline (12 phases)...\n");

    uint8_t  *base_deep_lv  = (uint8_t  *)calloc(nw, sizeof(uint8_t));
    uint32_t *base_deep_idx = (uint32_t *)calloc(nw, sizeof(uint32_t));

    qtm_tiling_desc_t descs[QTC_N_TILINGS];
    qtm_get_tiling_descs(descs);

    double phi_inv = 1.0 / ((1.0 + sqrt(5.0)) / 2.0);
    for (int t = 0; t < 12; t++) {
        uint32_t nt;
        qtc_tile_t *tiles = qtm_gen_tiling(&descs[t], nw, &nt);
        if (!tiles) continue;
        qtc_hierarchy_t hier;
        build_hierarchy(tiles, nt, QTC_MAX_HIER, &hier);
        collect_deep_from_tiling(tiles, nt, &hier, &cbs, nw, base_deep_lv, base_deep_idx);
        free_hierarchy(&hier);
        free(tiles);
    }

    uint32_t base_counts[QTM_N_ENC_LEVELS];
    run_greedy(nw, wids, &cbs, base_deep_lv, base_deep_idx, base_counts);

    double base_score = compression_score(base_counts);

    fprintf(stderr, "Baseline: ");
    static const char *lv_names[] = {
        "esc","uni","bi","tri","5g","8g","13g","21g","34g","55g","89g","144g"
    };
    for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++)
        if (base_counts[lv]) fprintf(stderr, "%s=%u ", lv_names[lv], base_counts[lv]);
    fprintf(stderr, " score=%.0f\n\n", base_score);

    /* ── Step 2: For each candidate alpha, add 2 phases to baseline ── */
    fprintf(stderr, "Scanning alpha %.4f to %.4f step %.4f...\n", alpha_min, alpha_max, alpha_step);

    /* Header */
    printf("alpha       score_delta");
    for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++)
        printf("  d_%s", lv_names[lv]);
    printf("  words_covered_by_deeper\n");

    double best_alpha = 0;
    double best_delta = 0;

    for (double alpha = alpha_min; alpha <= alpha_max + 1e-9; alpha += alpha_step) {
        /* Skip if too close to golden (already in baseline) */
        if (fabs(alpha - phi_inv) < 0.0001) continue;

        /* Copy baseline deep arrays */
        uint8_t  *test_lv  = (uint8_t  *)malloc(nw * sizeof(uint8_t));
        uint32_t *test_idx = (uint32_t *)malloc(nw * sizeof(uint32_t));
        memcpy(test_lv,  base_deep_lv,  nw * sizeof(uint8_t));
        memcpy(test_idx, base_deep_idx, nw * sizeof(uint32_t));

        /* Add 2 phases of candidate alpha */
        for (int ph = 0; ph < 2; ph++) {
            double phase = ph * 0.5;
            uint32_t nt;
            qtc_tile_t *tiles = qc_word_tiling_alpha(nw, alpha, phase, &nt);
            if (!tiles) continue;
            qtc_hierarchy_t hier;
            build_hierarchy(tiles, nt, QTC_MAX_HIER, &hier);
            collect_deep_from_tiling(tiles, nt, &hier, &cbs, nw, test_lv, test_idx);
            free_hierarchy(&hier);
            free(tiles);
        }

        /* Run greedy on augmented deep arrays */
        uint32_t test_counts[QTM_N_ENC_LEVELS];
        run_greedy(nw, wids, &cbs, test_lv, test_idx, test_counts);

        double test_score = compression_score(test_counts);
        double delta = test_score - base_score;  /* negative = better */

        /* Count words moved to deeper levels */
        int words_upgraded = 0;
        for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++) {
            int diff = (int)test_counts[lv] - (int)base_counts[lv];
            if (diff > 0) words_upgraded += diff * QTM_LEVEL_WORDS[lv];
        }

        printf("%.4f  %+10.0f", alpha, delta);
        for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++)
            printf("  %+5d", (int)test_counts[lv] - (int)base_counts[lv]);
        printf("  %+d", words_upgraded);
        if (delta < best_delta) {
            printf("  *BEST*");
            best_delta = delta;
            best_alpha = alpha;
        }
        printf("\n");

        free(test_lv);
        free(test_idx);
    }

    fprintf(stderr, "\nBest alpha: %.4f (score delta: %+.0f bits = %+.0f bytes)\n",
            best_alpha, best_delta, best_delta / 8.0);

    cbs_free(&cbs);
    return 0;
}
