// Browser-demo boot gate (epic #100): load the real demo page in headless
// Chromium, prove QuantumOS boots to the qsh prompt inside qemu-wasm, a
// citizen runs, and the terminal is interactive (real keystrokes reach the
// guest). Fails loudly with the serial tail so a regression is diagnosable.
//
// Run: node ci-boot-gate.mjs  (with serve.cjs serving ./ on $PORT/8905, COOP/COEP).
import { chromium } from "playwright";

const PORT = process.env.PORT || "8905";
const URL = `http://localhost:${PORT}/index.html`;
const BANNER = "QSH: QuantumOS interactive shell ready";

function fail(msg, extra) {
  console.error("FAIL: " + msg);
  if (extra) console.error(extra);
  process.exit(1);
}

const browser = await chromium.launch({ args: ["--no-sandbox", "--disable-dev-shm-usage"] });
const page = await browser.newPage();
page.on("pageerror", (e) => console.log("[pageerror]", e.message));
page.on("console", (m) => { if (m.type() === "error") console.log("[page-error]", m.text()); });
await page.goto(URL, { waitUntil: "domcontentloaded" });

// The page accumulates the FULL serial stream into window.__serial (xterm's
// own buffer is only the visible viewport, so it scrolls the banner away).
// coi-serviceworker reloads the page once on first load, which destroys the
// execution context mid-evaluate — treat that as "nothing yet", not a crash.
const evalSafe = async (fn, dflt) => { try { return await page.evaluate(fn); } catch { return dflt; } };
const serial = () => evalSafe(() => window.__serial || "", "");
const booted = () => evalSafe(() => !!window.__booted, false);
const t0 = Date.now();

// 1. Boots to the qsh banner (the page flips window.__booted on detection).
//    On a headers-less host this waits through the coi-serviceworker reload.
let didBoot = false;
for (let i = 0; i < 90; i++) {
  if (await booted()) { didBoot = true; break; }
  await new Promise((r) => setTimeout(r, 1000));
}
if (!didBoot) fail("never reached the qsh banner", (await serial()).slice(-1500));
console.log(`OK: booted to qsh in ${((Date.now() - t0) / 1000).toFixed(1)}s`);

// 2. The loader was replaced by the live prompt.
if (!(await evalSafe(() => document.getElementById("loader")?.hasAttribute("hidden"), false))) {
  fail("boot banner seen but the loading overlay never cleared");
}
if (!(await serial()).includes(BANNER)) fail("banner not present in the serial stream");

// Let the boot fully settle before typing (a `quiet` boot still finishes its
// one-time startup lines over the first several seconds).
await new Promise((r) => setTimeout(r, 12000));
await page.click("#screen");

// Helper: type a command and wait for a marker in the bytes emitted AFTER it.
async function typeExpect(cmd, marker, label, budget = 25) {
  const before = (await serial()).length;
  await page.keyboard.type(cmd + "\n");
  for (let i = 0; i < budget; i++) {
    if ((await serial()).slice(before).includes(marker)) return;
    await new Promise((r) => setTimeout(r, 1000));
  }
  fail(`typed '${cmd}' but '${marker}' never appeared`, (await serial()).slice(-1500));
}

// 3. Interactivity: a command produces its own output (proves keystrokes reach
//    the guest and the shell answers). Use `uptime` — a pure qsh builtin over
//    SYS_TICKS with a single deterministic reply (`qsh: uptime <n> ticks ...`).
//    NOT `ghost`: that send is capability-routed to ghostd, so it is sensitive to
//    the demo's IPC-cap lifecycle and to first-match cap ordering, which raced the
//    fixed post-boot settle under qemu-wasm's variable timing and intermittently
//    returned "ghost send denied (EPERM)" — reddening this gate on a healthy boot.
//    The qsh<->ghostd IPC path is covered deterministically by `make ci-smoke`
//    (serial), so proving interactivity here with a cap-free command loses no net
//    coverage while removing the flake.
await typeExpect("uptime", "qsh: uptime ", "uptime");
console.log("OK: typed 'uptime' — the shell answered");

// 4. A ring-3 citizen runs on demand with its full output (survives `quiet`).
await typeExpect("run /bin/consciousnessd", "CONSCIOUSNESS EMERGED", "consciousnessd", 30);
console.log("OK: ran /bin/consciousnessd — a citizen produced rich output");

// 5. The console is quiet at rest: with no input, almost nothing should print,
//    or periodic chatter would scroll over what a visitor types (the reported
//    bug). Allow a tiny margin for a stray line.
const q0 = (await serial()).length;
await new Promise((r) => setTimeout(r, 6000));
const grew = (await serial()).length - q0;
if (grew > 200) fail(`console is noisy at rest (grew ${grew}B in 6s idle) — input would be disrupted`);
console.log(`OK: console quiet at rest (grew ${grew}B in 6s — typing stays legible)`);

console.log("=== Browser Demo boot gate PASSED ===");
await browser.close();
process.exit(0);
