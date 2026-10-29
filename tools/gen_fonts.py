#!/usr/bin/env python3
"""UI ソース中の文字列リテラルから使用文字を集め、LVGL 用の Noto Sans JP サブセットフォントを生成する。

使い方: python3 tools/gen_fonts.py
必要: Node.js（npx で lv_font_conv を実行）, tools/fetch_fonts.sh で取得した元フォント
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC_DIRS = [ROOT / "main", ROOT / "components"]
FONT_SRC = ROOT / "assets" / "fonts" / "src"
OUT_DIR = ROOT / "components" / "ui" / "fonts"
EXTRA = ROOT / "assets" / "fonts" / "extra_chars.txt"
# npm 公開版 (1.5.x) は LVGL 9 非対応のため GitHub の版を固定して使う
LV_FONT_CONV = ["npx", "--yes", "github:lvgl/lv_font_conv#2107ab79b615a6ff7051796fa9f7ae52f042437b"]

ASCII = "0x20-0x7E"
NUMERIC = "0123456789.-+ :%"

# name, weight, size, charset ("ui" = 抽出した UI 文字, それ以外はその文字のみ), fallback
FONTS = [
    ("font_jp_16", "Regular", 16, "ui", "lv_font_montserrat_16"),
    ("font_jp_20", "Regular", 20, "ui", "lv_font_montserrat_20"),
    ("font_jp_24", "Medium", 24, "ui", "lv_font_montserrat_24"),
    ("font_jp_32", "Bold", 32, "ui", "lv_font_montserrat_32"),
    ("font_num_96", "Bold", 96, NUMERIC, None),
]


STRING_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def collect_ui_chars() -> str:
    chars = set()
    for d in SRC_DIRS:
        for p in d.rglob("*"):
            if p.suffix not in (".c", ".h") or OUT_DIR in p.parents:
                continue
            for lit in STRING_RE.findall(p.read_text(encoding="utf-8")):
                chars.update(c for c in lit if ord(c) > 0x7E)
    if EXTRA.exists():
        chars.update(c for c in EXTRA.read_text(encoding="utf-8") if ord(c) > 0x7E and not c.isspace())
    return "".join(sorted(chars))


def main() -> int:
    ui_chars = collect_ui_chars()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, weight, size, charset, fallback in FONTS:
        src = FONT_SRC / f"NotoSansJP-{weight}.otf"
        if not src.exists():
            print(f"missing {src} (run tools/fetch_fonts.sh)", file=sys.stderr)
            return 1
        # ROOT からの相対パスで渡す（生成ファイルの先頭に記録されるため）
        args = LV_FONT_CONV + ["--font", str(src.relative_to(ROOT)), "--size", str(size), "--bpp", "4", "--format", "lvgl",
                               "--no-compress", "--lv-include", "lvgl.h", "--lv-font-name", name,
                               "-o", str((OUT_DIR / f"{name}.c").relative_to(ROOT))]
        if charset == "ui":
            args += ["-r", ASCII]
            if ui_chars:
                args += ["--symbols", ui_chars]
        else:
            args += ["--symbols", charset]
        if fallback:
            args += ["--lv-fallback", fallback]
        subprocess.run(args, check=True, cwd=ROOT)
        print(f"{name}: {size}px {weight}")

    decls = "\n".join(f"LV_FONT_DECLARE({f[0]})" for f in FONTS)
    (OUT_DIR / "ui_fonts.h").write_text(
        "/* tools/gen_fonts.py が生成。手で編集しない */\n#pragma once\n\n#include \"lvgl.h\"\n\n" + decls + "\n",
        encoding="utf-8")
    print(f"UI chars ({len(ui_chars)}): {ui_chars}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
