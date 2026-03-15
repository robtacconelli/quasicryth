/*
 * QTC A/B Test: Fibonacci QC vs all-unigram baseline vs Period-5
 * Compares payload sizes using identical codebooks.
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
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ── Encode a unigram (word) via codebook or escape ── */
static void ab_encode_unigram(qtc_encoder_t *enc, uint32_t wid,
                               const qtc_cbs_t *cbs,
                               qtc_model_t *s_model, qtc_model_t *s_ext,
                               uint32_t *esc_count) {
    int32_t idx = (wid < cbs->uni_rmap_sz) ? cbs->uni_rmap[wid] : -1;
    if (idx < 0) {
        ac_enc_sym(enc, s_model, QTC_ESC_SYM);
        (*esc_count)++;
    } else {
        encode_index(enc, s_model, s_ext, (uint32_t)idx);
    }
}

/* ── Results structure ─── */
typedef struct {
    uint32_t payload;
    uint32_t esc_words;
    uint32_t hits[QTC_MAX_HIER + 1];   /* hits[1..7] */
    uint32_t bh;                        /* bigram hits */
    uint32_t uh;                        /* unigram hits */
    uint32_t nl, ns;                    /* L/S tile counts */
    double   phase;
    double   time;
} ab_result_t;

/* ══════════════════════════════════════════════════════
 * Mode A: All-Unigram (every word as S tile, no hierarchy)
 * ══════════════════════════════════════════════════════ */
static ab_result_t encode_all_unigram(const qtc_cbs_t *cbs) {
    double t0 = now_sec();
    ab_result_t r;
    memset(&r, 0, sizeof(r));

    qtc_model_t S_model, S_ext;
    model_init(&S_model);
    model_init(&S_ext);

    qtc_encoder_t encoder;
    enc_init(&encoder);

    for (uint32_t i = 0; i < cbs->n_words; i++) {
        uint32_t wid = cbs->word_ids[i];
        int32_t idx = (wid < cbs->uni_rmap_sz) ? cbs->uni_rmap[wid] : -1;
        if (idx >= 0) {
            encode_index(&encoder, &S_model, &S_ext, (uint32_t)idx);
            r.uh++;
        } else {
            ac_enc_sym(&encoder, &S_model, QTC_ESC_SYM);
            r.esc_words++;
        }
    }

    uint32_t payload_len;
    uint8_t *payload = enc_finish(&encoder, &payload_len);
    free(payload);
    enc_free(&encoder);

    r.payload = payload_len;
    r.ns = cbs->n_words;
    r.time = now_sec() - t0;
    return r;
}

/* ══════════════════════════════════════════════════════
 * Shared tiling encoder (used by both Period-5 and Fibonacci)
 * ══════════════════════════════════════════════════════ */
static ab_result_t encode_with_tiling(const qtc_cbs_t *cbs,
                                       qtc_tile_t *tiles, uint32_t n_tiles,
                                       uint32_t nw) {
    double t0 = now_sec();
    ab_result_t r;
    memset(&r, 0, sizeof(r));

    const uint32_t *wids = cbs->word_ids;

    /* Count L/S */
    for (uint32_t i = 0; i < n_tiles; i++) {
        if (tiles[i].is_L) r.nl++; else r.ns++;
    }

    /* Build hierarchy */
    qtc_hierarchy_t hier;
    build_hierarchy(tiles, n_tiles, QTC_MAX_HIER, &hier);

    uint8_t *tile_hctx = (uint8_t *)malloc(n_tiles);
    for (uint32_t ti = 0; ti < n_tiles; ti++)
        tile_hctx[ti] = get_hier_ctx(ti, &hier);

    qtc_deep_t dp = detect_deep_positions(tiles, n_tiles, &hier);

    /* Models */
    qtc_model_t L_models[8], S_models[8], L_ext, S_ext;
    for (int i = 0; i < 8; i++) { model_init(&L_models[i]); model_init(&S_models[i]); }
    model_init(&L_ext); model_init(&S_ext);

    qtc_model_t *lvl_flag = (qtc_model_t *)malloc((dp.max_k + 1) * sizeof(qtc_model_t));
    qtc_model_t *lvl_idx  = (qtc_model_t *)malloc((dp.max_k + 1) * sizeof(qtc_model_t));
    qtc_model_t *lvl_ext  = (qtc_model_t *)malloc((dp.max_k + 1) * sizeof(qtc_model_t));
    for (int k = 0; k <= dp.max_k; k++) {
        model_init(&lvl_flag[k]); model_init(&lvl_idx[k]); model_init(&lvl_ext[k]);
    }

    qtc_encoder_t encoder;
    enc_init(&encoder);

    int skip_count = 0;
    for (uint32_t ti = 0; ti < n_tiles; ti++) {
        if (skip_count > 0) { skip_count--; continue; }
        uint8_t hctx = tile_hctx[ti];
        uint32_t wpos = tiles[ti].wpos;

        if (tiles[ti].is_L) {
            bool hit = false;
            for (int k = dp.max_k; k >= 1; k--) {
                if (!dp.can[k] || !dp.can[k][ti]) continue;
                int gl = (k < QTC_MAX_HIER) ? QTC_HIER_WORD_LENS[k] : -1;
                if (gl < 0 || k > QTC_N_LEVELS) continue;
                if (wpos + (uint32_t)gl - 1 >= nw) continue;
                if (cbs->ng_count[k - 1] == 0) continue;

                uint32_t cb_idx;
                if (nmap_get(&cbs->ng_maps[k - 1], wpos, &cb_idx)) {
                    ac_enc_sym(&encoder, &lvl_flag[k], 1);
                    encode_index(&encoder, &lvl_idx[k], &lvl_ext[k], cb_idx);
                    skip_count = (int)dp.skip[k][ti];
                    r.hits[k]++;
                    hit = true; break;
                } else {
                    ac_enc_sym(&encoder, &lvl_flag[k], 0);
                }
            }
            if (hit) continue;

            /* Level 0: bigram */
            uint32_t bi_idx;
            if (u64map_get(&cbs->bi_map, pack_bi(wids[wpos], wids[wpos + 1]), &bi_idx)) {
                encode_index(&encoder, &L_models[hctx], &L_ext, bi_idx);
                r.bh++;
            } else {
                ac_enc_sym(&encoder, &L_models[hctx], QTC_ESC_SYM);
                ab_encode_unigram(&encoder, wids[wpos], cbs,
                                  &S_models[hctx], &S_ext, &r.esc_words);
                ab_encode_unigram(&encoder, wids[wpos + 1], cbs,
                                  &S_models[hctx], &S_ext, &r.esc_words);
            }
        } else {
            uint32_t wid = wids[wpos];
            int32_t uidx = (wid < cbs->uni_rmap_sz) ? cbs->uni_rmap[wid] : -1;
            if (uidx >= 0) {
                encode_index(&encoder, &S_models[hctx], &S_ext, (uint32_t)uidx);
                r.uh++;
            } else {
                ac_enc_sym(&encoder, &S_models[hctx], QTC_ESC_SYM);
                r.esc_words++;
            }
        }
    }

    uint32_t payload_len;
    uint8_t *payload = enc_finish(&encoder, &payload_len);
    free(payload);
    enc_free(&encoder);

    r.payload = payload_len;
    r.time = now_sec() - t0;

    /* Cleanup */
    free(lvl_flag); free(lvl_idx); free(lvl_ext);
    free(tile_hctx);
    free_deep(&dp, n_tiles);
    free_hierarchy(&hier);

    return r;
}

/* ══════════════════════════════════════════════════════
 * Mode B: Period-5 tiling (LLSLS repeated)
 * ══════════════════════════════════════════════════════ */
static ab_result_t encode_period5(const qtc_cbs_t *cbs, uint32_t nw) {
    static const bool pattern[5] = {true, true, false, true, false};
    uint32_t cap = nw;
    qtc_tile_t *tiles = (qtc_tile_t *)malloc(cap * sizeof(qtc_tile_t));
    uint32_t n_tiles = 0;
    uint32_t wpos = 0, k = 0;

    while (wpos < nw) {
        bool is_L = pattern[k % 5];
        uint32_t consume = is_L ? 2 : 1;
        if (wpos + consume > nw) {
            tiles[n_tiles].wpos = wpos;
            tiles[n_tiles].nwords = 1;
            tiles[n_tiles].is_L = false;
            n_tiles++; wpos++; k++;
            continue;
        }
        tiles[n_tiles].wpos = wpos;
        tiles[n_tiles].nwords = (uint8_t)consume;
        tiles[n_tiles].is_L = is_L;
        n_tiles++; wpos += consume; k++;
    }

    ab_result_t r = encode_with_tiling(cbs, tiles, n_tiles, nw);
    free(tiles);
    return r;
}

/* ══════════════════════════════════════════════════════
 * Mode C: Fibonacci QC (with phase search)
 * ══════════════════════════════════════════════════════ */
static ab_result_t encode_fibonacci(const qtc_cbs_t *cbs, uint32_t nw) {
    const uint32_t *wids = cbs->word_ids;

    /* Phase search */
    int best_phase_q = 0;
    int best_score = -1;

    for (int pq = 0; pq < QTC_N_PHASES; pq++) {
        double phase = (double)pq / QTC_N_PHASES;
        uint32_t nt;
        qtc_tile_t *tiles = qc_word_tiling(nw, phase, &nt);
        qtc_hierarchy_t hier;
        build_hierarchy(tiles, nt, QTC_MAX_HIER, &hier);
        qtc_deep_t dp = detect_deep_positions(tiles, nt, &hier);

        int score = 0;
        int skip_count = 0;
        for (uint32_t ti = 0; ti < nt; ti++) {
            if (skip_count > 0) { skip_count--; continue; }
            if (tiles[ti].is_L) {
                bool hit = false;
                uint32_t wpos = tiles[ti].wpos;
                for (int k = dp.max_k; k >= 1; k--) {
                    if (!dp.can[k] || !dp.can[k][ti]) continue;
                    int gl = (k < QTC_MAX_HIER) ? QTC_HIER_WORD_LENS[k] : -1;
                    if (gl < 0 || k > QTC_N_LEVELS) continue;
                    if (wpos + (uint32_t)gl - 1 >= nw) continue;
                    if (cbs->ng_count[k - 1] == 0) continue;
                    uint32_t cb_idx;
                    if (nmap_get(&cbs->ng_maps[k - 1], wpos, &cb_idx)) {
                        score += QTC_SCORE_BONUS[k];
                        skip_count = (int)dp.skip[k][ti];
                        hit = true; break;
                    }
                }
                if (!hit) {
                    uint32_t bi_idx;
                    if (u64map_get(&cbs->bi_map, pack_bi(wids[wpos], wids[wpos + 1]), &bi_idx))
                        score += 10;
                    else
                        score -= 1;
                }
            } else {
                uint32_t wid = wids[tiles[ti].wpos];
                if (wid < cbs->uni_rmap_sz && cbs->uni_rmap[wid] >= 0) score += 3;
            }
        }

        if (score > best_score) { best_score = score; best_phase_q = pq; }
        free_deep(&dp, nt);
        free_hierarchy(&hier);
        free(tiles);
    }

    /* Encode with best phase */
    double phase = (double)best_phase_q / QTC_N_PHASES;
    uint32_t n_tiles;
    qtc_tile_t *tiles = qc_word_tiling(nw, phase, &n_tiles);

    ab_result_t r = encode_with_tiling(cbs, tiles, n_tiles, nw);
    free(tiles);
    r.phase = phase;
    return r;
}

/* ══════════════════════════════════════════════════════
 * Print result
 * ══════════════════════════════════════════════════════ */
static void print_result(const char *label, const ab_result_t *r) {
    fprintf(stderr, "\n  %s\n", label);
    if (r->phase > 0)
        fprintf(stderr, "      Phase: %.4f, Tiles: L:%u S:%u\n", r->phase, r->nl, r->ns);
    else if (r->nl > 0)
        fprintf(stderr, "      Tiles: L:%u S:%u\n", r->nl, r->ns);

    static const char *ng_names[] = {"","3g","5g","8g","13g","21g","34g","55g","89g","144g"};
    for (int k = QTC_N_LEVELS; k >= 1; k--)
        if (r->hits[k] > 0)
            fprintf(stderr, "      %s-gram hits: %u\n", ng_names[k], r->hits[k]);
    if (r->bh > 0)
        fprintf(stderr, "      Bigram hits: %u, Unigram hits: %u\n", r->bh, r->uh);
    else if (r->uh > 0)
        fprintf(stderr, "      Unigram hits: %u\n", r->uh);
    fprintf(stderr, "      Payload: %u B, Escapes: %u words (%.1fs)\n",
            r->payload, r->esc_words, r->time);
}

/* ══════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════ */
static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(*len);
    if (fread(buf, 1, *len, f) != *len) { fclose(f); free(buf); return NULL; }
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ab_test <file>\n");
        return 1;
    }

    const char *fname = argv[1];
    size_t fsize;
    uint8_t *data = read_file(fname, &fsize);
    if (!data) return 1;

    fprintf(stderr, "\n  A/B Test: %s (%zu B)\n", fname, fsize);
    fprintf(stderr, "  ============================================================\n");

    /* ── Tokenize + word split ─── */
    double t0 = now_sec();
    uint8_t *lowered;
    uint32_t lowered_len;
    qtc_token_t *tokens;
    (void)tokenize(data, (uint32_t)fsize, &lowered, &lowered_len, &tokens);
    free(tokens);
    free(data);

    const uint8_t **word_ptrs;
    uint16_t *word_lens;
    uint32_t nw = word_split(lowered, lowered_len, &word_ptrs, &word_lens);

    /* ── Build codebooks ─── */
    qtc_cb_sizes_t sizes = auto_codebook_sizes(nw);
    qtc_cbs_t cbs;
    cbs_build(&cbs, word_ptrs, word_lens, nw, &sizes);
    free(word_ptrs);
    free(word_lens);
    free(lowered);

    fprintf(stderr, "  Codebooks built: %u words, %u uni, %u bi",
            nw, cbs.n_uni, cbs.n_bi);
    static const char *ng_names2[] = {"tri","5g","8g","13g","21g","34g","55g","89g","144g"};
    for (int lv = 0; lv < QTC_N_LEVELS; lv++)
        if (cbs.ng_count[lv]) fprintf(stderr, ", %u %s", cbs.ng_count[lv], ng_names2[lv]);
    fprintf(stderr, " (%.1fs)\n", now_sec() - t0);

    /* ── Run three modes ─── */
    fprintf(stderr, "\n  [A] All-Unigram...");
    ab_result_t r_uni = encode_all_unigram(&cbs);
    print_result("[A] All-Unigram", &r_uni);

    fprintf(stderr, "\n  [B] Period-5...");
    ab_result_t r_p5 = encode_period5(&cbs, nw);
    print_result("[B] Period-5 (LLSLS)", &r_p5);

    fprintf(stderr, "\n  [C] Fibonacci QC...");
    ab_result_t r_fib = encode_fibonacci(&cbs, nw);
    print_result("[C] Fibonacci QC", &r_fib);

    /* ── Summary ─── */
    fprintf(stderr, "\n  ============================================================\n");
    fprintf(stderr, "  RESULTS (payload-only comparison, same codebooks/escapes)\n");
    fprintf(stderr, "  ------------------------------------------------------------\n");
    fprintf(stderr, "  All-unigram:   %12u B  (baseline)\n", r_uni.payload);
    fprintf(stderr, "  Period-5:      %12u B  (delta: %+d B vs baseline)\n",
            r_p5.payload, (int)r_p5.payload - (int)r_uni.payload);
    fprintf(stderr, "  Fibonacci QC:  %12u B  (delta: %+d B vs baseline)\n",
            r_fib.payload, (int)r_fib.payload - (int)r_uni.payload);
    fprintf(stderr, "  ------------------------------------------------------------\n");
    fprintf(stderr, "  QC vs Unigram:  %+d B  (Fibonacci advantage)\n",
            (int)r_uni.payload - (int)r_fib.payload);
    fprintf(stderr, "  QC vs Period-5: %+d B  (aperiodic advantage)\n",
            (int)r_p5.payload - (int)r_fib.payload);
    fprintf(stderr, "  ============================================================\n");

    cbs_free(&cbs);
    return 0;
}
