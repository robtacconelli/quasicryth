/*
 * QTC - Tokenizer: case separation, word splitting, case encoding/decoding
 */
#ifndef QTC_TOK_H
#define QTC_TOK_H

#include <stdint.h>
#include <stdbool.h>

/* ── Token (from case separation) ──────────────────── */
typedef struct {
    const uint8_t *data;   /* pointer into lowered buffer */
    uint32_t       len;
    uint8_t        case_flag;  /* 0=lower, 1=capitalized, 2=UPPER */
} qtc_token_t;

/* ── Tokenize with case separation ─────────────────── */
/* Splits data into tokens, separates case.
 * lowered_out: receives malloc'd lowered byte data (caller frees).
 * tokens_out: receives malloc'd token array (caller frees).
 * Returns number of tokens. */
uint32_t tokenize(const uint8_t *data, uint32_t len,
                  uint8_t **lowered_out, uint32_t *lowered_len,
                  qtc_token_t **tokens_out);

/* Apply case flag to a token. Returns malloc'd buffer. */
uint8_t *apply_case(const uint8_t *data, uint32_t len, uint8_t flag, uint32_t *out_len);

/* ── Word split ────────────────────────────────────── */
/* Split lowered data into word tokens (byte sequences).
 * Returns number of words. words_out[i] = pointer, lens_out[i] = length.
 * Pointers point into data (not owned). Caller frees words_out and lens_out. */
uint32_t word_split(const uint8_t *data, uint32_t len,
                    const uint8_t ***words_out, uint16_t **lens_out);

/* ── Case encoding/decoding (small AC) ─────────────── */
/* Encode case flags. Returns malloc'd buffer. */
uint8_t *enc_case(const uint8_t *flags, uint32_t n, uint32_t *out_len);

/* Decode case flags. Returns malloc'd array of uint8_t. */
uint8_t *dec_case(const uint8_t *data, uint32_t n, uint32_t data_len);

#endif /* QTC_TOK_H */
