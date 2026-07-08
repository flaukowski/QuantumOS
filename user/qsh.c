/**
 * QuantumOS qsh — the interactive shell (epic #62 phase 1, #63).
 *
 * A ring-3 user-process service, watchdog-monitored like every other
 * citizen. Its entire authority is what it was granted: the console
 * device capability (keystrokes in, raw bytes out), a quantum-pool read
 * capability (the qrand/qseed builtins), and one IPC send-capability to
 * ghostd (the ghost builtin). It can inspect the live system through the
 * uncapped read-only SYS_SYSINFO, and nothing it does can reach past
 * those grants.
 *
 * Line discipline lives HERE, not in the kernel: the kernel hands over
 * raw bytes; qsh echoes, handles backspace, and assembles lines. `exit`
 * is a supervised death — the watchdog notices the silent heartbeat and
 * restarts the shell, which then introduces itself as reborn. That
 * banner is the merge-gate proof that the operator surface survives its
 * own crash.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ghost.h" /* usys.h + wire structs + freestanding string builders */

#define LINE_MAX 120

/* ------------------------------------------------------------------ *
 * Output helpers — every logical line is written with ONE SYS_CONS
 * call (< 256 bytes), so a timer-tick boot_log can never split it.
 * ------------------------------------------------------------------ */

static void out_bytes(const char *b, long n) {
    long off = 0;
    while (off < n) {
        long w = cons_write(b + off, n - off);
        if (w <= 0) {
            break;
        }
        off += w;
    }
}

static void out(const char *s) {
    out_bytes(s, str_len(s));
}

static void prompt(void) {
    out("qsh> ");
}

static int put_hex64(char *b, int o, unsigned long long v) {
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xF);
        b[o++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    return o;
}

/* ------------------------------------------------------------------ *
 * Command matching (no libc)
 * ------------------------------------------------------------------ */

static int is_cmd(const char *line, const char *cmd) {
    int i = 0;
    while (cmd[i] && line[i] == cmd[i]) {
        i++;
    }
    return cmd[i] == '\0' && line[i] == '\0';
}

/* Returns the argument after "<cmd> ", or 0 if line doesn't start so. */
static const char *arg_of(const char *line, const char *cmd) {
    int i = 0;
    while (cmd[i] && line[i] == cmd[i]) {
        i++;
    }
    if (cmd[i] == '\0' && line[i] == ' ') {
        while (line[i] == ' ') {
            i++;
        }
        return line + i;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Builtins
 * ------------------------------------------------------------------ */

static void cmd_help(void) {
    out("qsh: commands: help echo ps free uptime date pid ls cat write rm sync imprint recall "
        "fieldtest nslookup udping http run qrand qseed ghost clear exit\r\n");
}

/* write <path> <text>: create/truncate an overlay file with the text
 * (the shell's filesystem-write capability at work). */
static void cmd_write(const char *args) {
    /* Split "<path> <text...>" */
    char path[64];
    int p = 0;
    while (args[p] && args[p] != ' ' && p < (int)sizeof(path) - 1) {
        path[p] = args[p];
        p++;
    }
    path[p] = '\0';
    const char *text = args + p;
    while (*text == ' ') {
        text++;
    }
    if (!path[0] || !*text) {
        out("qsh: write: usage: write <path> <text>\r\n");
        return;
    }

    long fd = openf_(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        char b[96];
        int o = ghost_put(b, 0, "qsh: write: cannot create (err ");
        o = ghost_put_u(b, o, (unsigned)(-fd));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
        return;
    }

    long total = 0;
    long len = str_len(text);
    while (total < len) {
        long w = fwrite_(fd, text + total, len - total);
        if (w <= 0) {
            break;
        }
        total += w;
    }
    fwrite_(fd, "\n", 1);
    close_(fd);

    char b[96];
    int o = ghost_put(b, 0, "qsh: wrote ");
    o = ghost_put_u(b, o, (unsigned)total);
    o = ghost_put(b, o, " bytes to ");
    for (int i = 0; path[i] && o < (int)sizeof(b) - 4; i++) {
        b[o++] = path[i];
    }
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

static void cmd_rm(const char *path) {
    long r = unlink_(path);
    if (r == 0) {
        out("qsh: removed\r\n");
    } else {
        char b[96];
        int o = ghost_put(b, 0, "qsh: rm: failed (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
    }
}

/* ------------------------------------------------------------------ *
 * Kernel holographic field (epic #95): the shell holds the
 * CAP_RESOURCE_FIELD capability over region 0, so the operator gets
 * associative memory at the prompt — imprint text, recall it later
 * from a corrupted probe. The "FIELD:" output prefixes are merge
 * gates; their line forms cannot be produced by echoed input.
 * ------------------------------------------------------------------ */
#define QSH_FIELD_REGION 0u

static void cmd_imprint(const char *args) {
    long n = str_len(args);
    if (n <= 0) {
        out("qsh: imprint: usage: imprint <text>\r\n");
        return;
    }
    if (n > FIELD_PAT_MAX) {
        n = FIELD_PAT_MAX;
    }
    field_imprint_req_t req;
    req.region = QSH_FIELD_REGION;
    req.len = (unsigned)n;
    req.energy_q15 = 0; /* kernel default */
    for (long i = 0; i < FIELD_PAT_MAX; i++) {
        req.pattern[i] = i < n ? (unsigned char)args[i] : 0;
    }
    long r = imprint_(&req);
    if (r < 0) {
        char b[96];
        int o = ghost_put(b, 0, "qsh: imprint failed (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
        return;
    }
    char b[96];
    int o = ghost_put(b, 0, "FIELD: imprinted slot ");
    o = ghost_put_u(b, o, (unsigned)r);
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

static void cmd_recall(const char *args) {
    long n = str_len(args);
    if (n <= 0) {
        out("qsh: recall: usage: recall <probe text>\r\n");
        return;
    }
    if (n > FIELD_PAT_MAX) {
        n = FIELD_PAT_MAX;
    }
    field_recall_req_t req;
    req.region = QSH_FIELD_REGION;
    req.len = (unsigned)n;
    req.k = 3;
    for (long i = 0; i < FIELD_PAT_MAX; i++) {
        req.probe[i] = i < n ? (unsigned char)args[i] : 0;
    }
    field_recall_out_t res;
    long r = recall_(&req, &res);
    if (r < 0) {
        char b[96];
        int o = ghost_put(b, 0, "qsh: recall failed (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
        return;
    }
    if (res.n == 0) {
        out("FIELD: recall empty (n=0)\r\n");
        return;
    }
    char b[192];
    int o = ghost_put(b, 0, "FIELD: winner=\"");
    for (unsigned i = 0; i < res.winner_len && o < (int)sizeof(b) - 40; i++) {
        b[o++] = (char)res.winner[i];
    }
    o = ghost_put(b, o, "\" slot=");
    o = ghost_put_u(b, o, res.rank[0].slot);
    o = ghost_put(b, o, " score=");
    int sc = res.rank[0].score_q15;
    if (sc < 0) {
        b[o++] = '-';
        sc = -sc;
    }
    o = ghost_put_u(b, o, (unsigned)sc);
    o = ghost_put(b, o, " n=");
    o = ghost_put_u(b, o, res.n);
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

/* fieldtest: assert the field's two negative contracts from the one
 * process that HOLDS a field cap (ghost_test proves the capless side):
 * a cross-region request must be exactly EPERM even for a cap holder,
 * and a degenerate probe must be a clean n=0, never a kernel fault. */
static void cmd_fieldtest(void) {
    field_imprint_req_t req;
    req.region = QSH_FIELD_REGION + 1; /* a region the shell holds NO cap for */
    req.len = 5;
    req.energy_q15 = 0;
    for (int i = 0; i < FIELD_PAT_MAX; i++) {
        req.pattern[i] = 0;
    }
    req.pattern[0] = 'c';
    req.pattern[1] = 'r';
    req.pattern[2] = 'o';
    req.pattern[3] = 's';
    req.pattern[4] = 's';
    long r = imprint_(&req);
    if (r == -4) {
        out("FIELD: cross-region denied (EPERM)\r\n");
    } else {
        char b[96];
        int o = ghost_put(b, 0, "FIELD: cross-region NOT denied (ret ");
        o = ghost_put_u(b, o, (unsigned)(r < 0 ? -r : r));
        o = ghost_put(b, o, ") — isolation broken\r\n");
        out_bytes(b, o);
    }

    field_recall_req_t rr;
    rr.region = QSH_FIELD_REGION;
    rr.len = 4;
    rr.k = 1;
    for (int i = 0; i < FIELD_PAT_MAX; i++) {
        rr.probe[i] = 0;
    }
    rr.probe[0] = ' ';
    rr.probe[1] = ' ';
    rr.probe[2] = ' ';
    rr.probe[3] = ' '; /* all-equal bytes: degenerate on purpose */
    field_recall_out_t res;
    r = recall_(&rr, &res);
    if (r == 0 && res.n == 0) {
        out("FIELD: empty-probe ok (n=0)\r\n");
    } else {
        out("FIELD: empty-probe MISBEHAVED\r\n");
    }
}

/* nslookup <host>: resolve a hostname to an IPv4 address via SYS_RESOLVE
 * (the shell's network capability at work). The kernel does the lookup
 * in its net thread; we poll, yielding+heartbeating so a slow lookup
 * doesn't get the shell watchdog-killed. */
static void cmd_nslookup(const char *host) {
    unsigned char ip[4];
    long r = RESOLVE_WOULDBLOCK;
    for (int tries = 0; tries < 2000 && r == RESOLVE_WOULDBLOCK; tries++) {
        heartbeat();
        r = resolve_(host, ip);
        if (r == RESOLVE_WOULDBLOCK) {
            yield();
        }
    }
    if (r == 0) {
        char b[96];
        int o = ghost_put(b, 0, "qsh: ");
        for (int i = 0; host[i] && o < (int)sizeof(b) - 32; i++) {
            b[o++] = host[i];
        }
        o = ghost_put(b, o, " -> ");
        for (int i = 0; i < 4; i++) {
            o = ghost_put_u(b, o, ip[i]);
            if (i < 3) {
                b[o++] = '.';
            }
        }
        o = ghost_put(b, o, "\r\n");
        out_bytes(b, o);
    } else if (r == -4) {
        out("qsh: nslookup: denied (no network capability)\r\n");
    } else if (r == RESOLVE_WOULDBLOCK) {
        out("qsh: nslookup: timed out\r\n");
    } else {
        out("qsh: nslookup: no network / lookup failed\r\n");
    }
}

/* ------------------------------------------------------------------ *
 * udping <host>: a USERSPACE DNS client over the SYS_UDP socket API
 * (epic #80). Where nslookup asks the kernel to resolve, udping builds
 * the DNS A-query itself in ring 3, sends it as a raw datagram to
 * SLIRP's proxy (10.0.2.3:53) with UDP_SENDTO, receives the raw reply
 * with UDP_RECVFROM, and parses the answer itself — proving user
 * programs can speak arbitrary UDP through the capability-gated
 * socket API.
 * ------------------------------------------------------------------ */

/* Build a DNS A query for `name`. Returns the query length, 0 if the
 * name doesn't fit. */
static int dns_query_build(unsigned char *q, int max, const char *name, unsigned txid) {
    if (max < 18) {
        return 0;
    }
    q[0] = (unsigned char)(txid >> 8);
    q[1] = (unsigned char)txid;
    q[2] = 0x01; /* recursion desired */
    q[3] = 0x00;
    q[4] = 0x00;
    q[5] = 0x01; /* qdcount = 1 */
    for (int i = 6; i < 12; i++) {
        q[i] = 0;
    }
    int p = 12;
    int label = p++;
    int llen = 0;
    for (int i = 0; name[i]; i++) {
        if (p >= max - 5) {
            return 0;
        }
        if (name[i] == '.') {
            q[label] = (unsigned char)llen;
            label = p++;
            llen = 0;
        } else {
            if (llen >= 63) {
                return 0; /* RFC 1035: labels cap at 63 bytes — 64+ would
                           * collide with the compression-pointer tag */
            }
            q[p++] = (unsigned char)name[i];
            llen++;
        }
    }
    q[label] = (unsigned char)llen;
    q[p++] = 0x00; /* root label */
    q[p++] = 0x00; /* QTYPE A = 1 */
    q[p++] = 0x01;
    q[p++] = 0x00; /* QCLASS IN = 1 */
    q[p++] = 0x01;
    return p;
}

/* Advance past a (possibly compressed) DNS name. */
static int dns_skip_name(const unsigned char *r, int len, int off) {
    while (off < len) {
        unsigned char c = r[off];
        if (c == 0) {
            return off + 1;
        }
        if ((c & 0xC0) == 0xC0) {
            return off + 2; /* a compression pointer ends the name */
        }
        off += c + 1;
    }
    return len;
}

/* Find the first A record in a DNS response. 1 + ip filled, or 0. */
static int dns_reply_parse(const unsigned char *r, int len, unsigned char *ip) {
    if (len < 12) {
        return 0;
    }
    int qd = (r[4] << 8) | r[5];
    int an = (r[6] << 8) | r[7];
    int off = 12;
    for (int q = 0; q < qd; q++) {
        off = dns_skip_name(r, len, off) + 4; /* + QTYPE/QCLASS */
    }
    for (int a = 0; a < an; a++) {
        off = dns_skip_name(r, len, off);
        if (off + 10 > len) {
            return 0;
        }
        int type = (r[off] << 8) | r[off + 1];
        int rdlen = (r[off + 8] << 8) | r[off + 9];
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= len) {
            for (int i = 0; i < 4; i++) {
                ip[i] = r[off + i];
            }
            return 1;
        }
        off += rdlen;
    }
    return 0;
}

/* Send the query to 10.0.2.3:53, retrying WOULDBLOCK (the net thread
 * may be busy with a kernel resolve). Bounded, heartbeating. */
static long udping_send(long sock, unsigned char *query, int qlen) {
    udp_req_t req;
    req.sock = sock;
    req.ip[0] = 10;
    req.ip[1] = 0;
    req.ip[2] = 2;
    req.ip[3] = 3;
    req.port = 53;
    req.len = (unsigned short)qlen;
    req.buf = query;
    long r = UDP_WOULDBLOCK;
    for (int tries = 0; tries < 2000 && r == UDP_WOULDBLOCK; tries++) {
        heartbeat();
        r = udp_(UDP_SENDTO, &req);
        if (r == UDP_WOULDBLOCK) {
            yield();
        }
    }
    return r;
}

static void cmd_udping(const char *host) {
    udp_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((unsigned char *)&req)[i] = 0;
    }
    long sock = udp_(UDP_BIND, &req); /* port 0 = ephemeral */
    if (sock == -4) {
        out("qsh: udping: denied (no network capability)\r\n");
        return;
    }
    if (sock < 0) {
        out("qsh: udping: no socket (no network?)\r\n");
        return;
    }

    unsigned char query[288];
    unsigned txid = (unsigned)ticks() & 0xFFFFu;
    int qlen = dns_query_build(query, (int)sizeof(query), host, txid);
    if (qlen == 0) {
        req.sock = sock;
        udp_(UDP_CLOSE, &req);
        out("qsh: udping: hostname too long\r\n");
        return;
    }

    long sr = udping_send(sock, query, qlen);
    if (sr < 0) {
        req.sock = sock;
        udp_(UDP_CLOSE, &req);
        out(sr == UDP_WOULDBLOCK ? "qsh: udping: send queue busy\r\n"
                                 : "qsh: udping: send failed\r\n");
        return;
    }

    /* Poll for the reply, TIME-bounded (~8s at 100 Hz: resolver latency
     * is wall-clock, not an iteration count). Validate the datagram is
     * really our answer — right sender, right txid; strays are skipped.
     * One retransmit halfway in case the first query was lost. */
    unsigned char resp[512];
    long t0 = ticks();
    long got = -1;
    int resent = 0;
    while (ticks() - t0 < 800) {
        heartbeat();
        req.sock = sock;
        req.len = (unsigned short)sizeof(resp);
        req.buf = resp;
        long rr = udp_(UDP_RECVFROM, &req);
        if (rr == UDP_WOULDBLOCK) {
            if (!resent && ticks() - t0 > 400) {
                resent = 1;
                udping_send(sock, query, qlen);
            }
            yield();
            continue;
        }
        if (rr < 0) {
            break;
        }
        if (req.ip[0] == 10 && req.ip[1] == 0 && req.ip[2] == 2 && req.ip[3] == 3 &&
            req.port == 53 && rr >= 12 && resp[0] == (unsigned char)(txid >> 8) &&
            resp[1] == (unsigned char)txid) {
            got = rr;
            break;
        }
    }

    /* Single close point BEFORE reporting: qsh never exits, so a slot
     * leaked on any path here would be leaked for good (4 exist). */
    req.sock = sock;
    udp_(UDP_CLOSE, &req);

    if (got < 0) {
        out("qsh: udping: timed out\r\n");
        return;
    }

    char b[96];
    int o = ghost_put(b, 0, "qsh: udp ");
    o = ghost_put_u(b, o, (unsigned)got);
    o = ghost_put(b, o, " bytes from 10.0.2.3:53\r\n");
    out_bytes(b, o);

    unsigned char ip[4];
    if (dns_reply_parse(resp, (int)got, ip)) {
        o = ghost_put(b, 0, "qsh: udpdns ");
        for (int i = 0; host[i] && o < (int)sizeof(b) - 40; i++) {
            b[o++] = host[i];
        }
        o = ghost_put(b, o, " -> ");
        for (int i = 0; i < 4; i++) {
            o = ghost_put_u(b, o, ip[i]);
            if (i < 3) {
                b[o++] = '.';
            }
        }
        o = ghost_put(b, o, "\r\n");
        out_bytes(b, o);
    } else {
        o = ghost_put(b, 0, "qsh: udpdns ");
        for (int i = 0; host[i] && o < (int)sizeof(b) - 24; i++) {
            b[o++] = host[i];
        }
        o = ghost_put(b, o, ": no A record\r\n");
        out_bytes(b, o);
    }
}

/* ------------------------------------------------------------------ *
 * http <host> [port]: fetch a web page over the ring-3 TCP client
 * (epic #82). Resolves the name (SYS_RESOLVE — two syscalls compose),
 * opens a connection, sends one HTTP/1.0 GET, reads the response to EOF,
 * and prints the status line + byte count. All polling is time-bounded
 * (heartbeat+yield so a slow fetch can't get the shell watchdog-killed),
 * and the socket is closed on every exit path (qsh never exits — a
 * leaked connection would wedge the single TCB).
 * ------------------------------------------------------------------ */

/* Parse "a.b.c.d" into ip[4]; returns 1 on success. Stops at NUL/space. */
static int parse_ipv4(const char *s, unsigned char *ip) {
    int vals[4];
    int vi = 0, cur = 0, digits = 0;
    for (int i = 0;; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            digits++;
            if (cur > 255) {
                return 0;
            }
        } else if (c == '.' || c == '\0' || c == ' ') {
            if (digits == 0 || vi >= 4) {
                return 0;
            }
            vals[vi++] = cur;
            cur = 0;
            digits = 0;
            if (c == '\0' || c == ' ') {
                break;
            }
        } else {
            return 0;
        }
    }
    if (vi != 4) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        ip[i] = (unsigned char)vals[i];
    }
    return 1;
}

/* Poll-close the connection (bounded); ignores the outcome. */
static void http_close(tcp_req_t *req) {
    long t0 = ticks();
    for (int guard = 0; guard < 20000 && ticks() - t0 < 300; guard++) {
        heartbeat();
        long r = tcp_(TCP_CLOSE, req);
        if (r != UDP_WOULDBLOCK) {
            break;
        }
        yield();
    }
}

static void cmd_http(const char *args) {
    char host[64];
    int hlen = 0;
    while (args[hlen] && args[hlen] != ' ' && hlen < (int)sizeof(host) - 1) {
        host[hlen] = args[hlen];
        hlen++;
    }
    host[hlen] = '\0';

    const char *pp = args + hlen;
    while (*pp == ' ') {
        pp++;
    }
    unsigned port = 0;
    while (*pp >= '0' && *pp <= '9') {
        port = port * 10 + (unsigned)(*pp - '0');
        pp++;
    }
    if (port == 0) {
        port = 80;
    }

    /* Resolve the host (literal dotted-quad, or DNS via SYS_RESOLVE). */
    unsigned char ip[4];
    if (!parse_ipv4(host, ip)) {
        long r = RESOLVE_WOULDBLOCK;
        for (int tries = 0; tries < 2000 && r == RESOLVE_WOULDBLOCK; tries++) {
            heartbeat();
            r = resolve_(host, ip);
            if (r == RESOLVE_WOULDBLOCK) {
                yield();
            }
        }
        if (r == -4) {
            out("qsh: http: denied (no network capability)\r\n");
            return;
        }
        if (r != 0) {
            out("qsh: http: could not resolve host\r\n");
            return;
        }
    }

    tcp_req_t req;
    for (unsigned i = 0; i < sizeof(req); i++) {
        ((unsigned char *)&req)[i] = 0;
    }

    /* Connect (time-bounded poll). */
    long cr = UDP_WOULDBLOCK;
    long t0 = ticks();
    for (int guard = 0; guard < 200000 && ticks() - t0 < 600; guard++) {
        heartbeat();
        req.sock = 0;
        for (int i = 0; i < 4; i++) {
            req.ip[i] = ip[i];
        }
        req.port = (unsigned short)port;
        cr = tcp_(TCP_CONNECT, &req);
        if (cr == 0) {
            break;
        }
        if (cr != UDP_WOULDBLOCK) {
            break;
        }
        yield();
    }
    if (cr != 0) {
        http_close(&req);
        if (cr == -4) {
            out("qsh: http: denied (no network capability)\r\n");
        } else {
            out("qsh: http: connect failed\r\n");
        }
        return;
    }

    /* One HTTP/1.0 GET (<= MSS, sends in a single segment). */
    char get[256];
    int g = ghost_put(get, 0, "GET / HTTP/1.0\r\nHost: ");
    for (int i = 0; host[i] && g < (int)sizeof(get) - 32; i++) {
        get[g++] = host[i];
    }
    g = ghost_put(get, g, "\r\nConnection: close\r\n\r\n");

    long sres = UDP_WOULDBLOCK;
    t0 = ticks();
    for (int guard = 0; guard < 200000 && ticks() - t0 < 500; guard++) {
        heartbeat();
        req.buf = get;
        req.len = (unsigned short)g;
        sres = tcp_(TCP_SEND, &req);
        if (sres == g) {
            break;
        }
        if (sres != UDP_WOULDBLOCK) {
            break;
        }
        yield();
    }
    if (sres != g) {
        http_close(&req);
        out("qsh: http: send failed\r\n");
        return;
    }

    /* Read the response to EOF, capturing the first line and counting
     * bytes. The idle timer resets on progress so a large transfer isn't
     * cut short; only a genuine stall trips the bound. */
    char chunk[256];
    char status[80];
    int slen = 0, got_status = 0;
    long total = 0;
    int reset = 0;
    long tr = ticks();
    for (int guard = 0; guard < 2000000; guard++) {
        heartbeat();
        if (ticks() - tr > 800) {
            break;
        }
        req.buf = chunk;
        req.len = (unsigned short)sizeof(chunk);
        long rr = tcp_(TCP_RECV, &req);
        if (rr == 0) {
            break; /* clean EOF (peer FIN, ring drained) */
        }
        if (rr == UDP_WOULDBLOCK) {
            yield();
            continue;
        }
        if (rr < 0) {
            reset = 1; /* reset/error mid-stream — the fetch is truncated */
            break;
        }
        if (!got_status) {
            for (long i = 0; i < rr; i++) {
                char c = chunk[i];
                if (c == '\r' || c == '\n') {
                    got_status = 1;
                    break;
                }
                if (slen < (int)sizeof(status) - 1) {
                    status[slen++] = c;
                }
            }
        }
        total += rr;
        tr = ticks();
    }
    status[slen] = '\0';

    http_close(&req);

    if (!got_status) {
        out("qsh: http: no response (timed out)\r\n");
        return;
    }

    /* Status line — host label WITHOUT the port. */
    char b[128];
    int o = ghost_put(b, 0, "qsh: http ");
    for (int i = 0; host[i] && o < (int)sizeof(b) - 48; i++) {
        b[o++] = host[i];
    }
    o = ghost_put(b, o, " -> ");
    for (int i = 0; i < slen && o < (int)sizeof(b) - 4; i++) {
        b[o++] = status[i];
    }
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);

    o = ghost_put(b, 0, "qsh: http ");
    for (int i = 0; host[i] && o < (int)sizeof(b) - 40; i++) {
        b[o++] = host[i];
    }
    o = ghost_put(b, o, ": ");
    o = ghost_put_u(b, o, (unsigned)total);
    /* Be honest about a truncated transfer: a mid-stream reset drains what
     * was buffered and then returns an error, which must NOT read as a
     * clean fetch. */
    o = ghost_put(b, o, reset ? " bytes then connection reset\r\n" : " bytes received\r\n");
    out_bytes(b, o);
}

static void cmd_sync(void) {
    long r = sync_();
    if (r >= 0) {
        char b[64];
        int o = ghost_put(b, 0, "qsh: sync ok (");
        o = ghost_put_u(b, o, (unsigned)r);
        o = ghost_put(b, o, " files flushed)\r\n");
        out_bytes(b, o);
    } else if (r == -5) {
        out("qsh: sync failed (no disk)\r\n");
    } else {
        char b[64];
        int o = ghost_put(b, 0, "qsh: sync failed (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
    }
}

/* Print an initrd file raw (used for the motd greeting and `cat`).
 * Returns 0 on success, the negative errno if the open failed. */
static long print_file(const char *path) {
    long fd = open_(path);
    if (fd < 0) {
        return fd;
    }
    char buf[192];
    long n;
    char last = '\n';
    while ((n = read_(fd, buf, sizeof(buf))) > 0) {
        heartbeat();
        out_bytes(buf, n);
        last = buf[n - 1];
    }
    close_(fd);
    if (last != '\n') {
        out("\r\n");
    }
    return 0;
}

static void cmd_ls(const char *path) {
    static char buf[900];
    long n = readdir_(path, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    } else {
        out("qsh: ls: nothing found\r\n");
    }
}

static void cmd_cat(const char *path) {
    long err = print_file(path);
    if (err != 0) {
        char b[LINE_MAX + 48];
        int o = ghost_put(b, 0, "qsh: cat: cannot open '");
        for (int i = 0; path[i] && o < (int)sizeof(b) - 24; i++) {
            b[o++] = path[i];
        }
        o = ghost_put(b, o, "' (err ");
        o = ghost_put_u(b, o, (unsigned)(-err));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
    }
}

static void cmd_ps(void) {
    static char buf[900];
    long n = sysinfo(SYSINFO_PS, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    }
}

static void cmd_free(void) {
    char buf[128];
    long n = sysinfo(SYSINFO_MEM, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    }
}

static void cmd_date(void) {
    char buf[64];
    long n = sysinfo(SYSINFO_TIME, buf, sizeof(buf));
    if (n > 0) {
        out_bytes(buf, n);
    } else {
        out("qsh: date: unavailable\r\n");
    }
}

static void cmd_uptime(void) {
    char b[64];
    unsigned long t = (unsigned long)ticks();
    int o = ghost_put(b, 0, "qsh: uptime ");
    o = ghost_put_u(b, o, (unsigned)t);
    o = ghost_put(b, o, " ticks (");
    o = ghost_put_u(b, o, (unsigned)(t / 100));
    o = ghost_put(b, o, " s)\r\n");
    out_bytes(b, o);
}

static void cmd_pid(void) {
    char b[32];
    int o = ghost_put(b, 0, "qsh: pid ");
    o = ghost_put_u(b, o, (unsigned)getpid());
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

static void cmd_qrand(void) {
    unsigned char rb[8];
    long r = qrand_fill(rb, sizeof(rb));
    if (r < 0) {
        out("qsh: qrand denied (EPERM)\r\n");
        return;
    }
    char b[48];
    int o = ghost_put(b, 0, "qsh: qrand ");
    for (int i = 0; i < (int)r; i++) {
        int hi = rb[i] >> 4, lo = rb[i] & 0xF;
        b[o++] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
        b[o++] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
    }
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

static void cmd_qseed(void) {
    long s = qseed_value();
    if (s == -4) {
        out("qsh: qseed denied (EPERM)\r\n");
        return;
    }
    char b[40];
    int o = ghost_put(b, 0, "qsh: qseed ");
    o = put_hex64(b, o, (unsigned long long)s);
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

/* Query ghostd's field over capability-checked IPC: send a STATUS
 * request, poll the mailbox (yielding) for the reply, report R and the
 * live pattern count. ghostd replies to the sender via SYS_SEND_TO. */
static void cmd_ghost(void) {
    ghost_req_t req;
    req.op = GHOST_STATUS;
    req.slot = 0;
    req.pad[0] = req.pad[1] = 0;
    for (int w = 0; w < GHOST_PW; w++) {
        req.bits[w] = 0;
    }

    long r = send_msg((const char *)&req, sizeof(req));
    if (r < 0) {
        out("qsh: ghost send denied (EPERM)\r\n");
        return;
    }

    /* Poll for the reply, heartbeating: ghostd may be compute-bound in a
     * field relaxation for a while, and a silent wait would otherwise get
     * this shell watchdog-killed mid-command (found live: the phase-2
     * smoke run landed this query in ghostd's imprint window). */
    ghost_rep_t rep;
    long sender = 0;
    for (int tries = 0; tries < 5000 && sender == 0; tries++) {
        heartbeat();
        sender = recv_msg((char *)&rep, sizeof(rep));
        if (sender == 0) {
            yield();
        }
    }
    if (sender == 0) {
        out("qsh: ghost timeout (no reply)\r\n");
        return;
    }

    char b[64];
    int o = ghost_put(b, 0, "qsh: ghost R=");
    o = ghost_put_r(b, o, rep.r_q16);
    o = ghost_put(b, o, " live=");
    o = ghost_put_u(b, o, rep.live);
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);
}

/* Start an initrd program (SYS_SPAWN — the shell's spawn capability at
 * work) and poll its fate with SYS_WAITPID, heartbeating so the watchdog
 * never mistakes the wait for a hang. */
static void cmd_run(const char *path) {
    long pid = spawn_(path);
    if (pid < 0) {
        char b[LINE_MAX + 48];
        int o = ghost_put(b, 0, "qsh: run: cannot start '");
        for (int i = 0; path[i] && o < (int)sizeof(b) - 24; i++) {
            b[o++] = path[i];
        }
        o = ghost_put(b, o, "' (err ");
        o = ghost_put_u(b, o, (unsigned)(-pid));
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
        return;
    }

    char b[80];
    int o = ghost_put(b, 0, "qsh: spawned pid ");
    o = ghost_put_u(b, o, (unsigned)pid);
    o = ghost_put(b, o, "\r\n");
    out_bytes(b, o);

    long r = WAITPID_RUNNING;
    for (int tries = 0; tries < 5000 && r == WAITPID_RUNNING; tries++) {
        heartbeat();
        r = waitpid_(pid);
        if (r == WAITPID_RUNNING) {
            yield();
        }
    }

    o = ghost_put(b, 0, "qsh: pid ");
    o = ghost_put_u(b, o, (unsigned)pid);
    if (r >= 0 && r < 256) {
        o = ghost_put(b, o, " exited (code ");
        o = ghost_put_u(b, o, (unsigned)r);
        o = ghost_put(b, o, ")\r\n");
    } else if (r == WAITPID_RUNNING) {
        o = ghost_put(b, o, " still running (wait timed out)\r\n");
    } else {
        o = ghost_put(b, o, " vanished (err ");
        o = ghost_put_u(b, o, (unsigned)(-r));
        o = ghost_put(b, o, ")\r\n");
    }
    out_bytes(b, o);
}

static void cmd_unknown(const char *line) {
    char b[LINE_MAX + 48];
    int o = ghost_put(b, 0, "qsh: unknown command '");
    for (int i = 0; line[i] && o < (int)sizeof(b) - 24; i++) {
        b[o++] = line[i];
    }
    o = ghost_put(b, o, "' — try 'help'\r\n");
    out_bytes(b, o);
}

static void execute(const char *line) {
    /* strip leading spaces */
    while (*line == ' ') {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    const char *a;
    if (is_cmd(line, "help")) {
        cmd_help();
    } else if ((a = arg_of(line, "echo")) != 0) {
        char b[LINE_MAX + 16];
        int o = ghost_put(b, 0, "qsh: ");
        for (int i = 0; a[i] && o < (int)sizeof(b) - 4; i++) {
            b[o++] = a[i];
        }
        o = ghost_put(b, o, "\r\n");
        out_bytes(b, o);
    } else if (is_cmd(line, "echo")) {
        out("qsh: \r\n");
    } else if (is_cmd(line, "ps")) {
        cmd_ps();
    } else if ((a = arg_of(line, "ls")) != 0) {
        cmd_ls(a);
    } else if (is_cmd(line, "ls")) {
        cmd_ls("/");
    } else if ((a = arg_of(line, "cat")) != 0) {
        cmd_cat(a);
    } else if (is_cmd(line, "cat")) {
        out("qsh: cat: usage: cat <path>\r\n");
    } else if ((a = arg_of(line, "run")) != 0) {
        cmd_run(a);
    } else if (is_cmd(line, "run")) {
        out("qsh: run: usage: run <path>\r\n");
    } else if ((a = arg_of(line, "write")) != 0) {
        cmd_write(a);
    } else if (is_cmd(line, "write")) {
        out("qsh: write: usage: write <path> <text>\r\n");
    } else if ((a = arg_of(line, "rm")) != 0) {
        cmd_rm(a);
    } else if (is_cmd(line, "sync")) {
        cmd_sync();
    } else if ((a = arg_of(line, "imprint")) != 0) {
        cmd_imprint(a);
    } else if (is_cmd(line, "imprint")) {
        out("qsh: imprint: usage: imprint <text>\r\n");
    } else if ((a = arg_of(line, "recall")) != 0) {
        cmd_recall(a);
    } else if (is_cmd(line, "recall")) {
        out("qsh: recall: usage: recall <probe text>\r\n");
    } else if (is_cmd(line, "fieldtest")) {
        cmd_fieldtest();
    } else if ((a = arg_of(line, "nslookup")) != 0) {
        cmd_nslookup(a);
    } else if (is_cmd(line, "nslookup")) {
        out("qsh: nslookup: usage: nslookup <host>\r\n");
    } else if ((a = arg_of(line, "udping")) != 0) {
        cmd_udping(a);
    } else if (is_cmd(line, "udping")) {
        out("qsh: udping: usage: udping <host>\r\n");
    } else if ((a = arg_of(line, "http")) != 0) {
        cmd_http(a);
    } else if (is_cmd(line, "http")) {
        out("qsh: http: usage: http <host> [port]\r\n");
    } else if (is_cmd(line, "free")) {
        cmd_free();
    } else if (is_cmd(line, "uptime")) {
        cmd_uptime();
    } else if (is_cmd(line, "date")) {
        cmd_date();
    } else if (is_cmd(line, "pid")) {
        cmd_pid();
    } else if (is_cmd(line, "qrand")) {
        cmd_qrand();
    } else if (is_cmd(line, "qseed")) {
        cmd_qseed();
    } else if (is_cmd(line, "ghost")) {
        cmd_ghost();
    } else if (is_cmd(line, "clear")) {
        out("\x1b[2J\x1b[H");
    } else if (is_cmd(line, "exit")) {
        out("qsh: exiting — the watchdog will restart me\r\n");
        exit_(0);
    } else {
        cmd_unknown(line);
    }
}

/* ------------------------------------------------------------------ *
 * Main loop: heartbeat, drain input, edit the line, execute.
 * ------------------------------------------------------------------ */

void _start(void) {
    long restarts = svc_restarts();
    if (restarts > 0) {
        char b[48];
        int o = ghost_put(b, 0, "QSH: reborn (restart=");
        o = ghost_put_u(b, o, (unsigned)restarts);
        o = ghost_put(b, o, ")\r\n");
        out_bytes(b, o);
    } else {
        out("QSH: QuantumOS interactive shell ready — type 'help'\r\n");
    }

    /* Greet with the message of the day — read through the VFS, off the
     * embedded initrd. The first file the OS ever serves to a user. */
    {
        long err = print_file("/etc/motd");
        if (err != 0) {
            char b[48];
            int o = ghost_put(b, 0, "QSH: motd open failed (err ");
            o = ghost_put_u(b, o, (unsigned)(-err));
            o = ghost_put(b, o, ")\r\n");
            out_bytes(b, o);
        }
    }

    prompt();

    char line[LINE_MAX];
    int len = 0;
    int esc = 0;      /* 0 = none, 1 = got ESC, 2 = inside a CSI sequence */
    char prev_cr = 0; /* last byte was CR (suppress the LF of a CRLF pair) */

    for (;;) {
        heartbeat();

        unsigned char chunk[32];
        long n = cons_read(chunk, sizeof(chunk));
        if (n < 0) {
            /* No console capability — nothing a shell can do. The plain
             * SYS_WRITE path needs no cap, so the failure is loggable. */
            write_str("QSH: console read denied — shell exiting");
            exit_(1);
        }
        if (n == 0) {
            yield();
            continue;
        }

        for (long i = 0; i < n; i++) {
            char c = (char)chunk[i];

            /* Swallow ANSI escape sequences (arrow keys over serial send
             * ESC [ A etc.) instead of letting the residue pollute the
             * line — mirrors the PS/2 path's unmapped extended keys. */
            if (esc == 1) {
                esc = (c == '[') ? 2 : 0;
                continue;
            }
            if (esc == 2) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
                    esc = 0;
                }
                continue;
            }
            if (c == 27) {
                esc = 1;
                continue;
            }

            /* A CRLF pair is ONE Enter: the CR executed the line, so the
             * trailing LF must not run it again (double prompt). */
            if (c == '\n' && prev_cr) {
                prev_cr = 0;
                continue;
            }
            prev_cr = (c == '\r');

            if (c == '\r' || c == '\n') {
                out("\r\n");
                /* Trim trailing spaces so "ps " matches the builtin. */
                while (len > 0 && line[len - 1] == ' ') {
                    len--;
                }
                line[len] = '\0';
                execute(line);
                len = 0;
                prompt();
            } else if (c == 8 || c == 127) { /* backspace / DEL */
                if (len > 0) {
                    len--;
                    out("\b \b");
                }
            } else if (c >= 32 && c < 127 && len < LINE_MAX - 1) {
                line[len++] = c;
                out_bytes(&c, 1); /* echo */
            }
        }
    }
}
