#!/usr/bin/env python3
"""The contract the plugin SERVES must be the one module.json DECLARES.

    build/test_compressor --dump-contract > build/served_contract.txt
    tests/check_contract.py build/served_contract.txt

Also checks the hierarchy against chain_params: every key a level names is
declared, every declared control is reachable, and no level maps more than the
eight physical knobs.
"""

import json
import sys

served = open(sys.argv[1]).read().split("\n")
caps = json.load(open("src/module.json"))["capabilities"]
ok = True

if json.loads(served[0]) != caps["chain_params"]:
    print("  FAIL chain_params served by the plugin differ from module.json")
    ok = False
if json.loads(served[1]) != caps["ui_hierarchy"]:
    print("  FAIL ui_hierarchy served by the plugin differs from module.json")
    ok = False

declared = {p["key"] for p in caps["chain_params"]}
named = set()
for lvl in caps["ui_hierarchy"]["levels"].values():
    named.update(lvl.get("knobs", []))
    for p in lvl.get("params", []):
        if isinstance(p, dict) and "key" in p:
            named.add(p["key"])
missing = named - declared
unreached = declared - named
if missing:
    print("  FAIL hierarchy names undeclared keys:", sorted(missing))
    ok = False
if unreached:
    print("  FAIL declared but unreachable:", sorted(unreached))
    ok = False
for name, lvl in caps["ui_hierarchy"]["levels"].items():
    if len(lvl.get("knobs", [])) > 8:
        print("  FAIL level %s maps more than 8 knobs" % name)
        ok = False

# The meter must stay a live, read-only, custom-drawn key: losing any of the
# three degrades it silently (no refresh, a turnable cell, or a plain dial).
gr = next((p for p in caps["chain_params"] if p["key"] == "gr"), None)
if not gr or gr.get("access") != "read" or gr.get("live") is not True or \
        (gr.get("viz") or {}).get("kind") != "custom:grmeter":
    print("  FAIL gr must be access:read, live:true, viz.kind custom:grmeter")
    ok = False

print("  contract OK" if ok else "")
sys.exit(0 if ok else 1)
