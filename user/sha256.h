/**
 * QuantumOS SHA-256 — freestanding, integer-only (ghostd phase 4, #51).
 *
 * A fresh, public-domain-style implementation in pure uint32 arithmetic: no
 * tables of doubles, no libc, no floats — it links into the ring-3 swarm_svc
 * with the same -ffreestanding/-mno-sse flags as the rest of user space. It is
 * standard FIPS 180-4 SHA-256 (big-endian, 0x428a2f98… round constants), so a
 * host verifier using Python's hashlib computes byte-identical digests.
 *
 * swarm_svc uses it for two things: hashing the boot-attestation message, and
 * as the one-way function + counter-mode key expander of the Lamport
 * hash-based one-time signature (see swarm.h).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint32_t buflen;
    uint8_t  buf[64];
} sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t sha256_rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static inline void sha256_compress(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static inline void sha256_init(sha256_ctx *c) {
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0;
    c->buflen = 0;
}

static inline void sha256_update(sha256_ctx *c, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        c->buf[c->buflen++] = data[i];
        if (c->buflen == 64) {
            sha256_compress(c, c->buf);
            c->bitlen += 512;
            c->buflen = 0;
        }
    }
}

static inline void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->bitlen + (uint64_t)c->buflen * 8u;
    uint32_t i = c->buflen;

    c->buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) c->buf[i++] = 0;
        sha256_compress(c, c->buf);
        i = 0;
    }
    while (i < 56) c->buf[i++] = 0;
    for (int s = 7; s >= 0; s--) {
        c->buf[i++] = (uint8_t)((bits >> (s * 8)) & 0xFFu);
    }
    sha256_compress(c, c->buf);

    for (int j = 0; j < 8; j++) {
        out[j * 4]     = (uint8_t)(c->state[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(c->state[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(c->state[j] >> 8);
        out[j * 4 + 3] = (uint8_t)(c->state[j]);
    }
}

/* One-shot convenience wrapper. */
static inline void sha256(const uint8_t *data, uint32_t len, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

#endif /* SHA256_H */
