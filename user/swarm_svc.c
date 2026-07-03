/**
 * QuantumOS swarm_svc — the COM2 serial swarm bridge, a fourth isolated
 * ring-3 service (ghostd phase 4, #51; epic #47).
 *
 * Two jobs, both over the capability-guarded COM2 UART (it is the only
 * ring-3 process granted the DEVICE_ID_COM2 device cap, plus a quantum-pool
 * read cap and a single IPC send-cap to ghostd):
 *
 *   1. Boot attestation. At startup swarm_svc draws a 32-byte master seed
 *      from the quantum pool, deterministically expands it into a 256-pair
 *      Lamport key (SHA-256 counter mode), and emits — as CRC8-framed COM2
 *      frames — a public-key-digest commitment, the attestation string
 *      "QOS-BOOT|qseed=<hex|none>|ticks=<n>", and the Lamport signature over
 *      it. A host reading the COM2 stream can verify the signature against the
 *      committed public key and confirm the attested qseed. See swarm.h for
 *      the frame format and the exact Lamport construction.
 *
 *   2. Bridge traffic. It then speaks the length-prefixed frame protocol:
 *      PING is answered with PONG, and a DATA frame is a remote request routed
 *      over capability-checked IPC to ghostd (SYS_SEND_TO) and answered with a
 *      DATA frame carrying ghostd's reply.
 *
 * No floats anywhere; all crypto is integer SHA-256 (sha256.h).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "swarm.h"
#include "sha256.h"
#include "ghost.h"   /* ghost_req_t/ghost_rep_t, GHOST_*, usys.h, string builders */

/* ---- state (zeroed .bss) ---- */
static uint8_t  master_seed[LAMPORT_SEED_LEN];
static uint8_t  framebuf[SWARM_HDR_LEN + SWARM_MAX_PAYLOAD + 1];
static long     ghostd_pid = 0;

/* COM2 receive accumulator for the inbound frame parser. */
static uint8_t  rxbuf[SWARM_HDR_LEN + SWARM_MAX_PAYLOAD + 1];
static uint32_t rxlen = 0;

static void logline(const char *s) { write_str(s); }

/* ---- little formatting helpers (no libc) ---- */
static int put_hex_u64(char *b, int o, uint64_t v) {
    if (v == 0) { b[o++] = '0'; return o; }
    char t[16];
    int n = 0;
    while (v) {
        int d = (int)(v & 0xFu);
        t[n++] = (char)(d < 10 ? ('0' + d) : ('A' + d - 10));
        v >>= 4;
    }
    while (n) b[o++] = t[--n];
    return o;
}

/* ---- COM2 output: send every byte, chunked under the kernel's per-call cap.
 * SYS_COM2 moves at most COM2_MAX_BYTES (256) per call, so loop until done. */
static void com2_send_all(const uint8_t *buf, long len) {
    long off = 0;
    while (off < len) {
        long chunk = len - off;
        if (chunk > 256) chunk = 256;
        long w = com2_write_bytes(buf + off, chunk);
        if (w <= 0) return;   /* no cap / error — nothing more we can do */
        off += w;
    }
}

/* Build one framed message and push it out COM2. */
static void emit_frame(uint8_t type, const uint8_t *payload, uint32_t len) {
    if (len > SWARM_MAX_PAYLOAD) len = SWARM_MAX_PAYLOAD;
    framebuf[0] = SWARM_MAGIC;
    framebuf[1] = type;
    framebuf[2] = (uint8_t)(len & 0xFFu);
    framebuf[3] = (uint8_t)((len >> 8) & 0xFFu);
    for (uint32_t i = 0; i < len; i++) framebuf[SWARM_HDR_LEN + i] = payload[i];
    /* crc8 covers type + length + payload (everything but magic and crc) */
    framebuf[SWARM_HDR_LEN + len] = swarm_crc8(&framebuf[1], 3 + len);
    com2_send_all(framebuf, (long)(SWARM_HDR_LEN + len + 1));
}

/* ---- Lamport key material (counter-mode expansion of the master seed) ----
 * sk[i][b] = SHA-256(master_seed ‖ be32(i) ‖ b). */
static void expand_sk(uint32_t i, uint8_t b, uint8_t out[LAMPORT_HASH_LEN]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, master_seed, LAMPORT_SEED_LEN);
    uint8_t ib[5];
    ib[0] = (uint8_t)(i >> 24); ib[1] = (uint8_t)(i >> 16);
    ib[2] = (uint8_t)(i >> 8);  ib[3] = (uint8_t)i; ib[4] = b;
    sha256_update(&c, ib, 5);
    sha256_final(&c, out);
}

/* pubkey digest = SHA-256(pk[0][0] ‖ pk[0][1] ‖ … ‖ pk[255][1]),
 * pk[i][b] = SHA-256(sk[i][b]). Streamed so no 16 KB buffer is needed. */
static void compute_pubkey_digest(uint8_t out[LAMPORT_HASH_LEN]) {
    sha256_ctx pk_ctx;
    sha256_init(&pk_ctx);
    for (uint32_t i = 0; i < LAMPORT_BITS; i++) {
        for (uint8_t b = 0; b < 2; b++) {
            uint8_t sk[LAMPORT_HASH_LEN], pk[LAMPORT_HASH_LEN];
            expand_sk(i, b, sk);
            sha256(sk, LAMPORT_HASH_LEN, pk);
            sha256_update(&pk_ctx, pk, LAMPORT_HASH_LEN);
        }
    }
    sha256_final(&pk_ctx, out);
}

/* Sign md and emit the signature as FRAME_SIG chunks. Element i (for message
 * bit b_i) is the revealed preimage sk[i][b_i] followed by the complementary
 * public-key hash pk[i][1-b_i], so the verifier can rebuild the whole public
 * key and match it to the committed digest. */
static void emit_signature(const uint8_t md[LAMPORT_HASH_LEN]) {
    uint8_t chunk[SWARM_MAX_PAYLOAD];   /* 512 = 8 elements of 64 bytes */
    uint32_t cpos = 0;
    for (uint32_t i = 0; i < LAMPORT_BITS; i++) {
        uint8_t bit = (uint8_t)((md[i >> 3] >> (i & 7u)) & 1u);
        uint8_t sk[LAMPORT_HASH_LEN], sko[LAMPORT_HASH_LEN], pko[LAMPORT_HASH_LEN];
        expand_sk(i, bit, sk);                 /* revealed preimage */
        expand_sk(i, (uint8_t)(1u - bit), sko);
        sha256(sko, LAMPORT_HASH_LEN, pko);     /* complementary pk hash */
        for (int j = 0; j < LAMPORT_HASH_LEN; j++) chunk[cpos + j] = sk[j];
        for (int j = 0; j < LAMPORT_HASH_LEN; j++) chunk[cpos + LAMPORT_HASH_LEN + j] = pko[j];
        cpos += LAMPORT_SIG_ELEM;
        if (cpos == SWARM_MAX_PAYLOAD) {
            emit_frame(FRAME_SIG, chunk, cpos);
            cpos = 0;
        }
    }
    if (cpos > 0) emit_frame(FRAME_SIG, chunk, cpos);
}

/* Compose the attestation string into `msg`, returning its length. */
static int build_attestation(char *msg, int seed_present, uint64_t qseed, uint64_t tk) {
    int o = 0;
    o = ghost_put(msg, o, "QOS-BOOT|qseed=");
    if (seed_present) o = put_hex_u64(msg, o, qseed);
    else              o = ghost_put(msg, o, "none");
    o = ghost_put(msg, o, "|ticks=");
    o = ghost_put_u(msg, o, (unsigned)tk);
    return o;
}

/* Draw the Lamport master seed, sign a boot attestation, and emit it on COM2.
 * The console gate line (grepped by CI) is printed on COM1 afterwards. */
static void emit_boot_attestation(void) {
    /* 32 bytes of true entropy from the quantum pool; the 16 KB key is
     * SHA-256-expanded from it (security = SHA-256 + this 256-bit seed). */
    long got = qrand_fill(master_seed, LAMPORT_SEED_LEN);
    if (got < (long)LAMPORT_SEED_LEN) {
        /* No quantum cap / short draw: fall back to a tick-stirred filler so
         * the key is still well-defined. Honest — the attestation itself
         * records qseed provenance separately below. */
        uint32_t x = (uint32_t)ticks() ^ 0x9E3779B9u;
        for (int i = (got > 0 ? (int)got : 0); i < LAMPORT_SEED_LEN; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            master_seed[i] = (uint8_t)(x & 0xFFu);
        }
    }

    long sp = qrand_seed_present();          /* 1 present, 0 absent, <0 no cap */
    long qs = qseed_value();                  /* seed value, or <0 no cap */
    int seed_present = (sp == 1);
    uint64_t qseed = seed_present ? (uint64_t)qs : 0;
    uint64_t tk = (uint64_t)ticks();

    char msg[96];
    int msglen = build_attestation(msg, seed_present, qseed, tk);

    /* HANDSHAKE opens the stream (protocol id), then the attestation trio. */
    emit_frame(FRAME_HANDSHAKE, (const uint8_t *)"QOS-SWARM/1", 11);

    uint8_t pkdigest[LAMPORT_HASH_LEN];
    compute_pubkey_digest(pkdigest);
    emit_frame(FRAME_PKDIGEST, pkdigest, LAMPORT_HASH_LEN);

    emit_frame(FRAME_ATTEST, (const uint8_t *)msg, (uint32_t)msglen);

    uint8_t md[LAMPORT_HASH_LEN];
    sha256((const uint8_t *)msg, (uint32_t)msglen, md);
    emit_signature(md);

    /* COM1 console gate line (CI greps "SWARM: boot attestation emitted"). */
    char line[96];
    int o = ghost_put(line, 0, "SWARM: boot attestation emitted (lamport-signed, qseed=");
    if (seed_present) o = put_hex_u64(line, o, qseed);
    else              o = ghost_put(line, o, "none");
    o = ghost_put(line, o, ")");
    line[o] = 0;
    logline(line);
}

/* ---- ghostd routing (learned pid + SYS_SEND_TO) ---- */

/* One-time discovery: send a STATUS via the single ghostd send-cap (first
 * match is unambiguous) and record the replying pid for targeted sends. */
static int discover_ghostd(void) {
    ghost_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    req.op = GHOST_STATUS;
    if (send_msg((const char *)&req, (long)sizeof(req)) != 0) return 0;

    char buf[sizeof(ghost_rep_t) + 8];
    for (long spins = 0; spins < 4000000; spins++) {
        long snd = recv_msg(buf, sizeof(buf));
        if (snd != 0) { ghostd_pid = snd; return 1; }
        yield();
    }
    return 0;
}

/* Forward a request to ghostd (targeted send) and block, bounded, for the
 * reply. Returns 1 on success. */
static int ghost_query(const ghost_req_t *req, ghost_rep_t *rep) {
    if (ghostd_pid == 0) return 0;
    if (send_to(ghostd_pid, (const char *)req, (long)sizeof(*req)) != 0) return 0;
    char buf[sizeof(ghost_rep_t) + 8];
    for (long spins = 0; spins < 2000000; spins++) {
        long snd = recv_msg(buf, sizeof(buf));
        if (snd != 0) {
            for (unsigned i = 0; i < sizeof(*rep); i++) ((uint8_t *)rep)[i] = (uint8_t)buf[i];
            return 1;
        }
        yield();
    }
    return 0;
}

/* ---- inbound frame dispatch ---- */

static void handle_data(const uint8_t *payload, uint32_t len) {
    if (len < 1) return;
    uint8_t op = payload[0];

    ghost_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) ((uint8_t *)&req)[i] = 0;
    ghost_rep_t rep;

    if (op == SWARM_OP_STATUS) {
        req.op = GHOST_STATUS;
        if (!ghost_query(&req, &rep)) return;
        /* reply: op, r_q16 (LE u32), live (u8) */
        uint8_t out[6];
        out[0] = SWARM_OP_STATUS;
        out[1] = (uint8_t)(rep.r_q16);
        out[2] = (uint8_t)(rep.r_q16 >> 8);
        out[3] = (uint8_t)(rep.r_q16 >> 16);
        out[4] = (uint8_t)(rep.r_q16 >> 24);
        out[5] = rep.live;
        emit_frame(FRAME_DATA, out, sizeof(out));
    } else if (op == SWARM_OP_RECALL) {
        if (len < 1 + GHOST_PW * 4) return;
        req.op = GHOST_RECALL;
        for (int w = 0; w < GHOST_PW; w++) {
            req.bits[w] = (uint32_t)payload[1 + w * 4] |
                          ((uint32_t)payload[1 + w * 4 + 1] << 8) |
                          ((uint32_t)payload[1 + w * 4 + 2] << 16) |
                          ((uint32_t)payload[1 + w * 4 + 3] << 24);
        }
        if (!ghost_query(&req, &rep)) return;
        /* reply: op, match (s8), r_q16 (LE u32) */
        uint8_t out[6];
        out[0] = SWARM_OP_RECALL;
        out[1] = (uint8_t)rep.match;
        out[2] = (uint8_t)(rep.r_q16);
        out[3] = (uint8_t)(rep.r_q16 >> 8);
        out[4] = (uint8_t)(rep.r_q16 >> 16);
        out[5] = (uint8_t)(rep.r_q16 >> 24);
        emit_frame(FRAME_DATA, out, sizeof(out));
    }
}

static void dispatch_frame(uint8_t type, const uint8_t *payload, uint32_t len) {
    switch (type) {
    case FRAME_PING:
        emit_frame(FRAME_PONG, payload, len);   /* echo payload back as PONG */
        break;
    case FRAME_DATA:
        handle_data(payload, len);
        break;
    case FRAME_DISCONNECT:
    case FRAME_HANDSHAKE:
    default:
        break;                                  /* nothing to do */
    }
}

/* Pull whatever COM2 has, append to the accumulator, and parse out every
 * complete, CRC-valid frame. Resynchronises byte-by-byte on the magic. */
static void poll_com2(void) {
    uint8_t in[128];
    long got = com2_read_bytes(in, (long)sizeof(in));
    for (long i = 0; i < got; i++) {
        if (rxlen < sizeof(rxbuf)) rxbuf[rxlen++] = in[i];
    }

    for (;;) {
        /* resync: drop leading bytes until the buffer starts with the magic */
        uint32_t start = 0;
        while (start < rxlen && rxbuf[start] != SWARM_MAGIC) start++;
        if (start > 0) {
            for (uint32_t k = start; k < rxlen; k++) rxbuf[k - start] = rxbuf[k];
            rxlen -= start;
        }
        if (rxlen < SWARM_HDR_LEN) return;      /* need a full header */

        uint32_t len = (uint32_t)rxbuf[2] | ((uint32_t)rxbuf[3] << 8);
        if (len > SWARM_MAX_PAYLOAD) {          /* bogus length: drop the magic, resync */
            for (uint32_t k = 1; k < rxlen; k++) rxbuf[k - 1] = rxbuf[k];
            rxlen -= 1;
            continue;
        }
        uint32_t total = SWARM_HDR_LEN + len + 1;
        if (rxlen < total) return;              /* frame not fully arrived yet */

        uint8_t crc = swarm_crc8(&rxbuf[1], 3 + len);
        if (crc == rxbuf[SWARM_HDR_LEN + len]) {
            dispatch_frame(rxbuf[1], &rxbuf[SWARM_HDR_LEN], len);
        }
        /* consume the frame (valid or crc-bad — either way it is complete) */
        for (uint32_t k = total; k < rxlen; k++) rxbuf[k - total] = rxbuf[k];
        rxlen -= total;
    }
}

void _start(void) {
    long restarts = svc_restarts();
    if (restarts > 0) {
        logline("SWARM: bridge REBORN — re-attesting after watchdog restart");
    } else {
        logline("SWARM: bridge online in ring 3 — COM2 serial swarm bridge");
    }

    /* Job 1: the Lamport-signed boot attestation, out COM2 + console gate. */
    emit_boot_attestation();

    /* Learn ghostd's pid for DATA routing (best-effort; PING still works). */
    discover_ghostd();

    /* Job 2: serve the bridge protocol. */
    while (1) {
        poll_com2();
        heartbeat();
        yield();
    }
}
