#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Run the pinned ESP-GSP compiler and normalise its Windows depfile.

The pinned `gspc` writes dependency files using extended-length paths written
in an escaped form, for example::

    \\\\?\\D:\\project\\ui\\fonts\\pc-ui.ttf

CMake's `cmake_transform_depfile Ninja gccdepfile` turns those into the invalid
path `/D:/project/ui/fonts/pc-ui.ttf`, and Ninja then fails with
`FindFirstFileExA(/D:/...): The filename, directory name, or volume label
syntax is incorrect.`

This wrapper forwards every argument to the real compiler, preserves its exit
code and output (so `--version` compatibility checks still work), and rewrites
the `--depfile` it produced into plain `drive:/path` form.

Selected through `GSPC_EXECUTABLE` by `CMakeLists.txt` on Windows only.
"""
from __future__ import annotations

import glob
import os
import shutil
import subprocess
import sys

BACKSLASH = chr(92)
DOUBLED = BACKSLASH + BACKSLASH
# gspc may write the marker either doubled (\\\\?\\D:) or plain (\\?\D:).
DOUBLED_MARKER = BACKSLASH * 4 + "?"
MARKER = BACKSLASH + BACKSLASH + "?" + BACKSLASH


def find_real_gspc() -> str | None:
    override = os.environ.get("GSPC_REAL_EXECUTABLE")
    if override and os.path.exists(override):
        return override

    cache = os.path.join(os.path.expanduser("~"), ".cache", "esp-mosaico", "gspc")
    candidates = []
    for pattern in ("*/gspc.exe", "*/gspc"):
        candidates.extend(glob.glob(os.path.join(cache, pattern)))
    if candidates:
        if len(candidates) == 1:
            return candidates[0]
        # Several pinned versions may be cached; the newest wins.
        versions = []
        for path in candidates:
            version = os.path.basename(os.path.dirname(path))
            versions.append((version, path))
        versions.sort(key=lambda item: item[0], reverse=True)
        return versions[0][1]

    return shutil.which("gspc")


def depfile_path(args: list[str]) -> str | None:
    for index, arg in enumerate(args):
        if arg == "--depfile" and index + 1 < len(args):
            return args[index + 1]
        if arg.startswith("--depfile="):
            return arg.split("=", 1)[1]
    return None


def normalise_depfile(path: str) -> None:
    if not os.path.exists(path):
        return
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()

    if MARKER not in text and DOUBLED_MARKER not in text:
        return

    if DOUBLED_MARKER in text:
        text = text.replace(DOUBLED, BACKSLASH)  # un-escape doubled separators
    text = text.replace(MARKER, "")               # drop the \\?\ prefix
    text = text.replace(BACKSLASH, "/")           # drive:/path for Ninja

    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def main(argv: list[str]) -> int:
    real = find_real_gspc()
    if not real:
        sys.stderr.write(
            "gspc_wrap: no ESP-GSP compiler found. Set GSPC_REAL_EXECUTABLE or "
            "let the build download the pinned gspc.\n")
        return 2

    completed = subprocess.run([real, *argv])
    target = depfile_path(argv)
    if target:
        try:
            normalise_depfile(target)
        except OSError as exc:  # pragma: no cover - diagnostics only
            sys.stderr.write(f"gspc_wrap: depfile fix skipped: {exc}\n")
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
