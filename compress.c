/*
 * QTC - Compress implementation
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

/* ── Encode a unigram (word) via codebook or escape ── */
static void encode_unigram(qtc_encoder_t *enc, uint32_t wid,
                           const qtc_cbs_t *cbs,
                           qtc_model_t *s_model, qtc_model_t *s_ext,
                           qtc_buf_t *esc_buf) {
    int32_t idx = (wid < cbs->uni_rmap_sz) ? cbs->uni_rmap[wid] : -1;
    if (idx < 0) {
        ac_enc_sym(enc, s_model, QTC_ESC_SYM);
        /* Store escaped word bytes in esc_buf */
        uint16_t wlen = cbs->pool_lens[wid];
        buf_write16(esc_buf, wlen);
        buf_append(esc_buf, cbs->pool + cbs->pool_offs[wid], wlen);
    } else {
        encode_index(enc, s_model, s_ext, (uint32_t)idx);
    }
}

/* ══════════════════════════════════════════════════════
 * qtc_compress
 * ══════════════════════════════════════════════════════ */
uint8_t *qtc_compress(const uint8_t *data, size_t len, size_t *out_len, bool verbose) {
    uint32_t n = (uint32_t)len;

    if (verbose) fprintf(stderr, "\n  QTC v%s -- %u bytes\n", QTC_VERSION, n);

    /* ── Case separation ─────────────────────────── */
    uint8_t *lowered = NULL;
    uint32_t lowered_len = 0;
    qtc_token_t *tokens = NULL;
    uint32_t n_tok = 0;
    uint8_t *case_enc = NULL;
    uint32_t case_enc_len = 0;
    bool is_binary = false;

    /* Check for binary */
    uint32_t check_len = n < 8192 ? n : 8192;
    for (uint32_t i = 0; i < check_len; i++) {
        if (data[i] == 0) { is_binary = true; break; }
    }

    if (!is_binary) {
        n_tok = tokenize(data, n, &lowered, &lowered_len, &tokens);
        /* Encode case flags */
        uint8_t *case_flags = (uint8_t *)malloc(n_tok);
        for (uint32_t i = 0; i < n_tok; i++) case_flags[i] = tokens[i].case_flag;
        case_enc = enc_case(case_flags, n_tok, &case_enc_len);
        free(case_flags);
        if (verbose)
            fprintf(stderr, "  [1] Case: %u tokens -> %uB\n", n_tok, case_enc_len);
    } else {
        lowered = (uint8_t *)malloc(n);
        memcpy(lowered, data, n);
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

    /* Serialize codebooks + zlib compress */
    uint32_t cb_raw_len;
    uint8_t *cb_raw = cbs_encode(&cbs, &cb_raw_len);
    uLongf cb_comp_len = compressBound(cb_raw_len);
    uint8_t *cb_data = (uint8_t *)malloc(cb_comp_len);
    compress2(cb_data, &cb_comp_len, cb_raw, cb_raw_len, 9);
    free(cb_raw);

    if (verbose) {
        fprintf(stderr, "  [3] Codebook: %u uni, %u bi", cbs.n_uni, cbs.n_bi);
        static const char *ng_names[] = {"tri","5g","8g","13g","21g","34g","55g","89g","144g"};
        for (int lv = 0; lv < QTC_N_LEVELS; lv++)
            if (cbs.ng_count[lv]) fprintf(stderr, ", %u %s", cbs.ng_count[lv], ng_names[lv]);
        fprintf(stderr, " -> %luB\n", (unsigned long)cb_comp_len);
    }

    const uint32_t *wids = cbs.word_ids;

    /* ── Phase search ────────────────────────────── */
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
                    if (wpos + gl - 1 >= nw) continue;
                    if (cbs.ng_count[k - 1] == 0) continue;
                    /* Check if this n-gram is in codebook */
                    uint32_t cb_idx;
                    if (nmap_get(&cbs.ng_maps[k - 1], wpos, &cb_idx)) {
                        score += QTC_SCORE_BONUS[k];
                        skip_count = (int)dp.skip[k][ti];
                        hit = true; break;
                    }
                }
                if (!hit) {
                    uint32_t bi_idx;
                    if (u64map_get(&cbs.bi_map, pack_bi(wids[wpos], wids[wpos + 1]), &bi_idx))
                        score += 10;
                    else
                        score -= 1;
                }
            } else {
                uint32_t wid = wids[tiles[ti].wpos];
                if (wid < cbs.uni_rmap_sz && cbs.uni_rmap[wid] >= 0) score += 3;
            }
        }

        if (score > best_score) { best_score = score; best_phase_q = pq; }
        free_deep(&dp, nt);
        free_hierarchy(&hier);
        free(tiles);
    }

    /* ── Build final tiling with best phase ──────── */
    double phase = (double)best_phase_q / QTC_N_PHASES;
    uint32_t n_tiles;
    qtc_tile_t *tiles = qc_word_tiling(nw, phase, &n_tiles);
    qtc_hierarchy_t hier;
    build_hierarchy(tiles, n_tiles, QTC_MAX_HIER, &hier);

    uint8_t *tile_hctx = (uint8_t *)malloc(n_tiles);
    for (uint32_t ti = 0; ti < n_tiles; ti++)
        tile_hctx[ti] = get_hier_ctx(ti, &hier);

    qtc_deep_t dp = detect_deep_positions(tiles, n_tiles, &hier);

    if (verbose) {
        uint32_t nl = 0;
        for (uint32_t i = 0; i < n_tiles; i++) if (tiles[i].is_L) nl++;
        fprintf(stderr, "  [4] QC tiling: %u tiles (L:%u S:%u), phase=%.4f\n",
                n_tiles, nl, n_tiles - nl, phase);
        fprintf(stderr, "      Hierarchy: %d levels\n", hier.n_levels);
    }

    /* ── Encode tiles ────────────────────────────── */
    qtc_model_t L_models[8], S_models[8], L_ext, S_ext;
    for (int i = 0; i < 8; i++) { model_init(&L_models[i]); model_init(&S_models[i]); }
    model_init(&L_ext); model_init(&S_ext);

    /* Per-level models for deep hierarchy */
    qtc_model_t *lvl_flag = (qtc_model_t *)malloc((dp.max_k + 1) * sizeof(qtc_model_t));
    qtc_model_t *lvl_idx  = (qtc_model_t *)malloc((dp.max_k + 1) * sizeof(qtc_model_t));
    qtc_model_t *lvl_ext  = (qtc_model_t *)malloc((dp.max_k + 1) * sizeof(qtc_model_t));
    for (int k = 0; k <= dp.max_k; k++) {
        model_init(&lvl_flag[k]); model_init(&lvl_idx[k]); model_init(&lvl_ext[k]);
    }

    qtc_encoder_t encoder;
    enc_init(&encoder);
    qtc_buf_t esc_buf;
    buf_init(&esc_buf, 4096);
    uint32_t esc_count = 0;

    int skip_count = 0;
    for (uint32_t ti = 0; ti < n_tiles; ti++) {
        if (skip_count > 0) { skip_count--; continue; }
        uint8_t hctx = tile_hctx[ti];
        uint32_t wpos = tiles[ti].wpos;

        if (tiles[ti].is_L) {
            bool hit = false;
            /* Try deepest level first */
            for (int k = dp.max_k; k >= 1; k--) {
                if (!dp.can[k] || !dp.can[k][ti]) continue;
                int gl = (k < QTC_MAX_HIER) ? QTC_HIER_WORD_LENS[k] : -1;
                if (gl < 0 || k > QTC_N_LEVELS) continue;
                if (wpos + (uint32_t)gl - 1 >= nw) continue;
                if (cbs.ng_count[k - 1] == 0) continue;

                uint32_t cb_idx;
                if (nmap_get(&cbs.ng_maps[k - 1], wpos, &cb_idx)) {
                    ac_enc_sym(&encoder, &lvl_flag[k], 1);
                    encode_index(&encoder, &lvl_idx[k], &lvl_ext[k], cb_idx);
                    skip_count = (int)dp.skip[k][ti];
                    hit = true; break;
                } else {
                    ac_enc_sym(&encoder, &lvl_flag[k], 0);
                }
            }
            if (hit) continue;

            /* Level 0: bigram */
            uint32_t bi_idx;
            if (u64map_get(&cbs.bi_map, pack_bi(wids[wpos], wids[wpos + 1]), &bi_idx)) {
                encode_index(&encoder, &L_models[hctx], &L_ext, bi_idx);
            } else {
                ac_enc_sym(&encoder, &L_models[hctx], QTC_ESC_SYM);
                encode_unigram(&encoder, wids[wpos], &cbs,
                              &S_models[hctx], &S_ext, &esc_buf);
                encode_unigram(&encoder, wids[wpos + 1], &cbs,
                              &S_models[hctx], &S_ext, &esc_buf);
            }
        } else {
            uint32_t wid = wids[wpos];
            int32_t uidx = (wid < cbs.uni_rmap_sz) ? cbs.uni_rmap[wid] : -1;
            if (uidx >= 0) {
                encode_index(&encoder, &S_models[hctx], &S_ext, (uint32_t)uidx);
            } else {
                ac_enc_sym(&encoder, &S_models[hctx], QTC_ESC_SYM);
                uint16_t wlen = cbs.pool_lens[wid];
                buf_write16(&esc_buf, wlen);
                buf_append(&esc_buf, cbs.pool + cbs.pool_offs[wid], wlen);
                esc_count++;
            }
        }
    }

    uint32_t payload_len;
    uint8_t *payload = enc_finish(&encoder, &payload_len);

    /* bz2 compress escape data */
    uint8_t *esc_data = NULL;
    uint32_t esc_data_len = 0;
    if (esc_buf.len > 0) {
        unsigned int bz_len = esc_buf.len + esc_buf.len / 100 + 600 + 1;
        esc_data = (uint8_t *)malloc(bz_len);
        BZ2_bzBuffToBuffCompress((char *)esc_data, &bz_len,
                                 (char *)esc_buf.data, esc_buf.len, 9, 0, 30);
        esc_data_len = bz_len;
    }

    if (verbose) {
        fprintf(stderr, "  [5] Payload: %uB\n", payload_len);
        fprintf(stderr, "  [6] Escapes: %u words, %uB raw -> %uB bz2\n",
                esc_count, esc_buf.len, esc_data_len);
    }

    /* ── Assemble output ─────────────────────────── */
    uint8_t flags = 0;
    if (is_binary) flags |= 0x02;

    uint32_t total_len = QTC_HEADER_SIZE + payload_len + case_enc_len +
                         (uint32_t)cb_comp_len + esc_data_len + 16;
    uint8_t *out = (uint8_t *)malloc(total_len);
    uint32_t o = 0;

    memcpy(out + o, QTC_MAGIC, 4); o += 4;
    memcpy(out + o, &n, 4); o += 4;
    uint32_t ld_len32 = lowered_len;
    memcpy(out + o, &ld_len32, 4); o += 4;
    memcpy(out + o, &nw, 4); o += 4;
    uint16_t pq16 = (uint16_t)best_phase_q;
    memcpy(out + o, &pq16, 2); o += 2;
    out[o++] = flags;
    memcpy(out + o, &n_tok, 4); o += 4;
    memcpy(out + o, &payload_len, 4); o += 4;
    memcpy(out + o, &case_enc_len, 4); o += 4;
    uint32_t cb_len32 = (uint32_t)cb_comp_len;
    memcpy(out + o, &cb_len32, 4); o += 4;
    memcpy(out + o, &esc_data_len, 4); o += 4;

    memcpy(out + o, payload, payload_len); o += payload_len;
    if (case_enc_len) { memcpy(out + o, case_enc, case_enc_len); o += case_enc_len; }
    memcpy(out + o, cb_data, cb_comp_len); o += (uint32_t)cb_comp_len;
    if (esc_data_len) { memcpy(out + o, esc_data, esc_data_len); o += esc_data_len; }

    /* MD5 checksum */
    uint8_t md5[16];
    md5_hash(data, n, md5);
    memcpy(out + o, md5, 16); o += 16;

    *out_len = o;

    if (verbose) {
        double ratio = 100.0 * o / n;
        fprintf(stderr, "\n  ==================================================\n");
        fprintf(stderr, "  Original:   %10u B\n", n);
        fprintf(stderr, "  Compressed: %10u B (%.2f%%)\n", o, ratio);
        fprintf(stderr, "  --------------------------------------------------\n");
        fprintf(stderr, "  Payload:    %10u B  (QC word-level AC)\n", payload_len);
        fprintf(stderr, "  Escapes:    %10u B  (bz2)\n", esc_data_len);
        fprintf(stderr, "  Codebook:   %10lu B\n", (unsigned long)cb_comp_len);
        fprintf(stderr, "  Case (AC):  %10u B\n", case_enc_len);
        fprintf(stderr, "  QC params:  phase=%.4f\n", phase);
        fprintf(stderr, "  ==================================================\n");
    }

    /* Cleanup */
    free(payload);
    free(esc_data);
    buf_free(&esc_buf);
    free(lvl_flag); free(lvl_idx); free(lvl_ext);
    free(tile_hctx);
    free_deep(&dp, n_tiles);
    free_hierarchy(&hier);
    free(tiles);
    cbs_free(&cbs);
    free(cb_data);
    free(case_enc);
    free(lowered);

    return out;
}
