/**
 * QuantumOS httpd — the status page served FROM the OS (epic #98).
 *
 * A ring-3 service that arms the kernel's single passive-open TCP
 * listener on :8080 and serves one HTTP/1.0 request at a time:
 * listen -> accept -> read the request under a TOTAL deadline -> one
 * response segment -> close -> re-listen. Every value in the body is
 * computed AT REQUEST TIME (uptime from SYS_TICKS, memory from
 * SYS_SYSINFO, a rising served counter), so a fetched page proves the
 * OS answered live — the CI gate asserts the counter rises across two
 * fetches, which no static page can satisfy.
 *
 * Capabilities: grant_net only. That cap is honestly COARSER than the
 * job needs — it also gates SYS_UDP / SYS_RESOLVE / outbound
 * TCP_CONNECT — so this program deliberately contains no outbound
 * operation of any kind, and the request bytes are discarded, never
 * parsed into actions. No NIC -> the listen fails hard and httpd idles
 * silently; the default NIC-less boot is unchanged.
 *
 * DoS bounds: the request read runs under a total deadline captured
 * ONCE (never reset on progress — a byte-dribbling slow-loris is cut
 * off, not refreshed), oversized requests are dropped at 1 KiB, and a
 * peer that wedges the connection is aborted by the close path's own
 * deadline. The kernel bounds the rest: at most one half-open
 * (SYN_RCVD reaps back to LISTEN on retransmit exhaustion).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ghost.h" /* usys.h + the ghost_put string builders */

#define HTTPD_PORT 8080
#define HTTPD_REQ_CAP 1024
#define HTTPD_REQ_DEADLINE 200   /* ticks (~2s): curl sends the GET in one segment */
#define HTTPD_SEND_DEADLINE 500  /* ticks: response staging bound */
#define HTTPD_CLOSE_DEADLINE 800 /* ticks: teardown incl. the retransmit path */

static unsigned served; /* process-local: a watchdog rebirth resets it */

static void req_clear(tcp_req_t *req) {
    for (unsigned i = 0; i < sizeof(*req); i++) {
        ((unsigned char *)req)[i] = 0;
    }
}

/* Poll TCP_CLOSE until the connection retires (bounded, heartbeating).
 * Works from every state: ERROR/LISTEN/SYN_RCVD abort immediately,
 * ESTABLISHED/CLOSE_WAIT drain the response then FIN. */
static void http_close(void) {
    long t0 = ticks();
    for (;;) {
        heartbeat();
        tcp_req_t req;
        req_clear(&req);
        long r = tcp_(TCP_CLOSE, &req);
        if (r != UDP_WOULDBLOCK) {
            break;
        }
        if (ticks() - t0 > HTTPD_CLOSE_DEADLINE) {
            break; /* the kernel's own retransmit timeout finishes the job */
        }
        yield();
    }
}

/* Build the full HTTP/1.0 response (headers + live body). Returns its
 * length. Content-Length lets the client detect EOF without waiting on
 * our FIN. All fixed-point/integer — no floats in ring 3. */
static int build_response(char *out, int cap) {
    static char body[512]; /* single-threaded service */
    int b = 0;
    b = ghost_put(body, b, "QuantumOS status\n");
    b = ghost_put(body, b, "uptime=");
    b = ghost_put_u(body, b, (unsigned)(ticks() / 100));
    b = ghost_put(body, b, "s\nserved=");
    b = ghost_put_u(body, b, served);
    b = ghost_put(body, b, "\n");
    /* Live memory line, formatted by the kernel (uncapped introspection,
     * same authority-free class as SYS_TICKS). Ends with \r\n already. */
    long m = sysinfo(SYSINFO_MEM, body + b, (long)sizeof(body) - b - 1);
    if (m > 0) {
        b += (int)m;
    }

    int o = 0;
    o = ghost_put(out, o, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ");
    o = ghost_put_u(out, o, (unsigned)b);
    o = ghost_put(out, o, "\r\nConnection: close\r\n\r\n");
    for (int i = 0; i < b && o < cap; i++) {
        out[o++] = body[i];
    }
    return o;
}

/* Is the header terminator (\r\n\r\n, or bare \n\n) in buf[0..len)? */
static int headers_complete(const char *buf, int len) {
    for (int i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            return 1;
        }
        if (i + 3 < len && buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
            buf[i + 3] == '\n') {
            return 1;
        }
    }
    return 0;
}

void _start(void) {
    for (;;) {
        heartbeat();

        /* Arm the listener: LISTEN is re-postable, poll until armed. A
         * lost arming store (reset race) simply re-posts next round. */
        long r;
        for (;;) {
            heartbeat();
            tcp_req_t req;
            req_clear(&req);
            req.port = HTTPD_PORT;
            r = tcp_(TCP_LISTEN, &req);
            if (r != UDP_WOULDBLOCK) {
                break;
            }
            yield();
        }
        if (r != 0) {
            /* Hard failure — no NIC (the default boot) or no capability.
             * Degrade honestly: say so once, then idle under heartbeat. */
            write_str("httpd: no network — idle");
            for (;;) {
                heartbeat();
                yield();
            }
        }

        /* Wait for a peer. EIO = the conn needs retiring: close, re-arm. */
        for (;;) {
            heartbeat();
            tcp_req_t req;
            req_clear(&req);
            r = tcp_(TCP_ACCEPT, &req);
            if (r != UDP_WOULDBLOCK) {
                break;
            }
            yield();
        }
        if (r != 0) {
            http_close();
            continue;
        }

        /* Read the request under a TOTAL deadline captured once — never
         * reset on progress (the slow-loris lesson: an idle-reset timer
         * is refreshed forever by one byte every few seconds). */
        static char reqbuf[HTTPD_REQ_CAP];
        int got = 0;
        int complete = 0;
        long t0 = ticks();
        while (!complete) {
            heartbeat();
            if (ticks() - t0 > HTTPD_REQ_DEADLINE) {
                break; /* too slow — drop, don't serve */
            }
            if (got >= HTTPD_REQ_CAP) {
                break; /* oversized — drop */
            }
            tcp_req_t req;
            req_clear(&req);
            req.buf = reqbuf + got;
            req.len = (unsigned short)(HTTPD_REQ_CAP - got);
            long n = tcp_(TCP_RECV, &req);
            if (n == 0) {
                complete = 1; /* EOF: the peer half-closed after its GET */
            } else if (n > 0) {
                got += (int)n;
                if (headers_complete(reqbuf, got)) {
                    complete = 1;
                }
            } else if (n == UDP_WOULDBLOCK) {
                yield();
            } else {
                break; /* reset/aborted */
            }
        }
        if (!complete) {
            http_close();
            continue;
        }

        /* One response segment (<= MSS). The request bytes are discarded
         * — every path is a 200 status page; there is nothing to parse
         * into an action. */
        served++;
        static char resp[768];
        int rlen = build_response(resp, (int)sizeof(resp));
        t0 = ticks();
        long sr;
        for (;;) {
            heartbeat();
            tcp_req_t req;
            req_clear(&req);
            req.buf = resp;
            req.len = (unsigned short)rlen;
            sr = tcp_(TCP_SEND, &req);
            if (sr != UDP_WOULDBLOCK) {
                break;
            }
            if (ticks() - t0 > HTTPD_SEND_DEADLINE) {
                break;
            }
            yield();
        }

        http_close(); /* the close path waits for the staged data to drain */
    }
}
