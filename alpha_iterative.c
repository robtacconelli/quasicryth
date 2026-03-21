/*
 * Iterative Alpha Builder: greedily pick the best alpha one at a time,
 * always measuring against the CURRENT accumulated set.
 *
 * Usage: alpha_iterative <file> <n_to_pick> [alpha_min alpha_max alpha_step]
 */
#include "qtc.h"
#include "ht.h"
#include "ac.h"
#include "fib.h"
#include "tok.h"
#include "cb.h"
extern uint32_t collect_deep_from_tiling(const qtc_tile_t *tiles, uint32_t n_tiles,
    const qtc_hierarchy_t *hier, const qtc_cbs_t *cbs, uint32_t nw,
    uint8_t *deep_best_lv, uint32_t *deep_best_idx);
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

static void run_greedy(
    uint32_t nw, const uint32_t *wids, const qtc_cbs_t *cbs,
    uint8_t *deep_lv, uint32_t *deep_idx,
    uint32_t counts[QTM_N_ENC_LEVELS])
{
    memset(counts, 0, QTM_N_ENC_LEVELS * sizeof(uint32_t));
    bool *covered = (bool *)calloc(nw, sizeof(bool));

    for (int lv = QTM_N_ENC_LEVELS - 1; lv >= 3; lv--) {
        int nwords = QTM_LEVEL_WORDS[lv];
        for (uint32_t wp = 0; wp < nw; wp++) {
            if (deep_lv[wp] != (uint8_t)lv) continue;
            if (wp + (uint32_t)nwords > nw) continue;
            bool ok = true;
            for (int j = 0; j < nwords; j++)
                if (covered[wp + (uint32_t)j]) { ok = false; break; }
            if (!ok) continue;
            for (int j = 0; j < nwords; j++) covered[wp + (uint32_t)j] = true;
            counts[lv]++;
        }
    }
    for (uint32_t wp = 0; wp + 1 < nw; wp++) {
        if (covered[wp] || covered[wp + 1]) continue;
        uint32_t bi_idx;
        if (u64map_get(&cbs->bi_map, pack_bi(wids[wp], wids[wp + 1]), &bi_idx)) {
            covered[wp] = true; covered[wp + 1] = true;
            counts[2]++;
        }
    }
    for (uint32_t wp = 0; wp < nw; wp++) {
        if (covered[wp]) continue;
        int32_t uidx = (wids[wp] < cbs->uni_rmap_sz) ? cbs->uni_rmap[wids[wp]] : -1;
        if (uidx >= 0) counts[1]++; else counts[0]++;
        covered[wp] = true;
    }
    free(covered);
}

static double score(const uint32_t c[QTM_N_ENC_LEVELS]) {
    static const double bpe[] = {40,14,13,12,14,15,15,16,16,17,17,18};
    double s = 0;
    for (int i = 0; i < QTM_N_ENC_LEVELS; i++) s += c[i] * bpe[i];
    return s;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: alpha_iterative <file> <n_to_pick> [min max step]\n");
        return 1;
    }
    const char *fname = argv[1];
    int n_pick = atoi(argv[2]);
    double a_min  = (argc > 3) ? atof(argv[3]) : 0.595;
    double a_max  = (argc > 4) ? atof(argv[4]) : 0.645;
    double a_step = (argc > 5) ? atof(argv[5]) : 0.0002;

    FILE *f = fopen(fname, "rb");
    fseek(f, 0, SEEK_END); size_t fsize = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(fsize); fread(data, 1, fsize, f); fclose(f);

    uint8_t *lowered; uint64_t lowered_len; qtc_token_t *tokens;
    tokenize(data, fsize, &lowered, &lowered_len, &tokens);
    free(tokens); free(data);
    const uint8_t **wptrs; uint16_t *wlens;
    uint32_t nw = word_split(lowered, lowered_len, &wptrs, &wlens);
    qtc_cb_sizes_t sizes = auto_codebook_sizes(nw);
    qtc_cbs_t cbs;
    cbs_build(&cbs, wptrs, wlens, nw, &sizes);
    free(wptrs); free(wlens); free(lowered);
    const uint32_t *wids = cbs.word_ids;

    static const char *ln[] = {"esc","uni","bi","tri","5g","8g","13g","21g","34g","55g","89g","144g"};

    /* Golden baseline */
    fprintf(stderr, "Building golden baseline (12 phases)...\n");
    uint8_t  *cur_lv  = calloc(nw, sizeof(uint8_t));
    uint32_t *cur_idx = calloc(nw, sizeof(uint32_t));

    qtm_tiling_desc_t descs[QTC_N_TILINGS];
    qtm_get_tiling_descs(descs);
    for (int t = 0; t < 12; t++) {
        uint32_t nt;
        qtc_tile_t *tiles = qtm_gen_tiling(&descs[t], nw, &nt);
        if (!tiles) continue;
        qtc_hierarchy_t hier;
        build_hierarchy(tiles, nt, QTC_MAX_HIER, &hier);
        collect_deep_from_tiling(tiles, nt, &hier, &cbs, nw, cur_lv, cur_idx);
        free_hierarchy(&hier); free(tiles);
    }

    uint32_t cur_counts[QTM_N_ENC_LEVELS];
    run_greedy(nw, wids, &cbs, cur_lv, cur_idx, cur_counts);
    double cur_score = score(cur_counts);

    printf("=== Golden baseline (12 phases) ===\n");
    for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++)
        if (cur_counts[lv]) printf("  %5s: %u\n", ln[lv], cur_counts[lv]);
    printf("  Score: %.0f bits\n\n", cur_score);

    /* Build candidate list */
    double phi_inv = 1.0 / ((1.0 + sqrt(5.0)) / 2.0);
    int n_cand = 0;
    double *cand_alphas = malloc(10000 * sizeof(double));
    for (double a = a_min; a <= a_max + 1e-9; a += a_step) {
        if (fabs(a - phi_inv) < 0.00015) continue; /* skip golden itself */
        cand_alphas[n_cand++] = a;
    }
    bool *used = calloc(n_cand, sizeof(bool));

    fprintf(stderr, "%d candidates in [%.4f, %.4f]\n\n", n_cand, a_min, a_max);

    /* Iterative greedy: pick best alpha one at a time */
    for (int round = 0; round < n_pick; round++) {
        double best_delta = 0;
        int best_ci = -1;
        uint8_t  *best_lv  = NULL;
        uint32_t *best_idx = NULL;
        uint32_t best_counts[QTM_N_ENC_LEVELS];

        double t0 = now_sec();
        for (int ci = 0; ci < n_cand; ci++) {
            if (used[ci]) continue;
            double alpha = cand_alphas[ci];

            /* Copy current state */
            uint8_t  *test_lv  = malloc(nw); memcpy(test_lv, cur_lv, nw);
            uint32_t *test_idx = malloc(nw * 4); memcpy(test_idx, cur_idx, nw * 4);

            /* Add 2 phases */
            for (int ph = 0; ph < 2; ph++) {
                uint32_t nt;
                qtc_tile_t *tiles = qc_word_tiling_alpha(nw, alpha, ph * 0.5, &nt);
                if (!tiles) continue;
                qtc_hierarchy_t hier;
                build_hierarchy(tiles, nt, QTC_MAX_HIER, &hier);
                collect_deep_from_tiling(tiles, nt, &hier, &cbs, nw, test_lv, test_idx);
                free_hierarchy(&hier); free(tiles);
            }

            uint32_t test_counts[QTM_N_ENC_LEVELS];
            run_greedy(nw, wids, &cbs, test_lv, test_idx, test_counts);
            double delta = score(test_counts) - cur_score;

            if (delta < best_delta || best_ci < 0) {
                best_delta = delta;
                best_ci = ci;
                free(best_lv); free(best_idx);
                best_lv = test_lv; best_idx = test_idx;
                memcpy(best_counts, test_counts, sizeof(best_counts));
            } else {
                free(test_lv); free(test_idx);
            }
        }

        if (best_ci < 0) break;

        double elapsed = now_sec() - t0;
        used[best_ci] = true;

        printf("=== Round %d: picked alpha=%.4f (delta=%+.0f bits = %+.0f bytes, %.1fs) ===\n",
               round + 1, cand_alphas[best_ci], best_delta, best_delta / 8.0, elapsed);

        /* Show per-level changes */
        for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++) {
            int d = (int)best_counts[lv] - (int)cur_counts[lv];
            if (d != 0)
                printf("  %5s: %u (%+d)\n", ln[lv], best_counts[lv], d);
        }

        /* Show words upgraded to deeper levels */
        int words_up = 0, words_down = 0;
        for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++) {
            int d = (int)best_counts[lv] - (int)cur_counts[lv];
            int w = d * QTM_LEVEL_WORDS[lv];
            if (d > 0 && lv >= 3) words_up += w;
            if (d < 0 && lv <= 2) words_down += -w;
        }
        printf("  Words moved to deeper: +%d, from shallower: %d\n", words_up, words_down);
        printf("  Cumulative score: %.0f (total delta from golden: %+.0f = %+.0f bytes)\n\n",
               score(best_counts), score(best_counts) - score(cur_counts) + (cur_score - score(cur_counts)),
               (score(best_counts) - score(cur_counts)) / 8.0);

        /* Update state */
        free(cur_lv); free(cur_idx);
        cur_lv = best_lv; cur_idx = best_idx;
        memcpy(cur_counts, best_counts, sizeof(cur_counts));
        cur_score = score(cur_counts);
    }

    printf("=== Final state after %d additions ===\n", n_pick);
    for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++)
        if (cur_counts[lv]) printf("  %5s: %u\n", ln[lv], cur_counts[lv]);
    printf("  Score: %.0f bits\n", cur_score);

    free(cur_lv); free(cur_idx);
    free(cand_alphas); free(used);
    cbs_free(&cbs);
    return 0;
}
