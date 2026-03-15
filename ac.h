/*
 * QTC - Adaptive arithmetic coder (256-symbol, 24-bit precision)
 * Plus small 3-symbol 18-bit coder for case flags.
 */
#ifndef QTC_AC_H
#define QTC_AC_H

#include <stdint.h>
#include <stdbool.h>
#include "ht.h"  /* for qtc_buf_t */

/* ── Constants ─────────────────────────────────────── */
#define AC_PREC     24
#define AC_FULL     (1u << AC_PREC)
#define AC_HALF     (AC_FULL >> 1)
#define AC_QTR      (AC_HALF >> 1)
#define AC_MAX_FREQ (1u << 14)

#define QTC_ESC_SYM 255
#define QTC_EXT_SYM 254

/* ── Adaptive model (256 symbols) ──────────────────── */
typedef struct {
    uint16_t freq[256];
    uint32_t total;
} qtc_model_t;

void model_init(qtc_model_t *m);
void model_update(qtc_model_t *m, uint8_t sym);

/* Compute CDF. cdf[i] = sum of freq[0..i-1]. cdf[256] = total.
 * Caller must provide uint32_t cdf[257]. Returns total. */
uint32_t model_cdf(const qtc_model_t *m, uint32_t cdf[257]);

/* ── Encoder ───────────────────────────────────────── */
typedef struct {
    uint32_t  lo, hi;
    uint32_t  pending;
    qtc_buf_t bits;
    uint8_t   buf;      /* partial byte accumulator */
    uint8_t   bc;       /* bits in buf */
} qtc_encoder_t;

void     enc_init(qtc_encoder_t *e);
void     enc_encode(qtc_encoder_t *e, uint32_t cum_lo, uint32_t cum_hi, uint32_t total);
uint8_t *enc_finish(qtc_encoder_t *e, uint32_t *out_len);
void     enc_free(qtc_encoder_t *e);

/* ── Decoder ───────────────────────────────────────── */
typedef struct {
    uint32_t       lo, hi, val;
    const uint8_t *data;
    uint32_t       data_len;
    uint32_t       byte_pos;
    uint8_t        bit_idx;
} qtc_decoder_t;

void    dec_init(qtc_decoder_t *d, const uint8_t *data, uint32_t len);
uint8_t dec_decode(qtc_decoder_t *d, const uint32_t cdf[257], uint32_t total);

/* ── High-level encode/decode helpers ──────────────── */
void    ac_enc_sym(qtc_encoder_t *e, qtc_model_t *m, uint8_t sym);
uint8_t ac_dec_sym(qtc_decoder_t *d, qtc_model_t *m);

/* Multi-stage index encoding (0-252 direct, 254+lo extended, 254+255+hi+lo stage2) */
void    encode_index(qtc_encoder_t *e, qtc_model_t *model, qtc_model_t *ext, uint32_t idx);
/* Returns index, or -1 for ESC, -2 for TRI(253) */
int32_t decode_index(qtc_decoder_t *d, qtc_model_t *model, qtc_model_t *ext);

/* ── Small AC (18-bit, for case flags) ─────────────── */
#define SAC_PREC 18
#define SAC_FULL (1u << SAC_PREC)
#define SAC_HALF (SAC_FULL >> 1)
#define SAC_QTR  (SAC_HALF >> 1)

typedef struct {
    uint32_t lo, hi, pending;
    uint32_t *bits;
    uint32_t  bits_len, bits_cap;
} qtc_senc_t;

typedef struct {
    uint32_t lo, hi, val;
    uint8_t *bits;
    uint32_t bits_len, pos;
} qtc_sdec_t;

void     senc_init(qtc_senc_t *e);
void     senc_encode(qtc_senc_t *e, uint32_t lo, uint32_t hi, uint32_t total);
uint8_t *senc_finish(qtc_senc_t *e, uint32_t *out_len);
void     senc_free(qtc_senc_t *e);

void    sdec_init(qtc_sdec_t *d, const uint8_t *data, uint32_t len);
uint8_t sdec_decode(qtc_sdec_t *d, const uint16_t cdf[4], uint16_t total);
void    sdec_free(qtc_sdec_t *d);

#endif /* QTC_AC_H */
