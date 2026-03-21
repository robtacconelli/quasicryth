/*
 * QTC - Tokenizer implementation
 */
#include "tok.h"
#include "ac.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ══════════════════════════════════════════════════════
 * Helpers
 * ══════════════════════════════════════════════════════ */
static inline bool is_alpha_or_hi(uint8_t c) {
    return isalpha(c) || c >= 128;
}
static inline bool is_ws(uint8_t c) {
    return c == 32 || c == 10 || c == 13 || c == 9;
}

/* ══════════════════════════════════════════════════════
 * Apply case
 * ══════════════════════════════════════════════════════ */
uint8_t *apply_case(const uint8_t *data, uint32_t len, uint8_t flag, uint32_t *out_len) {
    uint8_t *out = (uint8_t *)malloc(len);
    memcpy(out, data, len);
    *out_len = len;
    if (flag == 0) return out;
    if (flag == 2) {
        for (uint32_t i = 0; i < len; i++) out[i] = (uint8_t)toupper(out[i]);
        return out;
    }
    /* flag == 1: capitalize first alpha */
    for (uint32_t i = 0; i < len; i++) {
        if (isalpha(out[i])) { out[i] = (uint8_t)toupper(out[i]); break; }
    }
    return out;
}

/* Check if apply_case(lowered, flag) == original */
static bool case_roundtrips(const uint8_t *orig, const uint8_t *low,
                            uint32_t len, uint8_t flag) {
    uint32_t cl;
    uint8_t *check = apply_case(low, len, flag, &cl);
    bool ok = (cl == len && memcmp(check, orig, len) == 0);
    free(check);
    return ok;
}

/* ══════════════════════════════════════════════════════
 * Tokenize with case separation
 * ══════════════════════════════════════════════════════ */
uint32_t tokenize(const uint8_t *data, uint64_t len,
                  uint8_t **lowered_out, uint64_t *lowered_len,
                  qtc_token_t **tokens_out) {
    /* Allocate lowered buffer (same size as input) */
    uint8_t *low = (uint8_t *)malloc((size_t)(len + 1));
    uint64_t low_len = 0;

    /* Token array (estimated) */
    size_t tok_cap = (size_t)(len / 4) + 16;
    qtc_token_t *toks = (qtc_token_t *)malloc(tok_cap * sizeof(qtc_token_t));
    uint32_t n_tok = 0;

    uint64_t i = 0;
    while (i < len) {
        if (n_tok >= tok_cap) {
            tok_cap += tok_cap / 2;  /* grow 1.5x to avoid overflow */
            toks = (qtc_token_t *)realloc(toks, tok_cap * sizeof(qtc_token_t));
        }

        if (is_alpha_or_hi(data[i])) {
            /* Word part (alpha) */
            uint64_t j = i + 1;
            while (j < len && is_alpha_or_hi(data[j])) j++;
            uint64_t k = j;
            while (k < len && is_ws(data[k])) k++;

            /* Determine case flag from the word-only part [i..j) */
            const uint8_t *wp = &data[i];
            uint32_t wp_len = (uint32_t)(j - i);
            uint32_t full_len = (uint32_t)(k - i);

            /* Check all-lower */
            bool all_lower = true;
            for (uint32_t m = 0; m < wp_len; m++)
                if (isupper(wp[m])) { all_lower = false; break; }

            uint8_t cflag;
            if (all_lower) {
                cflag = 0;
            } else {
                /* Check all-upper */
                bool all_upper = true;
                for (uint32_t m = 0; m < wp_len; m++)
                    if (islower(wp[m])) { all_upper = false; break; }
                if (all_upper && wp_len > 1) cflag = 2;
                else if (isupper(wp[0])) cflag = 1;
                else cflag = 0;
            }

            /* Make lowered version of full token [i..k) */
            uint64_t low_start = low_len;
            for (uint64_t m = i; m < k; m++)
                low[low_len++] = (uint8_t)tolower(data[m]);

            /* Verify roundtrip */
            if (!case_roundtrips(&data[i], &low[low_start], full_len, cflag)) {
                /* Fallback: store as-is with flag 0 */
                cflag = 0;
                low_len = low_start;
                for (uint64_t m = i; m < k; m++) low[low_len++] = data[m];
            }

            toks[n_tok].data = &low[low_start];
            toks[n_tok].len = full_len;
            toks[n_tok].case_flag = cflag;
            n_tok++;
            i = k;
        } else {
            /* Non-alpha token */
            uint64_t j = i + 1;
            while (j < len && is_ws(data[j])) j++;
            uint64_t low_start = low_len;
            for (uint64_t m = i; m < j; m++) low[low_len++] = data[m];
            toks[n_tok].data = &low[low_start];
            toks[n_tok].len = (uint32_t)(j - i);
            toks[n_tok].case_flag = 0;
            n_tok++;
            i = j;
        }
    }

    *lowered_out = low;
    *lowered_len = low_len;
    *tokens_out = toks;
    return n_tok;
}

/* ══════════════════════════════════════════════════════
 * Word split
 * ══════════════════════════════════════════════════════ */
uint32_t word_split(const uint8_t *data, uint64_t len,
                    const uint8_t ***words_out, uint16_t **lens_out) {
    size_t cap = (size_t)(len / 4) + 16;
    const uint8_t **wp = (const uint8_t **)malloc(cap * sizeof(uint8_t *));
    uint16_t *wl = (uint16_t *)malloc(cap * sizeof(uint16_t));
    uint32_t nw = 0;
    uint64_t i = 0;

    while (i < len) {
        if (nw >= cap) {
            cap += cap / 2;
            wp = (const uint8_t **)realloc(wp, cap * sizeof(uint8_t *));
            wl = (uint16_t *)realloc(wl, cap * sizeof(uint16_t));
        }
        if (is_alpha_or_hi(data[i])) {
            uint64_t j = i + 1;
            while (j < len && is_alpha_or_hi(data[j])) j++;
            uint64_t k = j;
            while (k < len && is_ws(data[k])) k++;
            wp[nw] = &data[i];
            wl[nw] = (uint16_t)(k - i);
            nw++;
            i = k;
        } else {
            uint64_t j = i + 1;
            while (j < len && is_ws(data[j])) j++;
            wp[nw] = &data[i];
            wl[nw] = (uint16_t)(j - i);
            nw++;
            i = j;
        }
    }

    *words_out = wp;
    *lens_out = wl;
    return nw;
}

/* ══════════════════════════════════════════════════════
 * Case encoding (order-2 adaptive, 24-bit AC, 3-symbol vmodel)
 * ══════════════════════════════════════════════════════ */
uint8_t *enc_case(const uint8_t *flags, uint32_t n, uint32_t *out_len) {
    qtc_vmodel_t models[3][3];  /* [prev_prev][prev] */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            vmodel_init(&models[i][j], 3);

    qtc_encoder_t enc;
    enc_init(&enc);

    uint8_t pp = 0, p = 0;
    for (uint32_t i = 0; i < n; i++) {
        venc_sym(&enc, &models[pp][p], flags[i]);
        pp = p;
        p = flags[i];
    }

    uint8_t *data = enc_finish(&enc, out_len);

    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            vmodel_free(&models[i][j]);

    return data;
}

uint8_t *dec_case(const uint8_t *data, uint32_t n, uint32_t data_len) {
    qtc_vmodel_t models[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            vmodel_init(&models[i][j], 3);

    qtc_decoder_t dec;
    dec_init(&dec, data, data_len);

    uint8_t *flags_out = (uint8_t *)malloc(n);
    uint8_t pp = 0, p = 0;
    for (uint32_t i = 0; i < n; i++) {
        flags_out[i] = (uint8_t)vdec_sym(&dec, &models[pp][p]);
        pp = p;
        p = flags_out[i];
    }

    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            vmodel_free(&models[i][j]);

    return flags_out;
}
