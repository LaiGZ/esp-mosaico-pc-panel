# -*- coding: utf-8 -*-
"""Generate the UI font subsets used by ui/main.json.

The scene uses Chinese labels. Shipping a full CJK TTF would add ~15 MB each to
the repository, so this script extracts the exact character set the scene needs
(all literal text plus printable ASCII and a few symbols for dynamic values) and
subsets a system CJK font down to a few KB.

Source font: DengXian (Deng.ttf / Dengb.ttf) shipped with Windows.
Replace SRC_* with any licensed CJK TTF (for example Noto Sans SC, OFL) for
redistribution.

Run from the project directory:
    python ui/fonts/make_fonts.py
"""
from __future__ import annotations

import json
import os
import sys

from fontTools import subset

HERE = os.path.dirname(os.path.abspath(__file__))
SCENE = os.path.join(HERE, "..", "main.json")

SRC_REGULAR = r"C:\Windows\Fonts\Deng.ttf"
SRC_BOLD = r"C:\Windows\Fonts\Dengb.ttf"

# Printable ASCII (host names, numbers, units) plus symbols used dynamically.
EXTRA = (
    "".join(chr(c) for c in range(0x20, 0x7F))
    + "°℃↓↑·…—≥≤"
)


def scene_chars() -> set[str]:
    with open(SCENE, encoding="utf-8") as fh:
        scene = json.load(fh)

    chars: set[str] = set(EXTRA)

    def collect(node) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                if key in ("text", "ok_label", "shift_label", "delete_label",
                           "symbols_label", "letters_label", "space_label") and isinstance(value, str):
                    chars.update(value)
                elif key in ("font_charset", "font_charset_file") and isinstance(value, str):
                    # Declared runtime glyphs must be in the subset too, or the
                    # device renders the missing-glyph placeholder instead.
                    if key == "font_charset":
                        chars.update(value)
                    else:
                        charset_path = os.path.join(os.path.dirname(SCENE),
                                                    value)
                        if os.path.exists(charset_path):
                            with open(charset_path, encoding="utf-8") as fh:
                                chars.update(fh.read())
                        else:
                            print(f"missing font_charset_file: {charset_path}",
                                  file=sys.stderr)
                            sys.exit(5)
                else:
                    collect(value)
        elif isinstance(node, list):
            for item in node:
                collect(item)

    collect(scene)
    chars.discard("\n")

    # Strings the firmware sets at runtime (C sources in main/) also need glyphs.
    main_dir = os.path.join(HERE, "..", "..", "main")
    if not os.path.isdir(main_dir):
        print(f"expected the application sources at {main_dir}", file=sys.stderr)
        sys.exit(3)
    for name in sorted(os.listdir(main_dir)):
        if not name.endswith((".c", ".h")):
            continue
        with open(os.path.join(main_dir, name), encoding="utf-8") as fh:
            for ch in fh.read():
                if ord(ch) > 0x7F:
                    chars.add(ch)

    return chars


def verify(font_path: str, chars: set[str]) -> None:
    """Fail loudly when a requested character is missing from the subset.

    A missing glyph renders as '?' on the device, which is easy to miss in a
    small screenshot, so this is checked at build-input time instead.
    """
    from fontTools.ttLib import TTFont

    cmap = TTFont(font_path).getBestCmap()
    missing = sorted(ch for ch in chars if ord(ch) not in cmap)
    if missing:
        print(f"{os.path.basename(font_path)} is missing glyphs: {''.join(missing)}",
              file=sys.stderr)
        sys.exit(4)


def subset_font(src: str, dst: str, chars: set[str]) -> None:
    if not os.path.exists(src):
        print(f"missing source font: {src}", file=sys.stderr)
        sys.exit(2)
    args = [
        src,
        f"--text={''.join(sorted(chars))}",
        "--layout-features=*",
        "--no-hinting",
        "--desubroutinize",
        "--name-IDs=*",
        f"--output-file={dst}",
    ]
    subset.main(args)
    print(f"{os.path.basename(dst)}: {os.path.getsize(dst) / 1024:.1f} KB "
          f"({len(chars)} chars)")


def main() -> int:
    chars = scene_chars()
    cjk = sum(1 for ch in chars if ord(ch) > 0x7F)
    print(f"charset: {len(chars)} characters ({cjk} non-ASCII)")
    regular = os.path.join(HERE, "pc-ui.ttf")
    bold = os.path.join(HERE, "pc-ui-bold.ttf")
    subset_font(SRC_REGULAR, regular, chars)
    subset_font(SRC_BOLD, bold, chars)
    verify(regular, chars)
    verify(bold, chars)

    # ESP-GSP bakes glyphs for runtime-bound text from the scene's
    # `font_charset`/`font_charset_file`, not from the whole TTF, so this file
    # is what actually guarantees coverage on the device.
    charset_file = os.path.join(HERE, "..", "font_charset.txt")
    with open(charset_file, "w", encoding="utf-8") as fh:
        fh.write("".join(sorted(chars)))
    print(f"wrote {os.path.relpath(charset_file, os.path.join(HERE, '..', '..'))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
