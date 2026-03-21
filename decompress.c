/*
 * QTC Multi-Tiling - Decompress implementation
 * Sequential decoding: reads (level, codebook_index) pairs.
 * No tiling needed — the encoding is self-describing.
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
#include <lzma.h>

#define UNI_TIER_SPLIT 4096

/* ── Recency cache (simplified MTF) for index decoding ── */
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

static inline bool is_alpha_or_hi_d(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 128;
}
static inline bool is_ws_d(uint8_t c) {
    return c == 32 || c == 10 || c == 13 || c == 9;
}

static inline void emit_word(qtc_buf_t *out, const qtc_cbs_t *cbs, uint32_t wid) {
    buf_append(out, cbs->pool + cbs->pool_offs[wid], cbs->pool_lens[wid]);
}

/* ══════════════════════════════════════════════════════
 * qtc_decompress — Multi-tiling version
 * ══════════════════════════════════════════════════════ */
uint8_t *qtc_decompress(const uint8_t *comp, size_t comp_len, size_t *out_len, bool verbose) {
    /* ── Parse header (45 bytes) ─────────────────── */
    if (comp_len < QTC_HEADER_SIZE || memcmp(comp, QTC_MAGIC, 4) != 0) {
        fprintf(stderr, "Bad magic or truncated file\n");
        return NULL;
    }

    size_t o = 4;
    uint64_t orig_size;    memcpy(&orig_size, &comp[o], 8); o += 8;
    uint64_t ld_len;       memcpy(&ld_len, &comp[o], 8); o += 8;
    uint32_t nw;           memcpy(&nw, &comp[o], 4); o += 4;
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

    /* ── Decompress codebook (LZMA) ────────────── */
    size_t cb_raw_len = 10 * cb_sz + 65536;
    uint8_t *cb_raw = (uint8_t *)malloc(cb_raw_len);
    {
        uint64_t memlimit = UINT64_MAX;
        size_t in_pos = 0, out_pos = 0;
        lzma_stream_buffer_decode(&memlimit, 0, NULL,
                                  cb_comp, &in_pos, cb_sz,
                                  cb_raw, &out_pos, cb_raw_len);
        cb_raw_len = out_pos;
    }

    qtc_cbs_t cbs;
    uint32_t cb_off;
    cbs_decode(&cbs, cb_raw, 0, &cb_off);
    free(cb_raw);

    /* ── Decompress escapes (LZMA) ──────────────── */
    uint8_t *esc_words = NULL;
    if (esc_sz > 0) {
        size_t memlimit = UINT64_MAX;
        size_t in_pos = 0;
        size_t out_pos = 0;
        size_t out_cap = (size_t)(ld_len * 2 + 65536);
        esc_words = (uint8_t *)malloc(out_cap);
        lzma_ret lr = lzma_stream_buffer_decode(
            &memlimit, 0, NULL,
            esc_comp, &in_pos, esc_sz,
            esc_words, &out_pos, out_cap);
        if (lr != LZMA_OK) {
            fprintf(stderr, "LZMA decode failed: %d\n", (int)lr);
        }
    }
    uint32_t esc_off = 0;

    /* ── Sequential decode (variable-alphabet) ───── */
    /* Order-2 context-conditioned level model: 12x12 models indexed by (prev_lv, prev_prev_lv) */
    qtc_vmodel_t level_models[QTM_N_ENC_LEVELS][QTM_N_ENC_LEVELS];
    for (int i = 0; i < QTM_N_ENC_LEVELS; i++)
        for (int j = 0; j < QTM_N_ENC_LEVELS; j++)
            vmodel_init(&level_models[i][j], QTM_N_ENC_LEVELS);

    /* Per-level index models conditioned on previous level */
    qtc_vmodel_t vidx[QTM_N_ENC_LEVELS][QTM_N_ENC_LEVELS];
    memset(vidx, 0, sizeof(vidx));
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
    vmodel_init(&match_off_nbits_model, 32);
    qtc_vmodel_t match_off_bit_model[8];
    for (int _i = 0; _i < 8; _i++) vmodel_init(&match_off_bit_model[_i], 2);
    qtc_vmodel_t match_len_model;
    vmodel_init(&match_len_model, 253);      /* match_len - 3, max 252 */

    #define LZ_MIN_MATCH 3

    qtc_decoder_t decoder;
    dec_init(&decoder, payload, payload_sz);

    qtc_buf_t decoded;
    buf_init(&decoded, (size_t)(ld_len + 256));

    /* N-gram word counts for levels 3..11 (tri=3, 5g=5, ..., 144g=144) */
    static const uint8_t ng_lens[QTC_N_LEVELS] = {3, 5, 8, 13, 21, 34, 55, 89, 144};

    /* Event history for LZ match replay */
    uint32_t dec_hist_cap = nw;  /* generous upper bound */
    uint8_t  *dec_levels = (uint8_t  *)malloc(dec_hist_cap * sizeof(uint8_t));
    uint32_t *dec_idxs   = (uint32_t *)malloc(dec_hist_cap * sizeof(uint32_t));
    uint32_t n_dec_events = 0;

    uint8_t prev_lv = 1;
    uint8_t prev_prev_lv = 1;
    uint32_t wp = 0;
    while (wp < nw) {
        /* Decode match flag first */
        uint32_t mflag = vdec_sym(&decoder, &match_flag_model);

        if (mflag == 1) {
            /* Match: decode offset (log-scale) and length */
            uint32_t nbits = vdec_sym(&decoder, &match_off_nbits_model) + 1;
            uint32_t off = 1;
            for (int b = (int)nbits - 2; b >= 0; b--)
                off = (off << 1) | vdec_sym(&decoder, &match_off_bit_model[b < 8 ? b : 7]);
            uint32_t mlen = vdec_sym(&decoder, &match_len_model) + LZ_MIN_MATCH;

            /* Replay events from history */
            uint32_t ref = n_dec_events - off;
            for (uint32_t mi = 0; mi < mlen; mi++) {
                uint8_t lv = dec_levels[ref + mi];
                uint32_t idx = dec_idxs[ref + mi];

                /* Store in history */
                dec_levels[n_dec_events] = lv;
                dec_idxs[n_dec_events]   = idx;
                n_dec_events++;

                /* Emit words and update caches, exactly as if decoded normally */
                switch (lv) {
                case 0: {
                    /* Escape: read from escape buffer */
                    uint16_t wlen;
                    memcpy(&wlen, &esc_words[esc_off], 2); esc_off += 2;
                    buf_append(&decoded, &esc_words[esc_off], wlen); esc_off += wlen;
                    wp += 1;
                    break;
                }
                case 1: {
                    rcache_use(&rcaches[1], idx);
                    emit_word(&decoded, &cbs, cbs.uni_wids[idx]);
                    wp += 1;
                    break;
                }
                case 2: {
                    rcache_use(&rcaches[2], idx);
                    emit_word(&decoded, &cbs, cbs.bi_wids[2 * idx]);
                    emit_word(&decoded, &cbs, cbs.bi_wids[2 * idx + 1]);
                    wp += 2;
                    break;
                }
                default: {
                    rcache_use(&rcaches[lv], idx);
                    int ng_lv = (int)lv - 3;
                    uint8_t ng = ng_lens[ng_lv];
                    for (uint8_t j = 0; j < ng; j++)
                        emit_word(&decoded, &cbs, cbs.ng_wids[ng_lv][idx * ng + j]);
                    wp += QTM_LEVEL_WORDS[lv];
                    break;
                }
                }

                /* Update level context */
                prev_prev_lv = prev_lv;
                prev_lv = lv;
            }
        } else {
            /* Normal event: decode as before */
            uint32_t lv = vdec_sym(&decoder, &level_models[prev_lv][prev_prev_lv]);
            uint8_t ctx_lv = prev_lv;
            uint32_t idx = 0; /* will be set below for non-escape */

            prev_prev_lv = prev_lv;
            prev_lv = (uint8_t)lv;

            switch (lv) {
            case 0: {
                /* Escape: read word from escape buffer */
                uint16_t wlen;
                memcpy(&wlen, &esc_words[esc_off], 2); esc_off += 2;
                buf_append(&decoded, &esc_words[esc_off], wlen); esc_off += wlen;
                wp += 1;
                break;
            }
            case 1: {
                uint32_t hit = vdec_sym(&decoder, &cache_hit_model[1]);
                if (hit == 0) {
                    uint32_t cpos = vdec_sym(&decoder, &cache_pos_model[1]);
                    idx = rcaches[1].entries[cpos];
                } else if (use_uni_tier) {
                    uint32_t tier = vdec_sym(&decoder, &uni_tier_model[ctx_lv]);
                    if (tier == 0)
                        idx = vdec_sym(&decoder, &vidx[1][ctx_lv]);
                    else
                        idx = vdec_sym(&decoder, &vidx_rare[ctx_lv]) + UNI_TIER_SPLIT;
                } else {
                    idx = vdec_sym(&decoder, &vidx[1][ctx_lv]);
                }
                rcache_use(&rcaches[1], idx);
                emit_word(&decoded, &cbs, cbs.uni_wids[idx]);
                wp += 1;
                break;
            }
            case 2: {
                uint32_t hit = vdec_sym(&decoder, &cache_hit_model[2]);
                if (hit == 0) {
                    uint32_t cpos = vdec_sym(&decoder, &cache_pos_model[2]);
                    idx = rcaches[2].entries[cpos];
                } else {
                    idx = vdec_sym(&decoder, &vidx[2][ctx_lv]);
                }
                rcache_use(&rcaches[2], idx);
                emit_word(&decoded, &cbs, cbs.bi_wids[2 * idx]);
                emit_word(&decoded, &cbs, cbs.bi_wids[2 * idx + 1]);
                wp += 2;
                break;
            }
            default: {
                uint32_t hit = vdec_sym(&decoder, &cache_hit_model[lv]);
                if (hit == 0) {
                    uint32_t cpos = vdec_sym(&decoder, &cache_pos_model[lv]);
                    idx = rcaches[lv].entries[cpos];
                } else {
                    idx = vdec_sym(&decoder, &vidx[lv][ctx_lv]);
                }
                rcache_use(&rcaches[lv], idx);
                int ng_lv = (int)lv - 3;
                uint8_t ng = ng_lens[ng_lv];
                for (uint8_t j = 0; j < ng; j++)
                    emit_word(&decoded, &cbs, cbs.ng_wids[ng_lv][idx * ng + j]);
                wp += QTM_LEVEL_WORDS[lv];
                break;
            }
            }

            /* Store in event history */
            dec_levels[n_dec_events] = (uint8_t)lv;
            dec_idxs[n_dec_events]   = idx;
            n_dec_events++;
        }
    }

    free(dec_levels);
    free(dec_idxs);

    /* ── Apply case ──────────────────────────────── */
    (void)ld_len;
    uint8_t *result;
    size_t result_len;

    if (!is_binary && ntok > 0) {
        uint8_t *cflags = dec_case(case_data, ntok, case_sz);
        qtc_buf_t res;
        buf_init(&res, (size_t)(orig_size + 256));

        uint32_t ti_idx = 0;
        size_t i = 0;
        while (i < decoded.len && ti_idx < ntok) {
            if (is_alpha_or_hi_d(decoded.data[i])) {
                size_t j = i + 1;
                while (j < decoded.len && is_alpha_or_hi_d(decoded.data[j])) j++;
                size_t k = j;
                while (k < decoded.len && is_ws_d(decoded.data[k])) k++;
                uint32_t alen;
                uint8_t *applied = apply_case(decoded.data + i, (uint32_t)(k - i), cflags[ti_idx], &alen);
                buf_append(&res, applied, alen);
                free(applied);
                ti_idx++; i = k;
            } else {
                size_t j = i + 1;
                while (j < decoded.len && is_ws_d(decoded.data[j])) j++;
                buf_append(&res, decoded.data + i, j - i);
                ti_idx++; i = j;
            }
        }

        free(cflags);
        result = res.data;
        result_len = res.len < (size_t)orig_size ? res.len : (size_t)orig_size;
    } else {
        result = decoded.data;
        decoded.data = NULL;
        result_len = decoded.len < (size_t)orig_size ? decoded.len : (size_t)orig_size;
    }

    /* ── Verify ──────────────────────────────────── */
    uint8_t md5[16];
    md5_hash(result, result_len, md5);
    bool ok = (memcmp(md5, md5_expected, 16) == 0);

    if (verbose)
        fprintf(stderr, "  Decompress: %zuB | %s\n", result_len, ok ? "PASS" : "FAIL");

    if (!ok) {
        fprintf(stderr, "Integrity check failed!\n");
        free(result);
        result = NULL;
        result_len = 0;
    }

    *out_len = result_len;

    /* Cleanup */
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
    free(esc_words);
    buf_free(&decoded);

    return result;
}
