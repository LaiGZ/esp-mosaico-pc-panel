#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把客户端打包成单个 exe（PyInstaller）。

    python -m pip install pyinstaller
    python build_exe.py

产物：<工作区>/tools/pc_status_panel/pc_status_panel.exe

打包内容：
  * pc_status_gui.py / pc_status_agent.py（界面 + 采集 + 托盘）
  * LibreHardwareMonitor 运行库（agent/lhm，若不存在会自动调用 fetch_lhm.py
    下载）—— 客户端在自己进程内直读传感器，不需要 LHM 进程。

注意：exe 未做代码签名，首次运行 Windows 可能提示"未知发布者"，选择"仍要运行"即可；
读取 CPU 温度需要一次管理员权限装载内核驱动（会弹一次 UAC），之后不再需要。
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# 工作区根（vibe 的上一层），这样产物与 tools/lhm 放在一起、不进仓库。
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(HERE))))
OUT_DIR = os.path.join(ROOT, "tools", "pc_status_panel")
BUILD_DIR = os.path.join(ROOT, "tools", ".pc_status_build")
LHM_DIR = os.path.join(HERE, "lhm")


def ensure_lhm() -> str:
    """确保 agent/lhm 里有 LibreHardwareMonitorLib.dll。

    优先用工作区 tools/lhm 里已有的副本（离线、秒级），否则才去下载。
    """
    dll = os.path.join(LHM_DIR, "LibreHardwareMonitorLib.dll")
    if os.path.isfile(dll):
        return LHM_DIR
    workspace = os.path.join(ROOT, "tools", "lhm")
    if os.path.isfile(os.path.join(workspace, "LibreHardwareMonitorLib.dll")):
        print(f"[build] 从 {workspace} 复制传感器库到 agent/lhm …")
        os.makedirs(LHM_DIR, exist_ok=True)
        for name in os.listdir(workspace):
            src = os.path.join(workspace, name)
            if os.path.isfile(src):
                shutil.copy2(src, os.path.join(LHM_DIR, name))
        return LHM_DIR
    print("[build] agent/lhm 里没有传感器库，先下载 …")
    subprocess.run([sys.executable, os.path.join(HERE, "fetch_lhm.py")], check=True)
    if not os.path.isfile(dll):
        raise SystemExit("下载后仍找不到 LibreHardwareMonitorLib.dll")
    return LHM_DIR


def make_icon(path: str) -> None:
    """用托盘图标的画法生成一个 .ico（深色圆角 + 青色点）。"""
    from PIL import Image

    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = []
    for size in sizes:
        image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        from PIL import ImageDraw

        draw = ImageDraw.Draw(image)
        pad = max(1, size // 16)
        draw.rounded_rectangle((pad, pad, size - pad - 1, size - pad - 1),
                               radius=max(2, size // 6),
                               fill=(14, 21, 29, 255),
                               outline=(30, 42, 54, 255), width=max(1, size // 32))
        r = size // 4
        draw.ellipse((size // 2 - r, size // 2 - r, size // 2 + r, size // 2 + r),
                     fill=(34, 211, 238, 255))
        frames.append(image)
    frames[0].save(path, format="ICO",
                   sizes=[(s, s) for s in sizes], append_images=frames[1:])
    print(f"[build] 图标: {path}")


def main() -> int:
    lhm = ensure_lhm()
    os.makedirs(OUT_DIR, exist_ok=True)
    os.makedirs(BUILD_DIR, exist_ok=True)
    icon = os.path.join(BUILD_DIR, "app.ico")
    make_icon(icon)

    args = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm", "--clean",
        "--onefile", "--windowed",
        "--name", "pc_status_panel",
        "--icon", icon,
        "--distpath", OUT_DIR,
        "--workpath", BUILD_DIR,
        "--specpath", BUILD_DIR,
        # pythonnet 是 .NET 宿主，PyInstaller 需要显式收集
        "--collect-all", "pythonnet",
        "--collect-all", "clr_loader",
        "--hidden-import", "pystray._win32",
        "--hidden-import", "mss",
        "--hidden-import", "mss.windows",
        "--hidden-import", "clr",
        # 传感器库随 exe 一起分发（运行时解包到 sys._MEIPASS/lhm）
        "--add-data", f"{lhm}{os.pathsep}lhm",
        os.path.join(HERE, "pc_status_gui.py"),
    ]
    print("[build] " + " ".join(args[len(args) - 1:]))
    result = subprocess.run(args, cwd=BUILD_DIR)
    if result.returncode != 0:
        return result.returncode

    exe = os.path.join(OUT_DIR, "pc_status_panel.exe")
    if not os.path.isfile(exe):
        print("[build] 失败：没有生成 exe", file=sys.stderr)
        return 1
    print(f"[build] 完成: {exe}  ({os.path.getsize(exe) / 1e6:.1f} MB)")
    shutil.rmtree(BUILD_DIR, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
