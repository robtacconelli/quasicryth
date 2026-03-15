/*
 * QTC - Adaptive arithmetic coder implementation
 */
#include "ac.h"
#include <stdlib.h>
#include <string.h>

/* ══════════════════════════════════════════════════════
 * Adaptive Model (256 symbols)
 * ══════════════════════════════════════════════════════ */
void model_init(qtc_model_t *m) {
    for (int i = 0; i < 256; i++) m->freq[i] = 1;
    m->total = 256;
}

void model_update(qtc_model_t *m, uint8_t sym) {
    m->freq[sym]++;
    m->total++;
    if (m->total >= AC_MAX_FREQ) {
        m->total = 0;
        for (int i = 0; i < 256; i++) {
            m->freq[i] = (m->freq[i] >> 1) | 1;  /* max(1, freq>>1) */
            m->total += m->freq[i];
        }
    }
}

uint32_t model_cdf(const qtc_model_t *m, uint32_t cdf[257]) {
    uint32_t acc = 0;
    for (int i = 0; i < 256; i++) { cdf[i] = acc; acc += m->freq[i]; }
    cdf[256] = acc;
    return acc;
}

/* ══════════════════════════════════════════════════════
 * Encoder
 * ══════════════════════════════════════════════════════ */
void enc_init(qtc_encoder_t *e) {
    e->lo = 0; e->hi = AC_FULL - 1; e->pending = 0;
    buf_init(&e->bits, 4096);
    e->buf = 0; e->bc = 0;
}

static inline void enc_emit(qtc_encoder_t *e, uint8_t bit) {
    e->buf = (e->buf << 1) | bit;
    e->bc++;
    if (e->bc == 8) {
        buf_push(&e->bits, e->buf);
        e->buf = 0; e->bc = 0;
    }
}

static inline void enc_output(qtc_encoder_t *e, uint8_t bit) {
    enc_emit(e, bit);
    while (e->pending > 0) { enc_emit(e, 1 - bit); e->pending--; }
}

void enc_encode(qtc_encoder_t *e, uint32_t cum_lo, uint32_t cum_hi, uint32_t total) {
    uint32_t r = e->hi - e->lo + 1;
    e->hi = e->lo + (uint32_t)((uint64_t)r * cum_hi / total) - 1;
    e->lo = e->lo + (uint32_t)((uint64_t)r * cum_lo / total);
    for (;;) {
        if (e->hi < AC_HALF) {
            enc_output(e, 0);
        } else if (e->lo >= AC_HALF) {
            enc_output(e, 1);
            e->lo -= AC_HALF; e->hi -= AC_HALF;
        } else if (e->lo >= AC_QTR && e->hi < 3 * AC_QTR) {
            e->pending++;
            e->lo -= AC_QTR; e->hi -= AC_QTR;
        } else break;
        e->lo <<= 1;
        e->hi = (e->hi << 1) | 1;
    }
}

uint8_t *enc_finish(qtc_encoder_t *e, uint32_t *out_len) {
    e->pending++;
    enc_output(e, e->lo < AC_QTR ? 0 : 1);
    if (e->bc > 0) {
        e->buf <<= (8 - e->bc);
        buf_push(&e->bits, e->buf);
    }
    *out_len = e->bits.len;
    uint8_t *result = e->bits.data;
    e->bits.data = NULL;  /* transfer ownership */
    return result;
}

void enc_free(qtc_encoder_t *e) {
    buf_free(&e->bits);
}

/* ══════════════════════════════════════════════════════
 * Decoder
 * ══════════════════════════════════════════════════════ */
static inline uint8_t dec_read_bit(qtc_decoder_t *d) {
    if (d->byte_pos >= d->data_len) return 0;
    uint8_t bit = (d->data[d->byte_pos] >> (7 - d->bit_idx)) & 1;
    d->bit_idx++;
    if (d->bit_idx == 8) { d->bit_idx = 0; d->byte_pos++; }
    return bit;
}

void dec_init(qtc_decoder_t *d, const uint8_t *data, uint32_t len) {
    d->lo = 0; d->hi = AC_FULL - 1;
    d->data = data; d->data_len = len;
    d->byte_pos = 0; d->bit_idx = 0;
    d->val = 0;
    for (int i = 0; i < AC_PREC; i++)
        d->val = (d->val << 1) | dec_read_bit(d);
}

uint8_t dec_decode(qtc_decoder_t *d, const uint32_t cdf[257], uint32_t total) {
    uint32_t r = d->hi - d->lo + 1;
    uint32_t scaled = (uint32_t)(((uint64_t)(d->val - d->lo + 1) * total - 1) / r);
    /* Binary search for symbol */
    int lo_s = 0, hi_s = 255;
    while (lo_s < hi_s) {
        int mid = (lo_s + hi_s) >> 1;
        if (cdf[mid + 1] <= scaled) lo_s = mid + 1;
        else hi_s = mid;
    }
    uint8_t sym = (uint8_t)lo_s;
    d->hi = d->lo + (uint32_t)((uint64_t)r * cdf[sym + 1] / total) - 1;
    d->lo = d->lo + (uint32_t)((uint64_t)r * cdf[sym] / total);
    for (;;) {
        if (d->hi < AC_HALF) {
            /* nothing */
        } else if (d->lo >= AC_HALF) {
            d->lo -= AC_HALF; d->hi -= AC_HALF; d->val -= AC_HALF;
        } else if (d->lo >= AC_QTR && d->hi < 3 * AC_QTR) {
            d->lo -= AC_QTR; d->hi -= AC_QTR; d->val -= AC_QTR;
        } else break;
        d->lo <<= 1;
        d->hi = (d->hi << 1) | 1;
        d->val = (d->val << 1) | dec_read_bit(d);
    }
    return sym;
}

/* ══════════════════════════════════════════════════════
 * High-level helpers
 * ══════════════════════════════════════════════════════ */
void ac_enc_sym(qtc_encoder_t *e, qtc_model_t *m, uint8_t sym) {
    uint32_t cdf[257];
    uint32_t total = model_cdf(m, cdf);
    enc_encode(e, cdf[sym], cdf[sym + 1], total);
    model_update(m, sym);
}

uint8_t ac_dec_sym(qtc_decoder_t *d, qtc_model_t *m) {
    uint32_t cdf[257];
    uint32_t total = model_cdf(m, cdf);
    uint8_t sym = dec_decode(d, cdf, total);
    model_update(m, sym);
    return sym;
}

void encode_index(qtc_encoder_t *e, qtc_model_t *model, qtc_model_t *ext, uint32_t idx) {
    if (idx < 254) {
        ac_enc_sym(e, model, (uint8_t)idx);
    } else if (idx < 509) {
        ac_enc_sym(e, model, QTC_EXT_SYM);
        ac_enc_sym(e, ext, (uint8_t)(idx - 254));
    } else if (idx < 65789) {
        ac_enc_sym(e, model, QTC_EXT_SYM);
        ac_enc_sym(e, ext, 255);  /* stage-2 flag */
        uint32_t v = idx - 509;
        ac_enc_sym(e, ext, (uint8_t)(v >> 8));
        ac_enc_sym(e, ext, (uint8_t)(v & 0xFF));
    } else {
        ac_enc_sym(e, model, QTC_ESC_SYM);
    }
}

int32_t decode_index(qtc_decoder_t *d, qtc_model_t *model, qtc_model_t *ext) {
    uint8_t sym = ac_dec_sym(d, model);
    if (sym == QTC_EXT_SYM) {
        uint8_t lo = ac_dec_sym(d, ext);
        if (lo == 255) {
            uint8_t hi2 = ac_dec_sym(d, ext);
            uint8_t lo2 = ac_dec_sym(d, ext);
            return 509 + ((int32_t)hi2 << 8) + lo2;
        }
        return 254 + lo;
    }
    if (sym == QTC_ESC_SYM) return -1;
    return (int32_t)sym;
}

/* ══════════════════════════════════════════════════════
 * Small AC encoder (18-bit, for case flags)
 * ══════════════════════════════════════════════════════ */
void senc_init(qtc_senc_t *e) {
    e->lo = 0; e->hi = SAC_FULL - 1; e->pending = 0;
    e->bits_cap = 4096;
    e->bits = (uint32_t *)malloc(e->bits_cap * sizeof(uint32_t));
    e->bits_len = 0;
}

static inline void senc_emit(qtc_senc_t *e, uint8_t bit) {
    if (e->bits_len >= e->bits_cap) {
        e->bits_cap *= 2;
        e->bits = (uint32_t *)realloc(e->bits, e->bits_cap * sizeof(uint32_t));
    }
    e->bits[e->bits_len++] = bit;
}

static inline void senc_output(qtc_senc_t *e, uint8_t bit) {
    senc_emit(e, bit);
    while (e->pending) { senc_emit(e, 1 - bit); e->pending--; }
}

void senc_encode(qtc_senc_t *e, uint32_t lo, uint32_t hi, uint32_t total) {
    uint32_t r = e->hi - e->lo + 1;
    e->hi = e->lo + (uint32_t)((uint64_t)r * hi / total) - 1;
    e->lo = e->lo + (uint32_t)((uint64_t)r * lo / total);
    for (;;) {
        if (e->hi < SAC_HALF) {
            senc_output(e, 0);
        } else if (e->lo >= SAC_HALF) {
            senc_output(e, 1);
            e->lo -= SAC_HALF; e->hi -= SAC_HALF;
        } else if (e->lo >= SAC_QTR && e->hi < 3 * SAC_QTR) {
            e->pending++; e->lo -= SAC_QTR; e->hi -= SAC_QTR;
        } else break;
        e->lo <<= 1; e->hi = (e->hi << 1) | 1;
    }
}

uint8_t *senc_finish(qtc_senc_t *e, uint32_t *out_len) {
    e->pending++;
    senc_output(e, e->lo < SAC_QTR ? 0 : 1);
    /* Pad to byte boundary */
    while (e->bits_len % 8) senc_emit(e, 0);
    uint32_t nbytes = e->bits_len / 8;
    uint8_t *out = (uint8_t *)malloc(nbytes);
    for (uint32_t i = 0; i < nbytes; i++) {
        uint8_t v = 0;
        for (int j = 0; j < 8; j++)
            v = (v << 1) | (uint8_t)e->bits[i * 8 + j];
        out[i] = v;
    }
    *out_len = nbytes;
    return out;
}

void senc_free(qtc_senc_t *e) {
    free(e->bits); e->bits = NULL;
}

/* ══════════════════════════════════════════════════════
 * Small AC decoder (18-bit)
 * ══════════════════════════════════════════════════════ */
void sdec_init(qtc_sdec_t *d, const uint8_t *data, uint32_t len) {
    d->lo = 0; d->hi = SAC_FULL - 1;
    d->bits_len = len * 8;
    d->bits = (uint8_t *)malloc(d->bits_len);
    uint32_t pos = 0;
    for (uint32_t i = 0; i < len; i++)
        for (int j = 7; j >= 0; j--)
            d->bits[pos++] = (data[i] >> j) & 1;
    d->pos = 0;
    d->val = 0;
    for (int i = 0; i < SAC_PREC; i++) {
        uint8_t b = (d->pos < d->bits_len) ? d->bits[d->pos++] : 0;
        d->val = (d->val << 1) | b;
    }
}

uint8_t sdec_decode(qtc_sdec_t *d, const uint16_t cdf[4], uint16_t total) {
    uint32_t r = d->hi - d->lo + 1;
    uint32_t tgt = (uint32_t)(((uint64_t)(d->val - d->lo + 1) * total - 1) / r);
    int a = 0, b = 2; /* 3 symbols: 0,1,2 */
    while (a < b) {
        int m = (a + b) / 2;
        if (cdf[m + 1] <= tgt) a = m + 1;
        else b = m;
    }
    uint8_t sym = (uint8_t)a;
    d->hi = d->lo + (uint32_t)((uint64_t)r * cdf[sym + 1] / total) - 1;
    d->lo = d->lo + (uint32_t)((uint64_t)r * cdf[sym] / total);
    for (;;) {
        if (d->hi < SAC_HALF) {
            /* nothing */
        } else if (d->lo >= SAC_HALF) {
            d->lo -= SAC_HALF; d->hi -= SAC_HALF; d->val -= SAC_HALF;
        } else if (d->lo >= SAC_QTR && d->hi < 3 * SAC_QTR) {
            d->lo -= SAC_QTR; d->hi -= SAC_QTR; d->val -= SAC_QTR;
        } else break;
        d->lo <<= 1; d->hi = (d->hi << 1) | 1;
        uint8_t bit = (d->pos < d->bits_len) ? d->bits[d->pos++] : 0;
        d->val = (d->val << 1) | bit;
    }
    return sym;
}

void sdec_free(qtc_sdec_t *d) {
    free(d->bits); d->bits = NULL;
}
