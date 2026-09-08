#!/usr/bin/env bash
# build_packs.sh — regenerate every reader glyph pack (all 5 sizes x plain/tajweed)
# for the whole Quran, from the waqf-marked Uthmani text + remapped tajweed
# annotations (see tools/build_waqf_text.py). Writes into sdcard/packs, matching
# the layout the device + sim load. Long job (~14 GB); safe to re-run.
set -euo pipefail
cd "$(dirname "$0")/.."

PY=tools/.venv/bin/python; [ -x "$PY" ] || PY=python3
TEXT=tools/quran-tajweed/quran-uthmani-waqf.txt
ANNOT=tools/quran-tajweed/output/tajweed.hafs.waqf.json
SURAHS=${1:-1-114}

# size tag -> glyph pixel size (reproduces the current packs' baked line_h).
# Plain case (not an associative array) so this runs under macOS Bash 3.2.
px_for() {
  case "$1" in
    sm) echo 26 ;; md) echo 32 ;; lg) echo 40 ;; xl) echo 52 ;; xxl) echo 66 ;;
  esac
}

for sz in sm md lg xl xxl; do
  px=$(px_for "$sz")
  for tj in "" "_tj"; do
    flag=""; [ -n "$tj" ] && flag="--tajweed"
    out="sdcard/packs/reader_${sz}${tj}"
    echo "=== building $out (px=$px $flag) ==="
    "$PY" tools/shape_quran.py --px "$px" --maxw 300 --surahs "$SURAHS" \
      $flag --text-file "$TEXT" --annot-file "$ANNOT" --outdir "$out"
    echo "=== done $out ==="
  done
done
echo "ALL PACKS BUILT"
