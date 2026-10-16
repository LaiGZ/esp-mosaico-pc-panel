#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Sanity checks for ui/main.json.

ESP-GSP scenes are painted in object order and there is no real occlusion, so a
mistake here shows up as "a control appears somewhere else" — exactly the bug
this script is meant to catch. Run it after any scene edit:

    python ui/check_scene.py

Checks:
  * every `parent` index is valid and points at a container-like object;
  * no status-panel (root-level, non-page) object is painted after the first
    full-screen page, or it would draw on top of every page;
  * object names are unique and bound names are non-empty;
  * reports the child count of each page so a broken relationship is obvious.
"""
from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCENE = os.path.join(HERE, "main.json")
CONTAINER_TYPES = {"container", "button", "keyboard"}


def main() -> int:
    scene = json.load(open(SCENE, encoding="utf-8"))
    objs = scene["objects"]
    problems: list[str] = []

    # --- parents ---------------------------------------------------------
    for i, obj in enumerate(objs):
        parent = obj.get("parent", -1)
        if parent == -1:
            continue
        if not 0 <= parent < len(objs):
            problems.append(f"#{i} {obj.get('name') or obj['type']}: "
                            f"parent {parent} out of range")
            continue
        kind = objs[parent].get("type")
        if kind not in CONTAINER_TYPES:
            problems.append(f"#{i} {obj.get('name') or obj['type']}: parent "
                            f"#{parent} is a {kind}, which cannot contain objects")

    # --- paint order vs. pages -------------------------------------------
    pages = [(i, o.get("name")) for i, o in enumerate(objs)
             if o.get("type") == "container" and o.get("w") == scene["w"]
             and o.get("h") == scene["h"] and o.get("parent", -1) == -1]
    if pages:
        first_page = min(i for i, _ in pages)
        page_indexes = {i for i, _ in pages}
        for i, obj in enumerate(objs):
            if i in page_indexes or obj.get("parent", -1) != -1:
                continue
            if i >= first_page:
                problems.append(
                    f"#{i} {obj.get('name') or obj['type']}: root-level object "
                    f"painted after the first page (#{first_page}); it will "
                    f"draw on top of every page")

    # --- names -----------------------------------------------------------
    seen: dict[str, int] = {}
    for i, obj in enumerate(objs):
        name = obj.get("name")
        if not name:
            continue
        if name in seen:
            problems.append(f"duplicate object name {name!r} (#{seen[name]} and #{i})")
        seen[name] = i
        if obj.get("bind") and not obj["bind"].strip():
            problems.append(f"#{i} {name}: empty bind")

    # --- report ----------------------------------------------------------
    print(f"{os.path.relpath(SCENE)}: {len(objs)} objects, {len(pages)} full-screen pages")
    for i, name in pages:
        kids = sum(1 for o in objs if o.get("parent") == i)
        print(f"  page {name:<18} index {i:<4} children {kids}")
    if problems:
        print("\n发现问题:")
        for text in problems:
            print(f"  - {text}")
        return 1
    print("\n检查通过：父子关系、绘制顺序（页面之外无顶层对象）与名字都正常。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
