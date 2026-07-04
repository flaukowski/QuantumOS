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
    out("qsh: commands: help echo ps free uptime pid ls cat run qrand qseed ghost clear "
        "exit\r\n");
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
    } else if (is_cmd(line, "free")) {
        cmd_free();
    } else if (is_cmd(line, "uptime")) {
        cmd_uptime();
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
