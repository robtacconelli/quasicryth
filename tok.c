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
uint32_t tokenize(const uint8_t *data, uint32_t len,
                  uint8_t **lowered_out, uint32_t *lowered_len,
                  qtc_token_t **tokens_out) {
    /* Allocate lowered buffer (same size as input) */
    uint8_t *low = (uint8_t *)malloc(len + 1);
    uint32_t low_len = 0;

    /* Token array (estimated) */
    uint32_t tok_cap = len / 4 + 16;
    qtc_token_t *toks = (qtc_token_t *)malloc(tok_cap * sizeof(qtc_token_t));
    uint32_t n_tok = 0;

    uint32_t i = 0;
    while (i < len) {
        if (n_tok >= tok_cap) {
            tok_cap *= 2;
            toks = (qtc_token_t *)realloc(toks, tok_cap * sizeof(qtc_token_t));
        }

        if (is_alpha_or_hi(data[i])) {
            /* Word part (alpha) */
            uint32_t j = i + 1;
            while (j < len && is_alpha_or_hi(data[j])) j++;
            uint32_t k = j;
            while (k < len && is_ws(data[k])) k++;

            /* Determine case flag from the word-only part [i..j) */
            const uint8_t *wp = &data[i];
            uint32_t wp_len = j - i;
            uint32_t full_len = k - i;

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
            uint32_t low_start = low_len;
            for (uint32_t m = i; m < k; m++)
                low[low_len++] = (uint8_t)tolower(data[m]);

            /* Verify roundtrip */
            if (!case_roundtrips(&data[i], &low[low_start], full_len, cflag)) {
                /* Fallback: store as-is with flag 0 */
                cflag = 0;
                low_len = low_start;
                for (uint32_t m = i; m < k; m++) low[low_len++] = data[m];
            }

            toks[n_tok].data = &low[low_start];
            toks[n_tok].len = full_len;
            toks[n_tok].case_flag = cflag;
            n_tok++;
            i = k;
        } else {
            /* Non-alpha token */
            uint32_t j = i + 1;
            while (j < len && is_ws(data[j])) j++;
            uint32_t low_start = low_len;
            for (uint32_t m = i; m < j; m++) low[low_len++] = data[m];
            toks[n_tok].data = &low[low_start];
            toks[n_tok].len = j - i;
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
uint32_t word_split(const uint8_t *data, uint32_t len,
                    const uint8_t ***words_out, uint16_t **lens_out) {
    uint32_t cap = len / 4 + 16;
    const uint8_t **wp = (const uint8_t **)malloc(cap * sizeof(uint8_t *));
    uint16_t *wl = (uint16_t *)malloc(cap * sizeof(uint16_t));
    uint32_t nw = 0;
    uint32_t i = 0;

    while (i < len) {
        if (nw >= cap) {
            cap *= 2;
            wp = (const uint8_t **)realloc(wp, cap * sizeof(uint8_t *));
            wl = (uint16_t *)realloc(wl, cap * sizeof(uint16_t));
        }
        if (is_alpha_or_hi(data[i])) {
            uint32_t j = i + 1;
            while (j < len && is_alpha_or_hi(data[j])) j++;
            uint32_t k = j;
            while (k < len && is_ws(data[k])) k++;
            wp[nw] = &data[i];
            wl[nw] = (uint16_t)(k - i);
            nw++;
            i = k;
        } else {
            uint32_t j = i + 1;
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
 * Case encoding (small 3-symbol AC)
 * ══════════════════════════════════════════════════════ */
uint8_t *enc_case(const uint8_t *flags, uint32_t n, uint32_t *out_len) {
    /* Count frequencies */
    uint32_t cf[3] = {0, 0, 0};
    for (uint32_t i = 0; i < n; i++) cf[flags[i]]++;

    uint32_t fa[3];
    for (int i = 0; i < 3; i++) fa[i] = cf[i] + 1;
    uint32_t tm = fa[0] + fa[1] + fa[2];
    uint32_t CM = 1u << 16;

    uint16_t quant[3];
    for (int i = 0; i < 3; i++) {
        uint32_t q = (uint32_t)((uint64_t)fa[i] * (CM - 3) / tm);
        quant[i] = (uint16_t)(q < 1 ? 1 : q);
    }

    uint16_t cdf[4] = {0, quant[0], (uint16_t)(quant[0] + quant[1]),
                        (uint16_t)(quant[0] + quant[1] + quant[2])};
    uint16_t total = cdf[3];

    /* Encode */
    qtc_senc_t enc;
    senc_init(&enc);
    for (uint32_t i = 0; i < n; i++)
        senc_encode(&enc, cdf[flags[i]], cdf[flags[i] + 1], total);
    uint32_t ac_len;
    uint8_t *ac_data = senc_finish(&enc, &ac_len);
    senc_free(&enc);

    /* Output: 3 uint16 (CDF values) + AC data */
    *out_len = 6 + ac_len;
    uint8_t *out = (uint8_t *)malloc(*out_len);
    memcpy(out, &cdf[1], 2);
    memcpy(out + 2, &cdf[2], 2);
    memcpy(out + 4, &cdf[3], 2);
    memcpy(out + 6, ac_data, ac_len);
    free(ac_data);
    return out;
}

uint8_t *dec_case(const uint8_t *data, uint32_t n, uint32_t data_len) {
    uint16_t cdf[4];
    cdf[0] = 0;
    memcpy(&cdf[1], data, 2);
    memcpy(&cdf[2], data + 2, 2);
    memcpy(&cdf[3], data + 4, 2);
    uint16_t total = cdf[3];

    qtc_sdec_t dec;
    sdec_init(&dec, data + 6, data_len - 6);

    uint8_t *flags = (uint8_t *)malloc(n);
    for (uint32_t i = 0; i < n; i++)
        flags[i] = sdec_decode(&dec, cdf, total);
    sdec_free(&dec);
    return flags;
}
