#!/usr/bin/env python3
"""Every help.json line must fit the 128px screen.

A help line is drawn at x=4 and never wrapped: past x=127 it is silently cut,
with nothing logged. Measured with the device font's glyph widths
(tests/font_widths.json, taken from Schwung's font table). A character with no
glyph is drawn as a gap, so it fails too.
"""

import json
import sys

f = json.load(open("tests/font_widths.json"))
glyphs, spacing = f["widths"], f["charSpacing"]
BUDGET = 123


def width(s):
    if any(ch not in glyphs for ch in s):
        return None
    return sum(glyphs[ch] + spacing for ch in s) - spacing if s else 0


doc = json.load(open("src/help.json"))
if not doc.get("children"):
    print("  FAIL help.json has no top-level children; the viewer would ignore it")
    sys.exit(1)

bad = []


def walk(node, path):
    for child in node.get("children", []):
        walk(child, path + [child["title"]])
    for line in node.get("lines", []):
        w = width(line)
        if w is None or w > BUDGET:
            bad.append((" / ".join(path), line, w))


walk(doc, [])
for where, line, w in bad:
    print("  FAIL %s: %r (%s px, budget %d)" % (where, line, w, BUDGET))
print("  help OK" if not bad else "")
sys.exit(1 if bad else 0)
