#!/bin/sh
# Noto Sans JP（SIL OFL 1.1）を assets/fonts/src に取得する
set -eu
DIR="$(cd "$(dirname "$0")/.." && pwd)/assets/fonts/src"
mkdir -p "$DIR"
BASE="https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/JP"
for w in Regular Medium Bold; do
    [ -f "$DIR/NotoSansJP-$w.otf" ] || curl -sSfL -o "$DIR/NotoSansJP-$w.otf" "$BASE/NotoSansJP-$w.otf"
done
echo "fonts in $DIR"
