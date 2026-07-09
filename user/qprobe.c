/**
 * QuantumOS /bin/qprobe — the spawn target for the quota-test proof (epic
 * #135). A deliberately trivial citizen: it prints one distinctive marker
 * and exits with a code (7) that collides with NO existing gate string or
 * exit code (/bin/hello uses 42 + "HELLO:", /bin/args uses argc + "ARGS:").
 * quota-test spawns it TWICE; the second spawn must be refused by the
 * manifest spawn quota, so this program must be cheap and side-effect-free.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "libq/libq.h"

void _start(void) {
    write_str("QPROBE: alive — spawned under quota");
    exit_(7); /* a code no other gate asserts on */
    for (;;) {
    }
}
