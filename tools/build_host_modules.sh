#!/usr/bin/env bash
# Build the Schwung host-side modules the shell needs, with the SAME compile
# lines as schwung/scripts/build.sh:
#
#   modules/chain/dsp.so                    Signal Chain host (unmodified source)
#   modules/audio_fx/freeverb/freeverb.so   built-in audio FX
#   modules/midi_fx/{arp,chord}/dsp.so      built-in MIDI FX
#   modules/sound_generators/linein/dsp.so  audio input as a sound source
#
# Usage: build_host_modules.sh <schwung-src> <rootfs>
set -euo pipefail
SRC="$(cd "$1" && pwd)"
ROOT="$(mkdir -p "$2" && cd "$2" && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
export PATH="$HERE/toolchain/bin:$PATH"
export SCHWUNG_FSWRAP_OBJ="$HERE/toolchain/.cache/sw_fswrap.o"
mkdir -p "$HERE/toolchain/.cache"
${CC_NATIVE:-cc} -O2 -fPIC -fvisibility=hidden -c "$HERE/../core/src/sw_fswrap.c" -o "$SCHWUNG_FSWRAP_OBJ"
CC=aarch64-linux-gnu-gcc
M="$ROOT/modules"
OBJ="$ROOT/.obj/chain"
mkdir -p "$M/chain" "$M/audio_fx/freeverb" "$M/midi_fx/arp" "$M/midi_fx/chord" \
         "$M/sound_generators/linein" "$OBJ"
cd "$SRC"

echo "== chain host"
$CC -g -O3 -fPIC -fvisibility=hidden -c src/host/lane_store.c  -o "$OBJ/lane_store.o"  -Isrc
$CC -g -O3 -fPIC -fvisibility=hidden -c src/host/lane_serial.c -o "$OBJ/lane_serial.o" -Isrc
$CC -g -O3 -fPIC -fvisibility=hidden -c src/host/lane_edit.c   -o "$OBJ/lane_edit.o"   -Isrc -Isrc/host
$CC -g -O3 -shared -fPIC \
    src/modules/chain/dsp/chain_host.c src/modules/chain/dsp/chain_json.c \
    src/modules/chain/dsp/chain_params.c src/modules/chain/dsp/chain_mod.c \
    src/modules/chain/dsp/chain_midi.c src/modules/chain/dsp/chain_patch.c \
    src/modules/chain/dsp/chain_reorder.c src/modules/chain/dsp/chain_bus.c \
    src/modules/chain/dsp/chain_scene.c src/modules/chain/dsp/chain_lanes.c \
    src/host/unified_log.c \
    "$OBJ/lane_store.o" "$OBJ/lane_serial.o" "$OBJ/lane_edit.o" \
    -o "$M/chain/dsp.so" -Isrc -lm -ldl -lpthread
cp src/modules/chain/module.json "$M/chain/"
[ -f src/modules/chain/help.json ] && cp src/modules/chain/help.json "$M/chain/"
[ -d src/modules/chain/midi_fx ] && cp -R src/modules/chain/midi_fx "$M/chain/"

echo "== freeverb"
$CC -g -O3 -shared -fPIC src/modules/audio_fx/freeverb/freeverb.c \
    -o "$M/audio_fx/freeverb/freeverb.so" -Isrc -lm
cp src/modules/audio_fx/freeverb/{module.json,help.json} "$M/audio_fx/freeverb/"

for fx in chord arp; do
  echo "== $fx"
  $CC -g -O3 -shared -fPIC "src/modules/midi_fx/$fx/dsp/$fx.c" \
      -o "$M/midi_fx/$fx/dsp.so" -Isrc
  cp src/modules/midi_fx/$fx/module.json "$M/midi_fx/$fx/"
  [ -f src/modules/midi_fx/$fx/help.json ] && cp src/modules/midi_fx/$fx/help.json "$M/midi_fx/$fx/"
done

echo "== linein"
$CC -g -O3 -shared -fPIC src/modules/sound_generators/linein/linein.c \
    -o "$M/sound_generators/linein/dsp.so" -Isrc -lm
cp src/modules/sound_generators/linein/{module.json,help.json} "$M/sound_generators/linein/"

# Patches shipped with Schwung
mkdir -p "$ROOT/patches"
cp src/patches/*.json "$ROOT/patches/" 2>/dev/null || true
echo "done -> $M"
