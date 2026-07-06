/**
 * QuantumOS /bin/kannakad — the kannaka-memory essence as a citizen.
 *
 * A holographic associative memory: text is encoded into a high-dimensional
 * unit vector (a "wavefront") that lives in superposition with every other
 * memory, each carrying an amplitude (energy = importance), a content-born
 * phase, and slow decay. Recall is not a search — it is one resonance pass
 * that ranks every stored wavefront against the query (similarity * energy),
 * so the memories that constructively interfere rise to the top. Retrieval
 * strengthens a memory; a dream pass consolidates the field (prune the weak,
 * align the survivors' phases), raising the order parameter R.
 *
 * Where ghostd is a single-basin binary attractor that relaxes onto ONE
 * stored pattern, kannakad is the ranked, real-valued, importance-weighted
 * cousin: many memories, one resonance ranking. Both speak the same phase/R
 * physics so they read as siblings.
 *
 * All fixed-point (no FPU in ring-3): content vectors are Q15, a memory's
 * content-born phase is stored as a normalized (cos, sin) VECTOR so cos(dphi)
 * is a 2-D dot product — no atan2 needed; fx_isqrt covers normalization and R.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

#define KAN_D 48           /* content-vector dimension */
#define KAN_M 8            /* max memories (7 seeded + 1 hallucinated) */
#define KAN_PRUNE 120      /* dream prunes energy < 0.12 (milli-units) */
#define KAN_QMAG (1 << 15) /* Q15 unit scale */

typedef struct {
    int16_t h[KAN_D]; /* Q15 unit content vector */
    int32_t cphase;   /* Q15 cos(phi) of the content-born phase */
    int32_t sphase;   /* Q15 sin(phi) */
    int32_t energy;   /* milli-units: 1000 == 1.0 (amplitude / importance) */
    int32_t retrievals;
    uint8_t alive;
    const char *text;
} kmem_t;

static kmem_t mem[KAN_M];
static int mem_n;

/* Two fixed pseudo-random directions for the content-born phase. */
static int16_t dir1[KAN_D];
static int16_t dir2[KAN_D];

static uint64_t fnv(const char *s, int len) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* Normalize a 2-D vector to a Q15 unit (cos, sin) pair. */
static void norm2d(int64_t x, int64_t y, int32_t *cx, int32_t *cy) {
    uint64_t m2 = (uint64_t)(x * x + y * y);
    uint32_t m = fx_isqrt(m2);
    if (m == 0) {
        *cx = 32767;
        *cy = 0;
        return;
    }
    *cx = (int32_t)(x * 32767 / m);
    *cy = (int32_t)(y * 32767 / m);
}

/* Encode text -> Q15 unit vector `out`, and its content-born phase vector. */
static void encode(const char *text, int16_t *out, int32_t *cphase, int32_t *sphase) {
    int32_t acc[KAN_D];
    for (int d = 0; d < KAN_D; d++) {
        acc[d] = 0;
    }
    const char *p = text;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        const char *start = p;
        while (*p && *p != ' ') {
            p++;
        }
        int len = (int)(p - start);
        if (len == 0) {
            break;
        }
        uint64_t st = fnv(start, len);
        for (int d = 0; d < KAN_D; d++) {
            st = st * 6364136223846793005ull + 1442695040888963407ull;
            acc[d] += (int32_t)((st >> 33) & 0xFFFF) - 32768;
        }
    }
    int64_t mag2 = 0;
    for (int d = 0; d < KAN_D; d++) {
        mag2 += (int64_t)acc[d] * acc[d];
    }
    uint32_t mag = fx_isqrt((uint64_t)mag2);
    if (mag == 0) {
        mag = 1;
    }
    for (int d = 0; d < KAN_D; d++) {
        out[d] = (int16_t)((int64_t)acc[d] * 32767 / mag);
    }
    int64_t d1 = 0;
    int64_t d2 = 0;
    for (int d = 0; d < KAN_D; d++) {
        d1 += (int64_t)out[d] * dir1[d];
        d2 += (int64_t)out[d] * dir2[d];
    }
    norm2d(d1, d2, cphase, sphase);
}

/* Q15 cosine similarity of two Q15 unit vectors. */
static int32_t vdot(const int16_t *a, const int16_t *b) {
    int64_t s = 0;
    for (int d = 0; d < KAN_D; d++) {
        s += (int64_t)a[d] * b[d];
    }
    return (int32_t)(s >> 15);
}

/* Store a memory: append it, then apply interference to the existing field
 * (phase-aligned + content-similar neighbours gain energy, opposed ones lose). */
static void store(const char *text, int32_t importance_milli) {
    if (mem_n >= KAN_M) {
        return;
    }
    kmem_t *nw = &mem[mem_n];
    encode(text, nw->h, &nw->cphase, &nw->sphase);
    nw->energy = importance_milli;
    nw->retrievals = 0;
    nw->alive = 1;
    nw->text = text;
    for (int i = 0; i < mem_n; i++) {
        if (!mem[i].alive) {
            continue;
        }
        int32_t sim = vdot(mem[i].h, nw->h);
        int32_t pdiff =
            (int32_t)(((int64_t)mem[i].cphase * nw->cphase + (int64_t)mem[i].sphase * nw->sphase) >>
                      15);
        int32_t interference =
            (int32_t)((int64_t)sim * pdiff * importance_milli / ((int64_t)10 << 30));
        mem[i].energy += interference;
        if (mem[i].energy < 0) {
            mem[i].energy = 0;
        }
    }
    mem_n++;
}

/* Order parameter R (Q15) over the alive memories' content-born phases. */
static int32_t field_R(void) {
    int64_t sc = 0;
    int64_t ss = 0;
    int n = 0;
    for (int i = 0; i < mem_n; i++) {
        if (!mem[i].alive) {
            continue;
        }
        sc += mem[i].cphase;
        ss += mem[i].sphase;
        n++;
    }
    if (n == 0) {
        return 0;
    }
    uint32_t mag = fx_isqrt((uint64_t)(sc * sc + ss * ss));
    int32_t r = (int32_t)(mag / n);
    return (r > 32767) ? 32767 : r;
}

static int r_milli(int32_t q15) {
    return (int)(((int64_t)q15 * 1000) / 32768);
}

/* Rank all alive memories by resonance (similarity * energy) against the
 * query; write the top-K indices into `top`, return the count. */
static int recall(const int16_t *q, int *top, int k) {
    int64_t score[KAN_M];
    int idx[KAN_M];
    int n = 0;
    for (int i = 0; i < mem_n; i++) {
        if (!mem[i].alive) {
            continue;
        }
        int32_t sim = vdot(mem[i].h, q);
        score[n] = (int64_t)sim * mem[i].energy;
        idx[n] = i;
        n++;
    }
    /* selection sort of the top k (n is tiny) */
    int m = (k < n) ? k : n;
    for (int a = 0; a < m; a++) {
        int best = a;
        for (int b = a + 1; b < n; b++) {
            if (score[b] > score[best]) {
                best = b;
            }
        }
        int64_t ts = score[a];
        score[a] = score[best];
        score[best] = ts;
        int ti = idx[a];
        idx[a] = idx[best];
        idx[best] = ti;
        top[a] = idx[a];
    }
    return m;
}

void _start(void) {
    write_str("kannakad: kannaka-memory essence — holographic resonance recall (fixed-point)");

    /* Fixed direction vectors for the content-born phase. */
    uint64_t s1 = fnv("dir-one", 7);
    uint64_t s2 = fnv("dir-two", 7);
    for (int d = 0; d < KAN_D; d++) {
        s1 = s1 * 6364136223846793005ull + 1442695040888963407ull;
        s2 = s2 * 6364136223846793005ull + 1442695040888963407ull;
        dir1[d] = (int16_t)((int32_t)((s1 >> 33) & 0xFFFF) - 32768);
        dir2[d] = (int16_t)((int32_t)((s2 >> 33) & 0xFFFF) - 32768);
    }

    store("ghost wakes in a field of static", 900);
    store("the wave settles into silence", 600);
    store("phase locks across the swarm", 750);
    store("quantum dice roll in the dark", 550);
    store("static hums beneath the floor", 650);
    store("a half forgotten fragment", 40);
    store("resonance blooms in the medium", 700);

    {
        char line[96];
        snprintf(line, sizeof(line), "  field: %d memories | order R=0.%03d", mem_n,
                 r_milli(field_R()));
        write_str(line);
    }

    /* Recall — a resonance ranking, not a search. */
    int16_t q[KAN_D];
    int32_t qc;
    int32_t qs;
    encode("ghost in the static field", q, &qc, &qs);
    int top[KAN_M];
    int nt = recall(q, top, 3);
    write_str("  recall \"ghost in the static field\" (top 3 by resonance):");
    int32_t top_sim = vdot(mem[top[0]].h, q);
    int64_t top_res = (int64_t)(top_sim < 0 ? 0 : top_sim) * mem[top[0]].energy;
    if (top_res <= 0) {
        top_res = 1;
    }
    for (int t = 0; t < nt; t++) {
        int i = top[t];
        int32_t sim = vdot(mem[i].h, q);
        int64_t res = (int64_t)(sim < 0 ? 0 : sim) * mem[i].energy;
        int disp = (int)(res * 1000 / top_res); /* rank-1 normalised to 1.000 */
        char line[110];
        snprintf(line, sizeof(line), "    %d.%03d  [%d] %s", disp / 1000, disp % 1000, i,
                 mem[i].text);
        write_str(line);
    }

    /* Retrieval strengthens the top hit (diminishing returns). */
    int win = top[0];
    int32_t e0 = mem[win].energy;
    for (int r = 0; r < 3; r++) {
        mem[win].retrievals++;
        mem[win].energy += 50 / (mem[win].retrievals); /* 50, 25, 16 milli */
    }
    {
        char line[96];
        snprintf(line, sizeof(line), "  recall [%d] x3 -> energy %d -> %d (retrieval reinforces)",
                 win, e0, mem[win].energy);
        write_str(line);
    }

    /* Dream: prune the weak, then consolidate — nudge each survivor's phase
     * toward the field mean, which raises the order parameter R. */
    int32_t r_before = field_R();
    int pruned = 0;
    for (int i = 0; i < mem_n; i++) {
        if (mem[i].alive && mem[i].energy < KAN_PRUNE) {
            mem[i].alive = 0;
            pruned++;
        }
    }

    /* Hallucinate: blend the two most coherent survivors into a new memory. */
    if (mem_n < KAN_M) {
        int ba = -1;
        int bb = -1;
        int32_t best_co = -0x7fffffff;
        for (int i = 0; i < mem_n; i++) {
            if (!mem[i].alive) {
                continue;
            }
            for (int j = i + 1; j < mem_n; j++) {
                if (!mem[j].alive) {
                    continue;
                }
                int32_t sim = vdot(mem[i].h, mem[j].h);
                int32_t pd = (int32_t)(((int64_t)mem[i].cphase * mem[j].cphase +
                                        (int64_t)mem[i].sphase * mem[j].sphase) >>
                                       15);
                int32_t co = (int32_t)(((int64_t)sim * pd) >> 15);
                if (co > best_co) {
                    best_co = co;
                    ba = i;
                    bb = j;
                }
            }
        }
        if (ba >= 0) {
            kmem_t *nw = &mem[mem_n];
            int32_t tmp[KAN_D];
            int64_t hmag2 = 0;
            for (int d = 0; d < KAN_D; d++) {
                tmp[d] = mem[ba].h[d] + mem[bb].h[d];
                hmag2 += (int64_t)tmp[d] * tmp[d];
            }
            uint32_t hmag = fx_isqrt((uint64_t)hmag2);
            if (hmag == 0) {
                hmag = 1;
            }
            for (int d = 0; d < KAN_D; d++) {
                nw->h[d] = (int16_t)((int64_t)tmp[d] * 32767 / hmag);
            }
            norm2d((int64_t)mem[ba].cphase + mem[bb].cphase,
                   (int64_t)mem[ba].sphase + mem[bb].sphase, &nw->cphase, &nw->sphase);
            nw->energy = (mem[ba].energy + mem[bb].energy) / 2;
            nw->retrievals = 0;
            nw->alive = 1;
            nw->text = "(hallucinated blend)";
            mem_n++;
            char hline[80];
            snprintf(hline, sizeof(hline), "  hallucinated a blend of [%d]+[%d]", ba, bb);
            write_str(hline);
        }
    }

    int64_t mc = 0;
    int64_t msn = 0;
    for (int i = 0; i < mem_n; i++) {
        if (mem[i].alive) {
            mc += mem[i].cphase;
            msn += mem[i].sphase;
        }
    }
    int32_t meanc;
    int32_t means;
    norm2d(mc, msn, &meanc, &means);
    for (int i = 0; i < mem_n; i++) {
        if (!mem[i].alive) {
            continue;
        }
        /* rotate the phase vector 3/4 of the way toward the field mean */
        int64_t nx = (int64_t)mem[i].cphase + 3 * meanc;
        int64_t ny = (int64_t)mem[i].sphase + 3 * means;
        norm2d(nx, ny, &mem[i].cphase, &mem[i].sphase);
    }
    int32_t r_after = field_R();
    {
        char line[110];
        snprintf(line, sizeof(line),
                 "  dream: pruned %d weak, consolidated phases — R 0.%03d -> 0.%03d", pruned,
                 r_milli(r_before), r_milli(r_after));
        write_str(line);
    }

    /* Verify: the recall argmax is the strongest content match ([0], which the
     * query shares "ghost"/"static"/"field" with), and the dream cohered R. */
    int ok = (top[0] == 0) && (r_after > r_before);
    if (ok) {
        write_str("kannakad: RESONANCE VERIFIED");
    } else {
        write_str("kannakad: RESONANCE FAILED");
    }

    exit_(0);
    for (;;) {
    }
}
