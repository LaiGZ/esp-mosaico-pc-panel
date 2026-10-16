#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 LibreHardwareMonitor 的运行库下载到客户端目录，供进程内直读使用。

用法（在 agent 目录下执行一次）：
    python -m pip install pythonnet        # 让 Python 能加载 .NET 库
    python fetch_lhm.py

脚本会取最新 release 解压到 ./lhm。客户端随后用其中的
**LibreHardwareMonitorLib.dll** 在**自己进程内**读传感器
（见 pc_status_agent.LhmDirect）：不需要 LHM 进程、不需要 Web 服务器、
也不会多出托盘图标 —— 托盘里只有我们的客户端。

同时也写了一份把 Web 服务器开在 127.0.0.1 的配置，作为直读不可用时的**兜底**
（那时客户端会自动把 LibreHardwareMonitor.exe 最小化拉起）。

为什么要它：Windows 不允许用户态读 CPU 封装温度/功率，必须由带内核驱动的读取器
提供 —— LHM（MPL-2.0）就是这个角色，可以随客户端一起分发。第一次读取温度时
Windows 会弹一次 UAC（装载驱动），之后不再需要。
"""
from __future__ import annotations

import io
import os
import socket
import sys
import urllib.request
import zipfile

REPO = "LibreHardwareMonitor/LibreHardwareMonitor"
ASSET = "LibreHardwareMonitor.zip"
HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, "lhm")
PORT_HINTS = (8085, 8086, 18086, 8899)

CONFIG = """<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <appSettings>
    <add key="runWebServerMenuItem" value="true" />
    <add key="listenerIp" value="127.0.0.1" />
    <add key="listenerPort" value="{port}" />
    <add key="authenticationEnabled" value="false" />
    <add key="startMinMenuItem" value="true" />
    <add key="minTrayMenuItem" value="true" />
  </appSettings>
</configuration>
"""


def free_port() -> int:
    """挑一个能绑上的端口，首选 LHM 的默认端口。"""
    for port in PORT_HINTS:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                probe.bind(("127.0.0.1", port))
            except OSError:
                continue
            return port
    raise SystemExit("8085/8086/18086/8899 都被占用，请手动指定端口")


def latest_asset_url() -> str:
    """跟随 releases/latest 的重定向拿到下载地址（GitHub API 可能被拦）。"""
    url = f"https://github.com/{REPO}/releases/latest"
    request = urllib.request.Request(url, headers={"User-Agent": "pc-status"})
    with urllib.request.urlopen(request, timeout=30) as response:
        final = response.geturl()          # .../releases/tag/vX.Y.Z
    tag = final.rstrip("/").rsplit("/", 1)[-1]
    return f"https://github.com/{REPO}/releases/download/{tag}/{ASSET}"


def main() -> int:
    os.makedirs(TARGET, exist_ok=True)
    url = latest_asset_url()
    print(f"[fetch-lhm] 下载 {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "pc-status"})
    with urllib.request.urlopen(request, timeout=120) as response:
        blob = response.read()
    print(f"[fetch-lhm] 已下载 {len(blob) / 1e6:.1f} MB，解压到 {TARGET}")
    with zipfile.ZipFile(io.BytesIO(blob)) as archive:
        archive.extractall(TARGET)

    port = free_port()
    config = os.path.join(TARGET, "LibreHardwareMonitor.config")
    with open(config, "w", encoding="utf-8") as handle:
        handle.write(CONFIG.format(port=port))
    print(f"[fetch-lhm] Web 服务器配置为 127.0.0.1:{port}（{config}）")

    exe = os.path.join(TARGET, "LibreHardwareMonitor.exe")
    dll = os.path.join(TARGET, "LibreHardwareMonitorLib.dll")
    if not os.path.isfile(dll):
        print("[fetch-lhm] 解压后没找到 LibreHardwareMonitorLib.dll", file=sys.stderr)
        return 1
    print(f"[fetch-lhm] 完成：{dll}")
    if not os.path.isfile(exe):
        print(f"[fetch-lhm] 注意：没找到 {exe}（兜底路径将不可用）")
    print("[fetch-lhm] 之后直接运行 pc_status_gui.py（或 run_gui.bat）："
          "客户端会在自己进程内读温度/功率/显存，只留一个托盘图标。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
