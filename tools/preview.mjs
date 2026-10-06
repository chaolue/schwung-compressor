#!/usr/bin/env node
/*
 * Render the Compressor's pages the way the Move draws them, with REAL meter
 * readings, using a Schwung checkout's own planner and renderer.
 *
 *   SCHWUNG_DIR=../schwung node tools/preview.mjs --png build/preview
 *   SCHWUNG_DIR=../schwung node tools/preview.mjs --gr "-6.2 dB in -9" --touch 7
 *   SCHWUNG_DIR=../schwung node tools/preview.mjs --set threshold=-30 --set ratio=8
 *
 * Schwung's own tools/param-pages/preview.mjs can render this module too
 * (--fixture / --widgets), but it synthesises a NUMBER for every value, so it
 * cannot show the packed "<reduction> dB in <level>" reading the meter is
 * built on. This drives the same functions with values you choose.
 *
 * Without --png it prints half-block art to the terminal.
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const SCHWUNG = path.resolve(process.env.SCHWUNG_DIR || path.join(HERE, "..", "schwung"));
if (!fs.existsSync(path.join(SCHWUNG, "src", "shared", "param_pages", "page_plan.mjs"))) {
    console.error(`no Schwung checkout at ${SCHWUNG} -- set SCHWUNG_DIR`);
    process.exit(2);
}
const imp = (rel) => import(pathToFileURL(path.join(SCHWUNG, rel)).href);

const { createFramebuffer, drawContext } = await imp("tools/param-pages/harness.mjs");
const { planPages, PAGE_KNOBS } = await imp("src/shared/param_pages/page_plan.mjs");
const { buildMetaIndex } = await imp("src/shared/param_pages/param_meta.mjs");
const { renderPageMovy } = await imp("src/shared/param_pages/render_page_movy.mjs");
const { resolveViz } = await imp("src/shared/param_pages/viz.mjs");
const { registerOverlayWidgets, clearWidgets } = await imp("src/shared/param_pages/widget_registry.mjs");
const { frameCtx } = await imp("src/shared/param_pages/frame_ctx.mjs");

const argv = process.argv.slice(2);
const opt = (name, dflt = null) => {
    const i = argv.indexOf("--" + name);
    return i >= 0 && argv[i + 1] !== undefined ? argv[i + 1] : dflt;
};
const sets = [];
argv.forEach((a, i) => { if (a === "--set" && argv[i + 1]) sets.push(argv[i + 1]); });

const mod = JSON.parse(fs.readFileSync(path.join(HERE, "src", "module.json"), "utf8"));
const caps = mod.capabilities;

/* The module's widgets, loaded as a script exactly as the device loads them. */
const g = {};
new Function("globalThis", fs.readFileSync(path.join(HERE, "src", "canvas.js"), "utf8"))(g);
clearWidgets();
registerOverlayWidgets(g.canvas_overlay);

/* Values: each param's declared default, then the meter, then --set. */
const values = {};
for (const p of caps.chain_params) {
    if (p.default !== undefined) values[p.key] = String(p.default);
}
values.gr = opt("gr", "-4.5 dB in -12");
for (const s of sets) {
    const eq = s.indexOf("=");
    if (eq > 0) values[s.slice(0, eq)] = s.slice(eq + 1);
}

const { pages } = planPages({ hierarchy: caps.ui_hierarchy, chainParams: caps.chain_params });
const metaIndex = buildMetaIndex({ hierarchy: caps.ui_hierarchy, chainParams: caps.chain_params });
const touched = opt("touch") !== null ? parseInt(opt("touch"), 10) : -1;
const pngDir = opt("png");
const scale = parseInt(opt("scale", "4"), 10);

function drawCanvasPage(ctx, band, canvas, extra) {
    g.canvas_overlay.drawPage(frameCtx(ctx, band), {
        key: canvas.key, values: (extra && extra.values) || {}, nowMs: 0,
    });
}

console.log(`${mod.id}: ${pages.length} pages -- ${pages.map((p) => p.name).join(" | ")}`);
pages.forEach((p, i) => {
    if (p.kind !== PAGE_KNOBS) return;
    const fb = createFramebuffer();
    const { groups } = resolveViz({ keys: p.keys || [], metaIndex });
    renderPageMovy(drawContext(fb), {
        drawCanvasPage, page: p, metaIndex, values,
        title: `T1 > ${mod.name.toUpperCase()}`,
        pageIndex: i, pageCount: pages.length, touched, viz: groups,
        footer: touched >= 0 ? [["MUTE", "DFLT"], ["SHFT", "FINE"]]
                             : [["JOG", "PG"], ["SHFT", "SECT"], ["CLK", "MENU"]],
    });
    if (pngDir) {
        fs.mkdirSync(pngDir, { recursive: true });
        const file = path.join(pngDir, `${mod.id}-${String(i).padStart(2, "0")}-${p.name.toLowerCase()}.png`);
        fs.writeFileSync(file, fb.toPng(scale));
        console.log("  " + file);
    } else {
        console.log(`\n-- page ${i}: ${p.name}`);
        console.log(fb.toBlocks());
    }
});
