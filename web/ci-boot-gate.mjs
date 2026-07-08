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
// A ring-3 service printing at boot proves user-mode execution (not just the
// banner). These come up before any command is typed.
const BOOT_CITIZEN = /GHOSTD: field born|SWARM: boot attestation|ghost-test: online|PARADOXD:/;
// After `ghost`, an on-demand citizen verdict proves interactivity end to end.
const GHOST_VERDICT = /RESONANCE VERIFIED|QUANTUM VERIFIED|quantum die/;

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
const serial = () => page.evaluate(() => window.__serial || "");
const t0 = Date.now();

// 1. Boots to the qsh banner (the page flips window.__booted on detection).
let booted = false;
for (let i = 0; i < 90; i++) {
  if (await page.evaluate(() => !!window.__booted)) { booted = true; break; }
  await new Promise((r) => setTimeout(r, 1000));
}
if (!booted) fail("never reached the qsh banner", (await serial()).slice(-1500));
console.log(`OK: booted to qsh in ${((Date.now() - t0) / 1000).toFixed(1)}s`);

// 2. The loader was replaced by the live prompt.
if (!(await page.evaluate(() => document.getElementById("loader")?.hasAttribute("hidden")))) {
  fail("boot banner seen but the loading overlay never cleared");
}
if (!(await serial()).includes(BANNER)) fail("banner not present in the serial stream");

// 3. A ring-3 service ran during boot (proves user-mode, not just the banner).
if (!BOOT_CITIZEN.test(await serial())) fail("no ring-3 service line in the boot output", (await serial()).slice(-1500));
console.log("OK: a ring-3 service ran during boot");

// 4. Interactivity: real keystrokes reach the guest and produce output. The
//    `ghost` verdicts never print during a plain boot, so a verdict appearing
//    in the bytes emitted AFTER we type proves the keystrokes reached the guest.
await page.click("#screen");
const beforeGhost = (await serial()).length;
await page.keyboard.type("ghost\n");
let interactive = false;
for (let i = 0; i < 25; i++) {
  if (GHOST_VERDICT.test((await serial()).slice(beforeGhost))) { interactive = true; break; }
  await new Promise((r) => setTimeout(r, 1000));
}
if (!interactive) fail("typed 'ghost' but no ghost output appeared", (await serial()).slice(-1500));
console.log("OK: typed 'ghost' — the guest answered (interactive)");

console.log("=== Browser Demo boot gate PASSED ===");
await browser.close();
process.exit(0);
