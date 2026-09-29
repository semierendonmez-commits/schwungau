#!/usr/bin/env bash
# One-shot macOS build: modules -> Schwung chain host -> plugin bundles.
#
#   tools/build_mac.sh                      everything, native arch
#   INSTALL=1 tools/build_mac.sh            ... and install AU/VST3 for this user, run auval
#   SKIP_MODULES=1 INSTALL=1 tools/build_mac.sh
#                                           reuse the modules built last time
#                                           (skips steps 1-2: fetch + module build)
#   ONLY=moog,cloudseed,arp tools/build_mac.sh    a subset, fast
#   ARCHS="arm64 x86_64" tools/build_mac.sh        universal binaries
#   LATEST=1 tools/build_mac.sh             every module at its current HEAD
#   AUV3=1 tools/build_mac.sh               also build the AUv3 (needs full Xcode)
#
# Needs: Xcode Command Line Tools, CMake >= 3.22, Python 3, git, rsync.
# Optional: ninja (faster), cargo (Rust modules), zig (plaits),
# automake/libtool/autoconf (AirPlay). Missing optional tools are reported
# per module and never stop the build.
set -eo pipefail   # no -u: macOS bash 3.2 treats empty arrays as unset
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WORK="${WORK:-$ROOT/build-mac}"
MODS="$WORK/mods"
ROOTFS="$WORK/rootfs"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
export SCHWUNG_ARCHS="${ARCHS:-$(uname -m)}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
mkdir -p "$WORK"

# ---------------------------------------------------------------- preflight
missing=()
xcode-select -p >/dev/null 2>&1 || missing+=("Xcode Command Line Tools  ->  xcode-select --install")
for t in cmake python3 git rsync; do command -v $t >/dev/null 2>&1 || missing+=("$t  ->  brew install $t"); done
if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing required tools:"; printf '   %s\n' "${missing[@]}"; exit 1
fi
if command -v ninja >/dev/null 2>&1; then GEN="Ninja"; else GEN="Unix Makefiles"; echo "note: ninja not found, using make (brew install ninja is faster)"; fi
opt=()
for t in cargo zig automake libtool; do command -v $t >/dev/null 2>&1 || opt+=("$t"); done
[ ${#opt[@]} -gt 0 ] && echo "note: optional tools missing (${opt[*]}); the modules that need them will be skipped. brew install rust zig automake libtool"
WITH_AUV3=OFF
if [ -n "${AUV3:-}" ]; then
    if xcodebuild -version >/dev/null 2>&1; then WITH_AUV3=ON
    else echo "AUV3=1 needs the full Xcode app (not only the Command Line Tools); building AU/VST3/Standalone."; fi
fi

# ---------------------------------------------------------------- 1-2 modules
if [ -z "${SKIP_MODULES:-}" ]; then
    echo "== 1/6 fetch module sources"
    python3 "$HERE/fetch_modules.py" --dest "$MODS" ${LATEST:+--latest} ${ONLY:+--only "$ONLY"} --jobs 8

    echo "== 2/6 build modules (each with its own build script, via the toolchain shim)"
    python3 "$HERE/build_modules.py" --src "$MODS" --schwung "$MODS/charlesvestal__schwung" \
        --catalog "$ROOT/module-catalog.json" --out "$ROOTFS" ${ONLY:+--only "$ONLY"} \
        --jobs "${MODULE_JOBS:-2}" --timeout 3600
else
    echo "== 1-2/6 skipped (SKIP_MODULES): reusing $ROOTFS"
    [ -d "$MODS/charlesvestal__schwung" ] || { echo "no previous module build in $WORK; run once without SKIP_MODULES"; exit 1; }
fi
SCHWUNG_SRC="$MODS/charlesvestal__schwung"

echo "== 3/6 build Schwung chain host + built-in modules"
"$HERE/build_host_modules.sh" "$SCHWUNG_SRC" "$ROOTFS"

echo "== 4/6 stage data"
"$HERE/stage_data.sh" "$ROOTFS" "$WORK/SchwungData"

echo "== 5/6 build plugins (AU, VST3, Standalone$( [ $WITH_AUV3 = ON ] && echo ', AUv3'))"
cmake -S "$ROOT" -B "$WORK/cmake" -G "$GEN" -DCMAKE_BUILD_TYPE=Release \
      -DSCHWUNG_SRC="$SCHWUNG_SRC" -DSCHWUNG_STAGED_DATA="$WORK/SchwungData" \
      -DSCHWUNG_AUV3="$WITH_AUV3" \
      -DCMAKE_OSX_ARCHITECTURES="$(echo $SCHWUNG_ARCHS | tr ' ' ';')" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
cmake --build "$WORK/cmake" --config Release -j "$JOBS"

ART="$WORK/cmake/plugin"
# Ad-hoc sign every bundle: copying the module tree into Resources after the
# link invalidates the linker's signature, and Apple silicon refuses
# unsigned code.
while IFS= read -r b; do
    codesign --force --deep --sign - "$b" >/dev/null 2>&1 || echo "warning: could not sign $b"
done < <(find "$ART" -path '*_artefacts/Release/*' \( -name '*.component' -o -name '*.vst3' -o -name '*.app' \) -prune)

echo "== 6/6 verify: load, play and restore every module on this machine"
# Each module runs in its own process; one that cannot load (a missing symbol
# only shows up at dlopen with -undefined dynamic_lookup) is reported, not
# fatal. Writes build-mac/COMPATIBILITY-macos.md.
mkdir -p "$WORK/verify"; ln -sfn "$WORK/SchwungData/schwung" "$WORK/verify/schwung"
"$WORK/cmake/sw_render" --data "$WORK/verify" --sweep --seconds 2 --json "$WORK/sweep.json" > "$WORK/sweep.txt" 2> "$WORK/sweep.err" || true
tail -1 "$WORK/sweep.err" || true
python3 - "$WORK/sweep.json" <<'PY' || true
import json, sys
try: rs = json.load(open(sys.argv[1]))
except Exception: sys.exit(0)
bad = [r for r in rs if not r.get("loaded")]
for r in bad[:8]:
    print("   not loaded:", r["id"], "-", (r.get("crash") or r.get("error") or "")[:160])
if len(bad) > 8: print(f"   ... and {len(bad) - 8} more (see COMPATIBILITY-macos.md)")
PY
"$WORK/cmake/sw_latency" --data "$WORK/verify" > "$WORK/latency.txt" 2>&1 && echo "timing: all checks passed" \
    || echo "timing: FAILED, see $WORK/latency.txt"
python3 "$HERE/compat_report.py" --results "$ROOTFS/results.jsonl" --sweep "$WORK/sweep.json" \
        --out "$WORK/COMPATIBILITY-macos.md" || true

echo
echo "Built:"
find "$ART" -path '*_artefacts/Release/*' \( -name '*.component' -o -name '*.vst3' -o -name '*.app' \) -prune -print
echo "report: $WORK/COMPATIBILITY-macos.md"

if [ -n "${INSTALL:-}" ]; then
    echo "== install"
    # Copy a bundle into a plug-in folder, replacing any older copy. A folder
    # we cannot write to (created earlier by an installer running as root) is
    # reported with the command that fixes it; the rest carries on.
    install_bundle() {  # <bundle> <dest dir>
        local src="$1" dst="$2" name; name="$(basename "$1")"
        mkdir -p "$dst" 2>/dev/null || true
        if [ ! -w "$dst" ] || { [ -e "$dst/$name" ] && [ ! -w "$dst/$name" ]; }; then
            echo "   skipped $name: $dst is not writable. Fix once with:"
            echo "       sudo chown -R \"$(whoami)\" \"$dst\""
            return 0
        fi
        rm -rf "$dst/$name" && ditto "$src" "$dst/$name" && echo "   $name -> $dst"
    }
    for c in "$ART"/*_artefacts/Release/AU/*.component;  do install_bundle "$c" ~/Library/Audio/Plug-Ins/Components; done
    for v in "$ART"/*_artefacts/Release/VST3/*.vst3;     do install_bundle "$v" ~/Library/Audio/Plug-Ins/VST3; done
    for a in "$ART"/*_artefacts/Release/Standalone/*.app; do install_bundle "$a" /Applications; done
    # Make the system re-scan Audio Units.
    killall -9 AudioComponentRegistrar 2>/dev/null || true
    echo "== auval"
    auval -v aumu Swng Schw > "$WORK/auval-Schwung.txt" 2>&1 && echo "Schwung    (aumu Swng Schw): PASS" \
        || echo "Schwung    (aumu Swng Schw): FAIL, see $WORK/auval-Schwung.txt"
    auval -v aumf Swfx Schw > "$WORK/auval-SchwungFX.txt" 2>&1 && echo "Schwung FX (aumf Swfx Schw): PASS" \
        || echo "Schwung FX (aumf Swfx Schw): FAIL, see $WORK/auval-SchwungFX.txt"
fi
