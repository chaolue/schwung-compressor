#!/usr/bin/env bash
# Deploy dist/compressor/ to a Move running Schwung.
#   scripts/install.sh                          (WiFi, move.local)
#   MOVE_HOST=172.16.254.1 scripts/install.sh   (USB tether)
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
MODULE_ID=compressor
HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/audio_fx/${MODULE_ID}"

[ -d "$HERE/dist/${MODULE_ID}" ] || { echo "run scripts/build.sh first" >&2; exit 1; }

echo "==> installing to ableton@${HOST}:${DEST}"
ssh "ableton@${HOST}" "mkdir -p '${DEST}'"
# Upload under a temporary name and rename, so a .so that is currently
# loaded is replaced rather than overwritten in place (ETXTBSY).
scp "$HERE/dist/${MODULE_ID}/${MODULE_ID}.so" "ableton@${HOST}:${DEST}/.${MODULE_ID}.so.new"
ssh "ableton@${HOST}" "mv -f '${DEST}/.${MODULE_ID}.so.new' '${DEST}/${MODULE_ID}.so'"
for f in module.json help.json canvas.js; do
    scp "$HERE/dist/${MODULE_ID}/$f" "ableton@${HOST}:${DEST}/"
done
# Leave it writable so the web manager can update it later.
ssh "ableton@${HOST}" "chmod -R a+rw '${DEST}'"

echo "==> installed. Remove and re-add the FX (or restart Schwung) to load a new build."
