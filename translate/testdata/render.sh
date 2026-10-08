#!/usr/bin/env bash
# Renders the synthetic phone screens (*.html) to 1170x2532 PNGs with headless
# Chrome, plus NAME.gt.tsv: the ground truth of every rendered text line
# (x0 y0 x1 y1 normalised, TAB, text; written by gt.js) for OCR accuracy and
# for pm_translate_test --gt.
#   translate/testdata/render.sh OUTDIR
set -euo pipefail
cd "$(dirname "$0")"
OUT="${1:?output dir}"
mkdir -p "$OUT"
CHROME="${CHROME:-/c/Program Files/Google/Chrome/Application/chrome.exe}"
for f in *.html; do
  n="${f%.html}"
  url="file:///$(cygpath -m "$PWD")/$f"
  "$CHROME" --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=3 --window-size=390,844 \
    --screenshot="$(cygpath -w "$OUT")\\$n.png" "$url" >/dev/null 2>&1
  "$CHROME" --headless=new --disable-gpu --window-size=390,844 --virtual-time-budget=2000 --dump-dom "$url" 2>/dev/null |
    sed -n '/<pre id="gt"/,/<\/pre>/p' |
    sed -e 's/<pre id="gt"[^>]*>//' -e 's#</pre>.*##' -e 's/&amp;/\&/g' -e 's/&lt;/</g' -e 's/&gt;/>/g' >"$OUT/$n.gt.tsv"
  echo "$OUT/$n.png ($(wc -l <"$OUT/$n.gt.tsv") ground-truth lines)"
done
