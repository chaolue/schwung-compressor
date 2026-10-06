/*
 * Compressor -- the gain-reduction meter (a knob-grid cell) and the Curve page.
 *
 * Both draw from ONE value, `gr`, which the DSP serves as
 *
 *     "<gain change> dB in <detector level>"    e.g. "-4.5 dB in -12"
 *                                                    "0.0 dB in --"   (silence)
 *
 * The gain change is negative (it is a reduction); the sign is ignored when
 * parsing, so a reading of "4.5" means the same 4.5 dB of reduction.
 *
 * It is one key on purpose: the knob grid refreshes `live` params one read
 * per tick, shared between them, so a second meter key would halve the rate
 * of both. The DSP applies the meter ballistics (instant rise, steady fall),
 * so nothing here keeps state between frames.
 *
 * The rules this file lives by (Schwung's custom-widget contract):
 *   - (0,0) is the top-left of the frame we are handed and ctx.width/height
 *     are its size; everything is sized from those, never from constants.
 *   - We are given values and cannot read any. A value that did not arrive is
 *     null/undefined, and draws as "--" rather than as a zero.
 *   - If a drawer throws it is disabled for the session and the host draws a
 *     plain cell instead, so the page stays usable.
 */

(function () {
    "use strict";

    var GLYPH_H = 7;            /* the device font's cap height */
    var SCALE_MAX_DB = 24;      /* matches the `gr` param's max */
    var PLOT_RANGE_DB = 60;     /* the Curve page shows -60..0 dBFS */
    var TICKS_DB = [3, 6, 12];

    /* "-4.5 dB in -12" -> { gr: 4.5, inDb: -12 }: `gr` is the reduction, a
     * positive number of dB. A bare number is accepted as a reduction with no
     * level. */
    function parseMeter(raw) {
        if (raw === null || raw === undefined) return null;
        var s = String(raw);
        var m = /^\s*(-?\d+(?:\.\d+)?)\s*dB\s+in\s+(--|-?\d+(?:\.\d+)?)/.exec(s);
        if (m) {
            return {
                gr: Math.abs(Number(m[1])),
                inDb: m[2] === "--" ? null : Number(m[2]),
            };
        }
        var n = Number(s);
        if (s.trim() !== "" && isFinite(n)) return { gr: Math.abs(n), inDb: null };
        return null;
    }

    function num(v, fallback) {
        if (v === null || v === undefined || v === "") return fallback;
        var n = Number(v);
        return isFinite(n) ? n : fallback;
    }

    /* Meter scale: logarithmic in (1 + dB), so the 1-6 dB a vocal mostly
     * lives in takes up most of the travel instead of a quarter of it. */
    function scale(db) {
        if (!(db > 0)) return 0;
        var f = Math.log(1 + db) / Math.log(1 + SCALE_MAX_DB);
        return f > 1 ? 1 : f;
    }

    /* The DSP's static curve, mirrored: dB of reduction for a steady level. */
    function reduction(xDb, thr, ratio, knee) {
        var slope = 1 / Math.max(1, ratio) - 1;
        var over = xDb - thr;
        if (knee > 0.01) {
            if (2 * over < -knee) return 0;
            if (2 * over <= knee) {
                var t = over + 0.5 * knee;
                return -slope * t * t / (2 * knee);
            }
            return -slope * over;
        }
        return over > 0 ? -slope * over : 0;
    }

    function grText(m) {
        if (!m) return "--";
        return m.gr < 0.05 ? "0.0" : "-" + m.gr.toFixed(1);
    }

    function outline(ctx, x, y, w, h) {
        ctx.fillRect(x, y, w, 1, 1);
        ctx.fillRect(x, y + h - 1, w, 1, 1);
        ctx.fillRect(x, y, 1, h, 1);
        ctx.fillRect(x + w - 1, y, 1, h, 1);
    }

    /*
     * A horizontal reduction meter: 0 dB at the RIGHT, reduction pushing in
     * towards the left, the way a hardware compressor's needle or LED bar
     * reads. Ticks at 3, 6 and 12 dB. The number sits above it when the cell
     * is tall enough to hold both.
     */
    function drawCell(ctx, o) {
        var group = o && o.group;
        var values = o && o.values;
        var key = group && group.keys && group.keys[0];
        var m = parseMeter(key && values ? values[key] : null);

        var w = ctx.width, h = ctx.height;
        if (w < 6 || h < 4) return;

        var showText = h >= GLYPH_H + 6;
        var barTop = showText ? GLYPH_H + 2 : 0;
        var barH = h - barTop;

        if (showText) {
            var s = grText(m);
            var tw = ctx.textWidth(s);
            if (tw <= w) ctx.print(w - tw, 0, s, 1);
        }

        outline(ctx, 0, barTop, w, barH);
        var inner = w - 2;
        if (m && barH > 2) {
            var fill = Math.round(inner * scale(m.gr));
            if (fill > 0) ctx.fillRect(1 + inner - fill, barTop + 1, fill, barH - 2, 1);
        }
        if (barH > 3) {
            for (var i = 0; i < TICKS_DB.length; i++) {
                var tx = 1 + inner - Math.round(inner * scale(TICKS_DB[i]));
                var lit = m && Math.round(inner * scale(m.gr)) >= (1 + inner - tx);
                /* A tick is a notch in the bar's top inner row, drawn in the
                 * opposite colour to whatever it sits on, so it stays visible
                 * whether or not the bar has reached it. */
                ctx.fillRect(tx, barTop + 1, 1, 1, lit ? 0 : 1);
            }
        }
    }

    /*
     * The Curve page: the static transfer curve with the signal's current
     * position on it, a vertical reduction meter, and the numbers that matter
     * while setting it up. The eight knobs work here exactly as on the main
     * page, so turning Threshold / Ratio / Knee moves the curve under you.
     */
    function drawPage(ctx, o) {
        var values = (o && o.values) || {};
        var w = ctx.width, h = ctx.height;
        if (w < 40 || h < 20) return;

        var m = parseMeter(values.gr);
        var thr = num(values.threshold, -18);
        var ratio = num(values.ratio, 3);
        var knee = num(values.knee, 6);

        /* ---- transfer plot, a square on the left ---- */
        var S = Math.min(h, Math.floor(w * 0.42));
        var py0 = Math.floor((h - S) / 2);
        var span = S - 3;                       /* inside the frame */
        outline(ctx, 0, py0, S, S);

        function px(db) { return 1 + Math.round((db + PLOT_RANGE_DB) / PLOT_RANGE_DB * span); }
        function py(db) { return py0 + S - 2 - Math.round((db + PLOT_RANGE_DB) / PLOT_RANGE_DB * span); }
        function clampDb(db) { return db < -PLOT_RANGE_DB ? -PLOT_RANGE_DB : (db > 0 ? 0 : db); }

        /* unity line, dotted */
        for (var d = 0; d <= span; d += 3) ctx.fillRect(1 + d, py0 + S - 2 - d, 1, 1, 1);

        /* threshold, a dotted vertical from the floor up to the curve */
        if (thr > -PLOT_RANGE_DB) {
            var tx = px(thr), ty = py(thr);
            for (var yy = py0 + S - 2; yy > ty; yy -= 2) ctx.fillRect(tx, yy, 1, 1, 1);
        }

        /* the curve itself */
        var prevX = -1, prevY = -1;
        for (var col = 0; col <= span; col++) {
            var x = -PLOT_RANGE_DB + col / span * PLOT_RANGE_DB;
            var y = clampDb(x - reduction(x, thr, ratio, knee));
            var cx = 1 + col, cy = py(y);
            if (prevX >= 0) ctx.line(prevX, prevY, cx, cy, 1);
            prevX = cx; prevY = cy;
        }

        /* where the signal is now */
        if (m && m.inDb !== null && m.inDb > -PLOT_RANGE_DB) {
            var ix = clampDb(m.inDb);
            var dotX = px(ix), dotY = py(clampDb(ix - reduction(ix, thr, ratio, knee)));
            var r = S >= 30 ? 2 : 1;
            dotX = Math.max(1 + r, Math.min(S - 2 - r, dotX));
            dotY = Math.max(py0 + 1 + r, Math.min(py0 + S - 2 - r, dotY));
            ctx.fillCircle(dotX, dotY, r, 1);
        }

        /* ---- vertical reduction meter, filling DOWN from the top ---- */
        var bx = S + 3, bw = 6;
        outline(ctx, bx, py0, bw, S);
        if (m) {
            var fill = Math.round((S - 2) * scale(m.gr));
            if (fill > 0) ctx.fillRect(bx + 1, py0 + 1, bw - 2, fill, 1);
        }
        /* Ticks at 3 / 6 / 12 dB: notches on the bar's inner left edge, in
         * the opposite colour to what they sit on. */
        for (var i = 0; i < TICKS_DB.length; i++) {
            var ty2 = py0 + 1 + Math.round((S - 2) * scale(TICKS_DB[i]));
            var under = m && Math.round((S - 2) * scale(m.gr)) > (ty2 - py0 - 1);
            if (ty2 < py0 + S - 1) ctx.fillRect(bx + 1, ty2, 2, 1, under ? 0 : 1);
        }

        /* ---- readout: each line in the longest form that fits ---- */
        var tx0 = bx + bw + 4;
        var avail = w - tx0;
        var gr = grText(m);
        var inTxt = m && m.inDb !== null ? String(Math.round(m.inDb)) : "--";
        var rTxt = ratio.toFixed(1) + ":1";
        var lines = [
            ["GR " + gr + (m ? " dB" : ""), "GR " + gr, gr],
            ["IN " + inTxt + (inTxt !== "--" ? " dB" : ""), "IN " + inTxt, inTxt],
            ["THR " + thr.toFixed(1), "T " + thr.toFixed(0)],
            ["RATIO " + rTxt, "R " + rTxt, rTxt],
        ];
        var pitch = GLYPH_H + 3;
        var fit = Math.max(1, Math.min(lines.length, Math.floor((h + 3) / pitch)));
        var ly = Math.max(0, Math.floor((h - (fit * pitch - 3)) / 2));
        for (var k = 0; k < fit; k++) {
            for (var v = 0; v < lines[k].length; v++) {
                if (ctx.textWidth(lines[k][v]) <= avail) {
                    ctx.print(tx0, ly + k * pitch, lines[k][v], 1);
                    break;
                }
            }
        }
    }

    globalThis.canvas_overlay = {
        /* Must match chain_params: "viz": { "kind": "custom:grmeter" } */
        widgetKind: "custom:grmeter",
        drawCell: drawCell,
        drawPage: drawPage,
        /* Exposed for the offline tests; the host ignores unknown fields. */
        _test: { parseMeter: parseMeter, reduction: reduction, scale: scale },
    };
})();
