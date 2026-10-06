#!/usr/bin/env node
/*
 * Offline tests for src/canvas.js: the gain-reduction cell widget and the
 * Curve page.
 *
 *   node tests/test_canvas.mjs [build/curve.json]
 *
 * canvas.js is loaded the way the device loads it -- as a SCRIPT that assigns
 * globalThis.canvas_overlay, not as a module. The drawing context is a
 * stand-in for Schwung's frame context: the same primitives, clipped to the
 * frame, COUNTING anything drawn outside it. On the device such pixels are
 * silently dropped, so a drawer that overflows looks fine in one size and
 * loses its edge in another -- the count is how that shows up here.
 *
 * With build/curve.json (written by `test_compressor --dump-curve`) the JS
 * copy of the gain computer is checked against the C one, because the Curve
 * page draws the JS copy and the audio is shaped by the C one.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const src = fs.readFileSync(path.join(HERE, "..", "src", "canvas.js"), "utf8");
const g = {};
new Function("globalThis", src)(g);
const ov = g.canvas_overlay;

let pass = 0, fail = 0;
function check(cond, msg) {
    if (cond) pass++;
    else { fail++; console.log("  FAIL " + msg); }
}

/* Proportional 5x7 font, like the device's: most glyphs 5 wide, a few narrow. */
const NARROW = new Set([".", ":", "!", "|", "'", "i", "l", "1", " "]);
function textWidth(s) {
    let w = 0;
    for (const ch of String(s)) w += (NARROW.has(ch) ? 3 : 5) + 1;
    return Math.max(0, w - 1);
}

function makeCtx(w, h) {
    const px = new Uint8Array(w * h);
    let clipped = 0, ops = 0;
    const ctx = {
        width: w, height: h,
        fillRect(x, y, rw, rh, color) {
            ops++;
            x = Math.round(x); y = Math.round(y); rw = Math.round(rw); rh = Math.round(rh);
            if (!(rw > 0 && rh > 0)) return;
            if (x < 0 || y < 0 || x + rw > w || y + rh > h) clipped++;
            for (let yy = Math.max(0, y); yy < Math.min(h, y + rh); yy++)
                for (let xx = Math.max(0, x); xx < Math.min(w, x + rw); xx++)
                    px[yy * w + xx] = color ? 1 : 0;
        },
        print(x, y, text, color) {
            ops++;
            if (x < 0 || y < 0 || x + textWidth(text) > w || y + 7 > h) clipped++;
        },
        textWidth,
        setPixel(x, y, c) { ctx.fillRect(x, y, 1, 1, c); },
        line(x0, y0, x1, y1, c) {
            x0 = Math.round(x0); y0 = Math.round(y0); x1 = Math.round(x1); y1 = Math.round(y1);
            const dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
            const dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
            let err = dx + dy;
            for (;;) {
                ctx.fillRect(x0, y0, 1, 1, c);
                if (x0 === x1 && y0 === y1) break;
                const e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
        },
        fillCircle(cx, cy, r, c) {
            for (let dy = -r; dy <= r; dy++) {
                const half = Math.floor(Math.sqrt(r * r - dy * dy));
                ctx.fillRect(cx - half, cy + dy, half * 2 + 1, 1, c);
            }
        },
        drawCircle() { ops++; },
        drawArc() { ops++; },
    };
    return { ctx, clipped: () => clipped, ops: () => ops, lit: () => px.reduce((a, b) => a + b, 0) };
}

/* ---------------------------------------------------------------- contract */

check(ov && ov.widgetKind === "custom:grmeter", "registers custom:grmeter");
check(typeof ov.drawCell === "function" && typeof ov.drawPage === "function", "exports drawCell and drawPage");
const mod = JSON.parse(fs.readFileSync(path.join(HERE, "..", "src", "module.json"), "utf8"));
const declared = mod.capabilities.chain_params.filter((p) => p.viz && p.viz.kind).map((p) => p.viz.kind);
check(declared.includes(ov.widgetKind), "the kind canvas.js registers is the one module.json declares");

/* ------------------------------------------------------------------ parser */

const { parseMeter, reduction, scale } = ov._test;
const pm = (s) => JSON.stringify(parseMeter(s));
const M = (gr, inDb, bypass) => JSON.stringify({ gr, inDb, bypass });
check(pm("-4.5 dB in -12") === M(4.5, -12, false), "parses a full reading");
check(pm("4.5 dB in -12") === M(4.5, -12, false), "the sign of the gain change is not load-bearing");
check(pm("0.0 dB in --") === M(0, null, false), "parses silence");
check(pm("-4.5 dB in -12 byp") === M(4.5, -12, true), "parses the bypass mark");
check(pm("0.0 dB in -- byp") === M(0, null, true), "parses the bypass mark on silence");
check(pm("7") === M(7, null, false), "a bare number is a reduction");
check(parseMeter(null) === null && parseMeter(undefined) === null, "no answer is null, not zero");
check(parseMeter("") === null && parseMeter("garbage") === null, "an unreadable value is null");
check(scale(0) === 0 && scale(24) === 1 && scale(100) === 1 && scale(-3) === 0, "meter scale bounds");
check(scale(3) > 0.4 && scale(6) > 0.55, "small reductions get most of the travel");

/* ------------------------------------------- JS curve == C curve (if given) */

const curveFile = process.argv[2];
if (curveFile && fs.existsSync(curveFile)) {
    const rows = JSON.parse(fs.readFileSync(curveFile, "utf8"));
    let worst = 0;
    for (const r of rows) worst = Math.max(worst, Math.abs(reduction(r.x, r.t, r.r, r.k) - r.g));
    check(rows.length > 50 && worst < 1e-3, `JS curve matches the DSP's (worst diff ${worst.toExponential(2)} dB)`);
} else {
    console.log("  (no curve table given; skipping the C cross-check)");
}

/* ---------------------------------------------------------- drawCell sweep */

const READINGS = [null, undefined, "", "garbage", "0.0 dB in --", "0.0 dB in -45",
                  "-4.5 dB in -12", "-12.0 dB in -3", "-30.0 dB in 0", "7", "-0.1 dB in -60",
                  "-6.0 dB in -9 byp", "0.0 dB in -- byp", "-30.0 dB in 0 byp"];
let cellOverflow = 0, cellThrew = 0, cells = 0;
for (let w = 14; w <= 64; w += 1) {
    for (let h = 4; h <= 30; h += 1) {
        for (const v of READINGS) {
            const t = makeCtx(w, h);
            try {
                ov.drawCell(t.ctx, { values: { gr: v }, group: { keys: ["gr"] }, nowMs: 0 });
            } catch (e) { cellThrew++; if (cellThrew < 3) console.log("    threw:", w, h, v, e.message); }
            if (t.clipped()) { cellOverflow++; if (cellOverflow < 4) console.log("    overflow:", w, h, v); }
            cells++;
        }
    }
}
check(cellThrew === 0, `drawCell never throws (${cellThrew} of ${cells})`);
check(cellOverflow === 0, `drawCell stays inside its frame (${cellOverflow} of ${cells} overflowed)`);

/* A missing group or values object is not an error either. */
let threw = false;
try { const t = makeCtx(32, 20); ov.drawCell(t.ctx, {}); ov.drawCell(t.ctx, undefined); } catch (e) { threw = true; }
check(!threw, "drawCell tolerates a missing payload");

/* More reduction lights more of the bar. */
function litFor(v) { const t = makeCtx(32, 20); ov.drawCell(t.ctx, { values: { gr: v }, group: { keys: ["gr"] } }); return t.lit(); }
check(litFor("-12.0 dB in -6 byp") > 0 && litFor("-12.0 dB in -6 byp") < litFor("-12.0 dB in -6"),
      "bypassed, the bar is a lighter ghost of the same reading");
check(litFor("-12.0 dB in -6") > litFor("-3.0 dB in -6") && litFor("-3.0 dB in -6") > litFor("0.0 dB in -6"),
      "the bar grows with the reduction");

/* ---------------------------------------------------------- drawPage sweep */

const PAGES = [
    {},
    { gr: null },
    { gr: "0.0 dB in --", threshold: "-18.000", ratio: "3.000", knee: "6.000" },
    { gr: "-6.2 dB in -9", threshold: "-24.000", ratio: "4.000", knee: "0.000" },
    { gr: "-24.0 dB in 0", threshold: "-60.000", ratio: "20.000", knee: "24.000" },
    { gr: "-1.0 dB in -70", threshold: "0.000", ratio: "1.000", knee: "0.000" },
    { gr: "-6.2 dB in -9 byp", threshold: "-24.000", ratio: "4.000", knee: "6.000" },
    { gr: "-3.0 dB in -12", threshold: "abc", ratio: "", knee: null },
];
let pageOverflow = 0, pageThrew = 0, pagesDrawn = 0;
for (let w = 96; w <= 128; w += 4) {
    for (let h = 20; h <= 52; h += 2) {
        for (const values of PAGES) {
            const t = makeCtx(w, h);
            try { ov.drawPage(t.ctx, { values, nowMs: 0 }); }
            catch (e) { pageThrew++; if (pageThrew < 3) console.log("    threw:", w, h, JSON.stringify(values), e.message); }
            if (t.clipped()) { pageOverflow++; if (pageOverflow < 4) console.log("    overflow:", w, h, JSON.stringify(values)); }
            pagesDrawn++;
        }
    }
}
check(pageThrew === 0, `drawPage never throws (${pageThrew} of ${pagesDrawn})`);
check(pageOverflow === 0, `drawPage stays inside its band (${pageOverflow} of ${pagesDrawn} overflowed)`);
threw = false;
try { const t = makeCtx(10, 10); ov.drawPage(t.ctx, {}); ov.drawPage(makeCtx(128, 44).ctx, undefined); } catch (e) { threw = true; }
check(!threw, "drawPage tolerates a tiny band and a missing payload");

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
