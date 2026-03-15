/*
 * QTC - Decompress implementation
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
#include <zlib.h>
#include <bzlib.h>

static inline bool is_alpha_or_hi_d(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 128;
}
static inline bool is_ws_d(uint8_t c) {
    return c == 32 || c == 10 || c == 13 || c == 9;
}

/* Emit word bytes from word_id into output buffer */
static inline void emit_word(qtc_buf_t *out, const qtc_cbs_t *cbs, uint32_t wid) {
    buf_append(out, cbs->pool + cbs->pool_offs[wid], cbs->pool_lens[wid]);
}


/* ══════════════════════════════════════════════════════
 * qtc_decompress
 * ══════════════════════════════════════════════════════ */
uint8_t *qtc_decompress(const uint8_t *comp, size_t comp_len, size_t *out_len, bool verbose) {
    /* ── Parse header ────────────────────────────── */
    if (comp_len < QTC_HEADER_SIZE || memcmp(comp, QTC_MAGIC, 4) != 0) {
        fprintf(stderr, "Bad magic or truncated file\n");
        return NULL;
    }

    uint32_t o = 4;
    uint32_t orig_size;    memcpy(&orig_size, &comp[o], 4); o += 4;
    uint32_t ld_len;       memcpy(&ld_len, &comp[o], 4); o += 4;
    uint32_t nw;           memcpy(&nw, &comp[o], 4); o += 4;
    uint16_t phase_q;      memcpy(&phase_q, &comp[o], 2); o += 2;
    uint8_t flags = comp[o++];
    uint32_t ntok;         memcpy(&ntok, &comp[o], 4); o += 4;
    uint32_t payload_sz;   memcpy(&payload_sz, &comp[o], 4); o += 4;
    uint32_t case_sz;      memcpy(&case_sz, &comp[o], 4); o += 4;
    uint32_t cb_sz;        memcpy(&cb_sz, &comp[o], 4); o += 4;
    uint32_t esc_sz;       memcpy(&esc_sz, &comp[o], 4); o += 4;

    const uint8_t *payload = &comp[o]; o += payload_sz;
    const uint8_t *case_data = &comp[o]; o += case_sz;
    const uint8_t *cb_comp = &comp[o]; o += cb_sz;
    const uint8_t *esc_comp = &comp[o]; o += esc_sz;
    const uint8_t *md5_expected = &comp[o]; o += 16;

    bool is_binary = (flags & 0x02) != 0;
    double phase = (double)phase_q / QTC_N_PHASES;

    /* ── Decompress codebook ─────────────────────── */
    uLongf cb_raw_len = 10 * cb_sz + 65536;
    uint8_t *cb_raw = (uint8_t *)malloc(cb_raw_len);
    uncompress(cb_raw, &cb_raw_len, cb_comp, cb_sz);

    qtc_cbs_t cbs;
    uint32_t cb_off;
    cbs_decode(&cbs, cb_raw, 0, &cb_off);
    free(cb_raw);

    /* ── Decompress escapes ──────────────────────── */
    uint8_t *esc_words = NULL;
    if (esc_sz > 0) {
        unsigned int bz_len = ld_len * 2 + 65536;
        esc_words = (uint8_t *)malloc(bz_len);
        BZ2_bzBuffToBuffDecompress((char *)esc_words, &bz_len,
                                   (char *)esc_comp, esc_sz, 0, 0);
    }
    uint32_t esc_off = 0;

    /* ── Build tiling + hierarchy ────────────────── */
    uint32_t n_tiles;
    qtc_tile_t *tiles = qc_word_tiling(nw, phase, &n_tiles);
    qtc_hierarchy_t hier;
    build_hierarchy(tiles, n_tiles, QTC_MAX_HIER, &hier);

    uint8_t *tile_hctx = (uint8_t *)malloc(n_tiles);
    for (uint32_t ti = 0; ti < n_tiles; ti++)
        tile_hctx[ti] = get_hier_ctx(ti, &hier);

    qtc_deep_t dp = detect_deep_positions(tiles, n_tiles, &hier);

    /* ── Decode tiles ────────────────────────────── */
    qtc_model_t L_models[8], S_models[8], L_ext, S_ext;
    for (int i = 0; i < 8; i++) { model_init(&L_models[i]); model_init(&S_models[i]); }
    model_init(&L_ext); model_init(&S_ext);

    qtc_model_t *lvl_flag = (qtc_model_t *)malloc((dp.max_k + 1) * sizeof(qtc_model_t));
    qtc_model_t *lvl_idx  = (qtc_model_t *)malloc((dp.max_k + 1) * sizeof(qtc_model_t));
    qtc_model_t *lvl_ext  = (qtc_model_t *)malloc((dp.max_k + 1) * sizeof(qtc_model_t));
    for (int k = 0; k <= dp.max_k; k++) {
        model_init(&lvl_flag[k]); model_init(&lvl_idx[k]); model_init(&lvl_ext[k]);
    }

    qtc_decoder_t decoder;
    dec_init(&decoder, payload, payload_sz);

    qtc_buf_t decoded;
    buf_init(&decoded, ld_len + 256);

    static const uint8_t ng_lens[QTC_N_LEVELS] = {3, 5, 8, 13, 21, 34, 55, 89, 144};

    int skip_count = 0;
    for (uint32_t ti = 0; ti < n_tiles; ti++) {
        if (skip_count > 0) { skip_count--; continue; }
        uint8_t hctx = tile_hctx[ti];

        if (tiles[ti].is_L) {
            bool hit = false;
            /* Try deepest level first (cap at QTC_N_LEVELS = 7) */
            for (int k = dp.max_k; k >= 1; k--) {
                if (!dp.can[k] || !dp.can[k][ti]) continue;
                int gl = (k < QTC_MAX_HIER) ? QTC_HIER_WORD_LENS[k] : -1;
                if (gl < 0 || k > QTC_N_LEVELS) continue;
                if (tiles[ti].wpos + (uint32_t)gl - 1 >= nw) continue;
                if (cbs.ng_count[k - 1] == 0) continue;
                uint8_t flag = ac_dec_sym(&decoder, &lvl_flag[k]);
                if (flag == 1) {
                    int32_t idx = decode_index(&decoder, &lvl_idx[k], &lvl_ext[k]);
                    /* Emit all words of this n-gram */
                    int lv = k - 1;
                    uint8_t ng = ng_lens[lv];
                    for (uint8_t j = 0; j < ng; j++)
                        emit_word(&decoded, &cbs, cbs.ng_wids[lv][(uint32_t)idx * ng + j]);
                    skip_count = (int)dp.skip[k][ti];
                    hit = true; break;
                }
            }
            if (hit) continue;

            /* Level 0: bigram */
            int32_t idx = decode_index(&decoder, &L_models[hctx], &L_ext);
            if (idx == -1) {
                /* ESC: two unigrams */
                for (int w = 0; w < 2; w++) {
                    int32_t uidx = decode_index(&decoder, &S_models[hctx], &S_ext);
                    if (uidx == -1) {
                        /* Escaped word from bz2 stream */
                        uint16_t wlen;
                        memcpy(&wlen, &esc_words[esc_off], 2); esc_off += 2;
                        buf_append(&decoded, &esc_words[esc_off], wlen); esc_off += wlen;
                    } else {
                        emit_word(&decoded, &cbs, cbs.uni_wids[(uint32_t)uidx]);
                    }
                }
            } else {
                emit_word(&decoded, &cbs, cbs.bi_wids[2 * (uint32_t)idx]);
                emit_word(&decoded, &cbs, cbs.bi_wids[2 * (uint32_t)idx + 1]);
            }
        } else {
            /* S tile: unigram */
            int32_t idx = decode_index(&decoder, &S_models[hctx], &S_ext);
            if (idx == -1) {
                uint16_t wlen;
                memcpy(&wlen, &esc_words[esc_off], 2); esc_off += 2;
                buf_append(&decoded, &esc_words[esc_off], wlen); esc_off += wlen;
            } else {
                emit_word(&decoded, &cbs, cbs.uni_wids[(uint32_t)idx]);
            }
        }
    }

    /* ── Apply case ──────────────────────────────── */
    (void)ld_len; /* used only in debug builds */
    uint8_t *result;
    uint32_t result_len;

    if (!is_binary && ntok > 0) {
        uint8_t *cflags = dec_case(case_data, ntok, case_sz);
        qtc_buf_t res;
        buf_init(&res, orig_size + 256);

        uint32_t ti_idx = 0;
        uint32_t i = 0;
        while (i < decoded.len && ti_idx < ntok) {
            if (is_alpha_or_hi_d(decoded.data[i])) {
                uint32_t j = i + 1;
                while (j < decoded.len && is_alpha_or_hi_d(decoded.data[j])) j++;
                uint32_t k = j;
                while (k < decoded.len && is_ws_d(decoded.data[k])) k++;
                uint32_t alen;
                uint8_t *applied = apply_case(decoded.data + i, k - i, cflags[ti_idx], &alen);
                buf_append(&res, applied, alen);
                free(applied);
                ti_idx++; i = k;
            } else {
                uint32_t j = i + 1;
                while (j < decoded.len && is_ws_d(decoded.data[j])) j++;
                buf_append(&res, decoded.data + i, j - i);
                ti_idx++; i = j;
            }
        }

        free(cflags);
        result = res.data;
        result_len = res.len < orig_size ? res.len : orig_size;
    } else {
        result = decoded.data;
        decoded.data = NULL;
        result_len = decoded.len < orig_size ? decoded.len : orig_size;
    }

    /* ── Verify ──────────────────────────────────── */
    uint8_t md5[16];
    md5_hash(result, result_len, md5);
    bool ok = (memcmp(md5, md5_expected, 16) == 0);

    if (verbose)
        fprintf(stderr, "  Decompress: %uB | %s\n", result_len, ok ? "PASS" : "FAIL");

    if (!ok) {
        fprintf(stderr, "Integrity check failed!\n");
        free(result);
        result = NULL;
        result_len = 0;
    }

    *out_len = result_len;

    /* Cleanup */
    free(lvl_flag); free(lvl_idx); free(lvl_ext);
    free(tile_hctx);
    free_deep(&dp, n_tiles);
    free_hierarchy(&hier);
    free(tiles);
    cbs_free(&cbs);
    free(esc_words);
    buf_free(&decoded);

    return result;
}
