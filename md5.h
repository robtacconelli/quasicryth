/*
 * MD5 - Minimal implementation (public domain, RFC 1321)
 */
#ifndef QTC_MD5_H
#define QTC_MD5_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[4];
    uint64_t count;
    uint8_t  buffer[64];
} md5_ctx_t;

void md5_init(md5_ctx_t *ctx);
void md5_update(md5_ctx_t *ctx, const uint8_t *data, size_t len);
void md5_final(md5_ctx_t *ctx, uint8_t digest[16]);

/* One-shot convenience */
void md5_hash(const uint8_t *data, size_t len, uint8_t digest[16]);

#endif
