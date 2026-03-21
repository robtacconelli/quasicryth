/*
 * Coverage benchmark: measure each tiling's marginal contribution
 * to unique codebook-matched positions at each n-gram level.
 *
 * Usage: coverage_bench <file>
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

/* ── Bitset for tracking unique positions ─────────── */
typedef struct {
    uint64_t *bits;
    uint32_t  n_words;  /* number of uint64 words */
    uint32_t  count;    /* number of set bits */
} bitset_t;

static void bs_init(bitset_t *bs, uint32_t n_positions) {
    bs->n_words = (n_positions + 63) / 64;
    bs->bits = (uint64_t *)calloc(bs->n_words, sizeof(uint64_t));
    bs->count = 0;
}

static void bs_free(bitset_t *bs) { free(bs->bits); bs->bits = NULL; }

/* Set bit, returns true if it was newly set (not already set) */
static bool bs_set(bitset_t *bs, uint32_t pos) {
    uint32_t w = pos / 64;
    uint64_t m = 1ULL << (pos % 64);
    if (bs->bits[w] & m) return false;
    bs->bits[w] |= m;
    bs->count++;
    return true;
}

/* ── File I/O ─────────────────────────────────────── */
static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(*len);
    if (fread(buf, 1, *len, f) != *len) { perror("fread"); free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── Tiling name helper ──────────────────────────── */
static const char *tiling_name(const qtm_tiling_desc_t *d, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "%-8s a=%.5f p=%.3f", d->name, d->alpha, d->phase);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: coverage_bench <file>\n");
        return 1;
    }

    size_t in_len;
    uint8_t *data = read_file(argv[1], &in_len);
    if (!data) return 1;

    fprintf(stderr, "File: %s (%zu bytes)\n", argv[1], in_len);

    /* ── Tokenize & build codebooks ──────────────── */
    uint8_t *lowered = NULL;
    uint64_t lowered_len = 0;
    qtc_token_t *tokens = NULL;
    uint32_t n_tok = tokenize(data, (uint32_t)in_len, &lowered, &lowered_len, &tokens);
    free(tokens);
    (void)n_tok;

    const uint8_t **word_ptrs;
    uint16_t *word_lens;
    uint32_t nw = word_split(lowered, lowered_len, &word_ptrs, &word_lens);
    fprintf(stderr, "Words: %u\n\n", nw);

    qtc_cb_sizes_t sizes = auto_codebook_sizes(nw);
    qtc_cbs_t cbs;
    cbs_build(&cbs, word_ptrs, word_lens, nw, &sizes);
    free(word_ptrs);
    free(word_lens);

    /* ── Per-level bitsets: track unique matched positions ── */
    /* Levels: 2=bigram, 3=tri, 4=5g, ..., 11=144g */
    bitset_t seen[QTM_N_ENC_LEVELS];
    for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++) bs_init(&seen[lv], nw);

    /* Also track hierarchy-valid positions (regardless of codebook) */
    bitset_t valid[QTM_N_ENC_LEVELS];
    for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++) bs_init(&valid[lv], nw);

    qtm_tiling_desc_t descs[QTC_N_TILINGS];
    qtm_get_tiling_descs(descs);

    /* ── Print header ────────────────────────────── */
    printf("%-3s  %-28s  %6s  %6s  ", "#", "Tiling", "hier", "bi");
    static const char *deep_names[] = {"tri","5g","8g","13g","21g","34g","55g","89g","144g"};
    for (int i = 0; i < QTC_N_LEVELS; i++) printf("%7s ", deep_names[i]);
    printf(" %8s  %8s  %8s\n", "deep+", "deep_cum", "bi_cum");
    for (int i = 0; i < 140; i++) putchar('-');
    putchar('\n');

    double t0 = now_sec();

    for (int t = 0; t < QTC_N_TILINGS; t++) {
        uint32_t nt;
        qtc_tile_t *tiles = qtm_gen_tiling(&descs[t], nw, &nt);
        if (!tiles) continue;

        qtc_hierarchy_t hier;
        build_hierarchy(tiles, nt, QTC_MAX_HIER, &hier);
        qtc_deep_t dp = detect_deep_positions(tiles, nt, &hier);

        /* Count new unique codebook-matched positions per level */
        uint32_t new_matched[QTM_N_ENC_LEVELS];
        uint32_t new_valid_cnt[QTM_N_ENC_LEVELS];
        memset(new_matched, 0, sizeof(new_matched));
        memset(new_valid_cnt, 0, sizeof(new_valid_cnt));

        for (uint32_t ti = 0; ti < nt; ti++) {
            if (!tiles[ti].is_L) continue;
            uint32_t wpos = tiles[ti].wpos;

            /* Deep levels */
            for (int k = dp.max_k; k >= 1; k--) {
                if (!dp.can[k] || !dp.can[k][ti]) continue;
                if (k > QTC_N_LEVELS) continue;
                int gl = QTC_HIER_WORD_LENS[k];
                if (wpos + (uint32_t)gl > nw) continue;

                int enc_level = k + 2;

                /* Track hierarchy-valid position (new or not) */
                if (bs_set(&valid[enc_level], wpos))
                    new_valid_cnt[enc_level]++;

                /* Check codebook */
                if (cbs.ng_count[k - 1] == 0) continue;
                uint32_t cb_idx;
                if (nmap_get(&cbs.ng_maps[k - 1], wpos, &cb_idx)) {
                    if (bs_set(&seen[enc_level], wpos))
                        new_matched[enc_level]++;
                }
                /* Don't break — track ALL levels this tile is valid at */
            }

            /* Bigram */
            if (wpos + 2 <= nw) {
                if (bs_set(&valid[2], wpos))
                    new_valid_cnt[2]++;

                uint32_t bi_idx;
                if (u64map_get(&cbs.bi_map,
                               pack_bi(cbs.word_ids[wpos], cbs.word_ids[wpos + 1]),
                               &bi_idx)) {
                    if (bs_set(&seen[2], wpos))
                        new_matched[2]++;
                }
            }
        }

        /* Totals */
        uint32_t total_new_deep = 0;
        uint32_t total_cum_deep = 0;
        for (int lv = 3; lv < QTM_N_ENC_LEVELS; lv++) {
            total_new_deep += new_matched[lv];
            total_cum_deep += seen[lv].count;
        }

        /* Print row */
        char name[64];
        tiling_name(&descs[t], name, sizeof(name));
        printf("%-3d  %-28s  %6u  %6u  ", t, name,
               hier.n_levels, new_matched[2]);

        for (int i = 0; i < QTC_N_LEVELS; i++) {
            int lv = i + 3;
            printf("%7u ", new_matched[lv]);
        }
        printf(" %8u  %8u  %8u\n", total_new_deep, total_cum_deep, seen[2].count);

        free_deep(&dp, nt);
        free_hierarchy(&hier);
        free(tiles);
    }

    double elapsed = now_sec() - t0;

    /* ── Summary ─────────────────────────────────── */
    printf("\n");
    for (int i = 0; i < 100; i++) putchar('=');
    printf("\nFinal cumulative coverage (unique codebook-matched positions):\n");
    printf("  %-10s  %10s  %10s\n", "Level", "Positions", "Valid(hier)");
    for (int i = 0; i < 40; i++) putchar('-');
    putchar('\n');

    printf("  %-10s  %10u  %10u\n", "bigram", seen[2].count, valid[2].count);
    uint32_t total_deep = 0, total_valid = 0;
    for (int i = 0; i < QTC_N_LEVELS; i++) {
        int lv = i + 3;
        printf("  %-10s  %10u  %10u\n", deep_names[i], seen[lv].count, valid[lv].count);
        total_deep += seen[lv].count;
        total_valid += valid[lv].count;
    }
    printf("  %-10s  %10u  %10u\n", "TOTAL deep", total_deep, total_valid);
    printf("\n  Time: %.2fs\n", elapsed);

    /* Cleanup */
    for (int lv = 0; lv < QTM_N_ENC_LEVELS; lv++) { bs_free(&seen[lv]); bs_free(&valid[lv]); }
    cbs_free(&cbs);
    free(lowered);
    free(data);

    return 0;
}
