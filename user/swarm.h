/**
 * QuantumOS swarm bridge — wire protocol + Lamport attestation parameters
 * (ghostd phase 4, #51; epic #47).
 *
 * swarm_svc speaks a small length-prefixed, CRC8-framed protocol on the COM2
 * UART. The framing is modelled on PlanetaryQuantumNetwork's MessageType
 * scheme (HANDSHAKE/DATA/PING/PONG/DISCONNECT over length-prefixed frames);
 * three extra frame types carry the boot attestation.
 *
 *   Frame layout (all little-endian):
 *     +------+------+--------+---------------+------+
 *     | 0xA5 | type | len:u16| payload[len]  | crc8 |
 *     +------+------+--------+---------------+------+
 *   magic = SWARM_MAGIC (0xA5). crc8 = CRC-8/CCITT (poly 0x07, init 0x00)
 *   over the type byte, the two length bytes, and the payload — i.e. every
 *   byte of the frame except the magic and the crc itself.
 *
 * Boot attestation (emitted once at startup):
 *   FRAME_PKDIGEST : 32-byte SHA-256 digest committing to the Lamport public
 *                    key (pk[0][0]‖pk[0][1]‖…‖pk[255][1]).
 *   FRAME_ATTEST   : the ASCII attestation string
 *                    "QOS-BOOT|qseed=<hex|none>|ticks=<n>".
 *   FRAME_SIG      : the signature, LAMPORT_BITS elements of 64 bytes each
 *                    (one revealed preimage + the complementary public-key
 *                    hash), chunked across as many frames as needed and
 *                    concatenated in receive order by the verifier.
 *
 * Lamport hash-based one-time signature (all hashing is SHA-256, see sha256.h):
 *   - digest md = SHA-256(attestation message), LAMPORT_BITS = 256 bits.
 *   - secret key: 256 pairs of 32-byte preimages, expanded in counter mode
 *     from a single 32-byte master seed drawn from the quantum pool:
 *         sk[i][b] = SHA-256(master_seed ‖ be32(i) ‖ b)
 *     so only 32 bytes of true entropy are consumed, not 16 KB; security then
 *     reduces to SHA-256 (preimage resistance) plus the 256-bit seed.
 *   - public key: pk[i][b] = SHA-256(sk[i][b]); the digest above commits to it.
 *   - signature element i (for message bit b_i): { sk[i][b_i], pk[i][1-b_i] }.
 *     Revealing the preimage proves knowledge of the secret; shipping the
 *     complementary pk hash lets the verifier rebuild the whole public key and
 *     check it against the committed digest. One message per key pair only.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef SWARM_H
#define SWARM_H

#include <stdint.h>

/* ---- framing ---- */
#define SWARM_MAGIC 0xA5u
#define SWARM_HDR_LEN 4       /* magic + type + len(2) */
#define SWARM_MAX_PAYLOAD 512 /* keep frames small for the polled UART */

/* Frame types */
#define FRAME_HANDSHAKE 0x01u
#define FRAME_DATA 0x02u
#define FRAME_PING 0x03u
#define FRAME_PONG 0x04u
#define FRAME_DISCONNECT 0x05u
#define FRAME_PKDIGEST 0x10u /* 32-byte Lamport public-key commitment */
#define FRAME_ATTEST 0x11u   /* attestation ASCII string */
#define FRAME_SIG 0x12u      /* signature chunk (concatenated in order) */

/* ---- DATA routing opcodes (payload[0]) ----
 * A DATA frame is a remote request routed over capability-checked IPC to a
 * ghostOS service and answered with a DATA frame carrying the reply. Kept
 * deliberately tiny and explicit. */
#define SWARM_OP_STATUS 0x01u /* -> ghostd GHOST_STATUS : field R + live */
#define SWARM_OP_RECALL 0x02u /* -> ghostd GHOST_RECALL : payload = 32B probe */
#define SWARM_OP_QSUBMIT                                                                           \
    0x03u /* -> SYS_QPU broker: payload = opaque circuit (epic #149 B1).
           * Reply: [0x03][status u8][result QC_RESULT_LEN on OK].
           * status: 0=OK, 1=refused (quota/EPERM), 2=broker/EINVAL error. */

/* ---- Lamport parameters ---- */
#define LAMPORT_BITS 256    /* SHA-256 digest width */
#define LAMPORT_HASH_LEN 32 /* SHA-256 output bytes */
#define LAMPORT_SEED_LEN 32 /* master-seed bytes drawn from the pool */
#define LAMPORT_SIG_ELEM 64 /* preimage(32) + complementary pk hash(32) */
#define LAMPORT_SIG_LEN (LAMPORT_BITS * LAMPORT_SIG_ELEM) /* 16384 bytes */

/* CRC-8/CCITT (poly 0x07, init 0x00), MSB-first — matches verify_attestation.py. */
static inline uint8_t swarm_crc8(const uint8_t *data, uint32_t len) {
    uint8_t crc = 0x00u;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

#endif /* SWARM_H */
