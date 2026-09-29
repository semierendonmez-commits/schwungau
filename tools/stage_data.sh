#!/usr/bin/env bash
# Assemble the data tree that ships inside the plugin bundles:
#   <out>/schwung/modules/...        chain host + every built module
#   <out>/schwung/patches/*.json     Schwung's bundled patches
#   <out>/schwung/module-catalog.json
#   <out>/build-id.txt               lets the plugin tell a new bundle from an old one
# Usage: stage_data.sh <rootfs> <out>
set -euo pipefail
ROOTFS="$(cd "$1" && pwd)"; OUT="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$OUT"; mkdir -p "$OUT/schwung"
# Only what a chain can load, without build by-products: debug symbols,
# repo furniture that some packaging scripts sweep up, and the Move-screen
# tools/overtake modules (no audio component to host).
rsync -a --exclude '.obj' --exclude 'logs' --exclude '*.jsonl' --exclude 'build_report.json' \
      --exclude '.au_state' --exclude '*.dSYM' --exclude '.github' --exclude '.git' \
      --exclude 'browser-test' --exclude 'tests' --exclude 'test' --exclude 'docs' --exclude 'archive' \
      --exclude '/modules/tools' --exclude '/modules/overtake' \
      "$ROOTFS/modules" "$OUT/schwung/"
[ -d "$ROOTFS/patches" ] && rsync -a "$ROOTFS/patches" "$OUT/schwung/"
cp "$HERE/../module-catalog.json" "$OUT/schwung/module-catalog.json"
( cd "$OUT" && find schwung -type f -print0 | sort -z | xargs -0 cat | shasum | cut -c1-16 ) > "$OUT/build-id.txt"
echo "staged $(find "$OUT/schwung/modules" -name module.json | wc -l | tr -d ' ') modules, build $(cat "$OUT/build-id.txt")"
