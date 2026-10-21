#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PC 状态上报端（ESP-Mosaico PC 状态面板）。

采集本机 CPU / 内存 / 磁盘 / 网络等指标，通过 UDP 以 JSON 小包发送给设备。
设备端（projects/pc_display）接收后在 480x480 屏幕上显示。

只发送指标数据，不传输图像，占用极低（每包约 200 字节）。

用法：
    python pc_status_agent.py                 # 广播到本网段设备
    python pc_status_agent.py --host 192.168.1.42
    python pc_status_agent.py --interval 0.5 --verbose

GPU 占用（Windows）自动通过系统自带性能计数器获取，无需额外软件。
CPU 温度/功率/频率、显存与 GPU 温度需要 LibreHardwareMonitor（Windows 不对外
提供这些数据）：运行后保持其 Web 服务器开启，本程序启动时会自动探测
http://127.0.0.1:8085、8086、18086（任一可用即可），也可用 --lhm <url> 指定。
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import os
import platform
import re
import secrets
import struct
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.request

DEFAULT_PORT = 33340
DEFAULT_INTERVAL = 1.0

# Windows 性能计数器：每个实例是一个 (进程, 引擎) 组合。
GPU_COUNTER = r"\GPU Engine(*)\Utilization Percentage"
# typeperf 单次采样约需一个采样周期，故 GPU 刷新慢于其它指标。
GPU_IDLE_PAUSE_S = 0.25
GPU_RETRY_PAUSE_S = 5.0

try:
    import psutil
except ImportError:  # pragma: no cover
    print("缺少依赖 psutil。请先安装：\n    python -m pip install psutil", file=sys.stderr)
    sys.exit(2)


# ----------------------------------------------------------------- 采集
def _gb(value: float) -> float:
    return round(value / (1024.0 ** 3), 1)


# 本机的 4 位配对码：首次运行随机生成并保存，之后保持不变（换机器才换码）。
def app_dir() -> str:
    """配置与配对码放在哪里。

    普通脚本运行时是脚本目录；打包成 exe（PyInstaller 单文件）后是 **exe 所在目录**
    ——否则文件会写进临时解包目录，退出即丢，配对码每次都会变。
    """
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


CODE_FILE = os.path.join(app_dir(), ".pc_status_code")



def list_monitors():
    """Enumerate the Windows monitors as [{rect, width, height, primary}]."""
    import ctypes

    class RECT(ctypes.Structure):
        _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                    ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

    class MONITORINFO(ctypes.Structure):
        _fields_ = [("cbSize", ctypes.c_ulong), ("rcMonitor", RECT),
                    ("rcWork", RECT), ("dwFlags", ctypes.c_ulong)]

    monitors = []
    user32 = ctypes.windll.user32
    proc = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
                              ctypes.POINTER(RECT), ctypes.c_double)

    def _collect(hmon, hdc, rect, data):
        info = MONITORINFO()
        info.cbSize = ctypes.sizeof(MONITORINFO)
        if user32.GetMonitorInfoW(hmon, ctypes.byref(info)):
            r = info.rcMonitor
            monitors.append({
                "rect": (r.left, r.top, r.right, r.bottom),
                "width": r.right - r.left,
                "height": r.bottom - r.top,
                "primary": bool(info.dwFlags & 1),
            })
        return 1

    user32.EnumDisplayMonitors(None, None, proc(_collect), 0)
    monitors.sort(key=lambda m: (not m["primary"], m["rect"][0]))
    return monitors


def pairing_code(preferred: str = "") -> str:
    """读取或生成这台电脑的配对码。

    设备端只接受携带该码（且地址或主机名吻合）的电脑的数据，因此多台电脑同网段
    时不会互相干扰；它只用于**识别**，不是安全凭据（局域网内的显示面板）。
    @p preferred 用于 `--code 1234` 或界面里手填的码（换机时把码抄过去即可）。
    """
    if preferred:
        code = preferred.strip()
        if len(code) == 4 and code.isdigit():
            _write_code(code)
            return code

    try:
        # utf-8-sig：手动编辑过的文件可能带 BOM，普通 utf-8 会读成乱码
        with open(CODE_FILE, encoding="utf-8-sig") as handle:
            text = handle.read().strip()
        if len(text) == 4 and text.isdigit():
            return text
    except OSError:
        pass

    code = f"{secrets.randbelow(10000):04d}"
    _write_code(code)
    return code


def _write_code(code: str) -> None:
    try:
        with open(CODE_FILE, "w", encoding="utf-8") as handle:
            handle.write(code)
    except OSError:
        pass


# Interfaces that either mirror traffic already counted elsewhere (VPN/tunnel,
# virtual switches) or can never carry it (loopback). Summing them would double
# count: a download shows up on both the physical NIC and the proxy/TUN adapter.
VIRTUAL_NIC_HINTS = (
    "loopback", "vethernet", "vmware", "virtualbox", "hyper-v", "docker", "wsl",
    "tap-", "tun", "mihomo", "clash", "tailscale", "zerotier", "oray",
    "virtual", "isatap", "teredo", "bluetooth", "npcap", "wan miniport",
)
# A physical link cannot exceed this; anything beyond is a counter glitch.
MAX_PLAUSIBLE_MBPS = 1250.0      # 10 GbE


def _is_physical_nic(name: str) -> bool:
    lowered = name.lower()
    return not any(hint in lowered for hint in VIRTUAL_NIC_HINTS)


def _net_counters() -> tuple[float, float]:
    """Bytes received/sent on physical interfaces.

    `psutil.net_io_counters()` (the aggregate) sums Windows' 32-bit interface
    table and was observed reporting tens of GB/s, so the per-NIC 64-bit
    counters are summed here instead.
    """
    per_nic = psutil.net_io_counters(pernic=True)
    received = sent = 0.0
    for name, io in per_nic.items():
        if _is_physical_nic(name):
            received += io.bytes_recv
            sent += io.bytes_sent
    if received <= 0.0 and sent <= 0.0:
        # Everything matched the virtual list (e.g. VPN-only setup): fall back
        # to every interface except loopback so the panel still shows traffic.
        for name, io in per_nic.items():
            if "loopback" not in name.lower():
                received += io.bytes_recv
                sent += io.bytes_sent
    return received, sent


def _net_rates(prev: tuple[float, float, float], now: tuple[float, float, float]):
    """返回 ((下行 MB/s, 上行 MB/s), 新的基准)。"""
    t0, d0, u0 = prev
    t1, d1, u1 = now
    dt = max(t1 - t0, 1e-6)
    down = min(max(d1 - d0, 0) / dt / (1024.0 * 1024.0), MAX_PLAUSIBLE_MBPS)
    up = min(max(u1 - u0, 0) / dt / (1024.0 * 1024.0), MAX_PLAUSIBLE_MBPS)
    return (round(down, 2), round(up, 2)), now


# LibreHardwareMonitor 的 Web 服务器默认端口；本机若被占用可改（见 README），
# 这里按顺序探测，任何返回合法 JSON 的端口即视为可用。
LHM_PORT_HINTS = (8085, 8086, 18086)
LHM_URL_TEMPLATE = "http://127.0.0.1:{port}/data.json"

# LHM 把传感器归在这些分组下，其余层级都是硬件/芯片。
LHM_TYPE_GROUPS = frozenset((
    "Temperatures", "Powers", "Clocks", "Load", "Data", "Throughput", "Levels",
    "Factors", "Voltages", "Timings",
))
LHM_GPU_SENSORS = frozenset((
    "GPU Core", "GPU Hot Spot", "GPU Memory Junction", "GPU Memory Used",
    "GPU Memory Total", "D3D Dedicated Memory Total",
))


def _number(text: str):
    """拆出 LHM 数值字符串里的数字和单位，例如 "1152.0 MB" -> (1152.0, "MB")."""
    match = re.match(r"^\s*(-?[\d.,]+)\s*(.*)$", text or "")
    if match is None:
        return None, ""
    raw = match.group(1)
    if "," in raw and "." not in raw:
        raw = raw.replace(",", ".")
    try:
        return float(raw), match.group(2).strip()
    except ValueError:
        return None, ""


def _to_gb(value: float, unit: str) -> float:
    return value / 1024.0 if unit.upper().startswith("MB") else value


def _lhm_sensors(url: str):
    """抓取并拍平传感器树，返回 [(硬件, 分组, 名称, 数值, 单位)]。"""
    try:
        with urllib.request.urlopen(url, timeout=1.0) as resp:
            data = json.loads(resp.read().decode("utf-8", "replace"))
    except Exception:
        return []

    sensors: list[tuple[str, str, str, float, str]] = []

    def walk(node, depth, hardware, group):
        name = node.get("Text", "")
        children = node.get("Children") or []
        if depth == 2:          # 计算机 -> 硬件（CPU/GPU/主板/内存/硬盘…）
            hardware = name
            group = ""
        if name in LHM_TYPE_GROUPS:
            group = name
        if not children:
            value, unit = _number(node.get("Value", ""))
            if value is not None:
                sensors.append((hardware, group, name, value, unit))
            return
        for child in children:
            walk(child, depth + 1, hardware, group)

    walk(data, 0, "", "")
    return sensors


def _find(sensors, group: str, names, hardware: str | None = None):
    """按名称优先顺序取第一个传感器，返回 (硬件, 数值, 单位)。"""
    for name in names:
        for hw, sensor_group, sensor_name, value, unit in sensors:
            if sensor_group == group and sensor_name == name:
                if hardware is None or hw == hardware:
                    return hw, value, unit
    return None


def _lhm_gpu_hardware(sensors) -> str | None:
    """哪块硬件是 GPU：看它是否上报 GPU/D3D 专有传感器名称。"""
    for hardware, _group, name, _value, _unit in sensors:
        if name in LHM_GPU_SENSORS or name.startswith(("D3D ", "GPU ")):
            return hardware
    return None


def _lhm_probe() -> str | None:
    """探测本机是否运行着 LibreHardwareMonitor。"""
    for port in LHM_PORT_HINTS:
        url = LHM_URL_TEMPLATE.format(port=port)
        if _lhm_read(url):
            return url
    return None


# 便携版 LibreHardwareMonitor 的可能位置：随客户端一起放（推荐）、环境变量、
# 工作区的 tools 目录、以及它自己注册的开机启动项。
LHM_EXE_NAME = "LibreHardwareMonitor.exe"
LHM_LIB_NAME = "LibreHardwareMonitorLib.dll"
LHM_RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"

# 目标是「用户只打开我们的客户端，不用管 LHM」：找不到 Web 服务器就自动把它
# 拉起来（LHM 的配置里已设为启动即最小化到托盘）。
LHM_LAUNCH_TIMEOUT_S = 12.0


def _lhm_exe_candidates() -> list[str]:
    return [os.path.join(d, LHM_EXE_NAME) for d in _lhm_dirs()]


def _lhm_lib_candidates() -> list[str]:
    return [os.path.join(d, LHM_LIB_NAME) for d in _lhm_dirs()]


def _lhm_dirs() -> list[str]:
    """可能放着 LibreHardwareMonitor 的目录（去重、只保留存在的）。"""
    here = os.path.dirname(os.path.abspath(__file__))
    dirs = [
        os.path.dirname(os.environ.get("PC_STATUS_LHM_EXE", "")),
        os.environ.get("PC_STATUS_LHM_DIR", ""),
        os.path.join(here, "lhm"),
        os.path.join(os.environ.get("LOCALAPPDATA", ""),
                     "LibreHardwareMonitor"),
    ]
    # 工作区布局：往上找若干层的 tools/lhm（开发机上 LHM 就放在那里）。
    parent = here
    for _ in range(4):
        parent = os.path.dirname(parent)
        if not parent or parent == os.path.dirname(parent):
            break
        dirs.append(os.path.join(parent, "tools", "lhm"))
    if platform.system() == "Windows":
        try:
            import winreg

            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, LHM_RUN_KEY) as key:
                value, _ = winreg.QueryValueEx(key, "LibreHardwareMonitor")
                dirs.append(os.path.dirname(value))
        except OSError:
            pass

    # frozen-app：PyInstaller 单文件 exe 会把 --add-data 的内容解包到
    # sys._MEIPASS，LHM 库就在那里；非单文件分发时库与 exe 同目录。
    meipass = getattr(sys, "_MEIPASS", "")
    if meipass:
        dirs.append(os.path.join(meipass, "lhm"))
        dirs.append(meipass)
    if getattr(sys, "frozen", False):
        exe_dir = os.path.dirname(os.path.abspath(sys.executable))
        dirs.append(os.path.join(exe_dir, "lhm"))
        dirs.append(exe_dir)

    seen = []
    for path in dirs:
        if path and os.path.isdir(path) and path not in seen:
            seen.append(path)
    return seen


def ensure_lhm(verbose: bool = True) -> str | None:
    """返回可用的 LibreHardwareMonitor 地址；没在跑就尝试自动拉起。

    传 --lhm <url> 时由调用方直接使用该地址，不会走到这里。
    """
    url = _lhm_probe()
    if url:
        return url

    exe = next(iter(_lhm_exe_candidates()), None)
    if exe is None:
        return None
    if verbose:
        print(f"[pc-status] 正在启动 LibreHardwareMonitor: {exe}")

    try:
        subprocess.Popen([exe], cwd=os.path.dirname(exe),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         creationflags=_quiet_flags() |
                         getattr(subprocess, "DETACHED_PROCESS", 0))
    except OSError as exc:
        if verbose:
            print(f"[pc-status] 启动失败: {exc}", file=sys.stderr)
        return None

    deadline = time.monotonic() + LHM_LAUNCH_TIMEOUT_S
    while time.monotonic() < deadline:
        time.sleep(0.5)
        url = _lhm_probe()
        if url:
            if verbose:
                print(f"[pc-status] LibreHardwareMonitor 已就绪: {url}")
            return url
    return None


def _lhm_read(url: str):
    """查询 LibreHardwareMonitor 的 Web 服务器（后备路径）。"""
    return _extract_sensors(_lhm_sensors(url))


def _extract_sensors(sensors) -> dict:
    """从拍平后的传感器表挑出需要的项。

    输入是 (硬件, 分组, 名称, 数值, 单位) 列表：直读 DLL 与 Web 服务器两条路径
    都先拍平成同样形状，因此挑选逻辑只写这一份。Web 服务器把树组织成
    计算机 -> 硬件 -> 分组(Temperatures/Load/…) -> 传感器，超算芯片（如主板上的
    Nuvoton）还会多一层、传感器直接挂在芯片下面；不同厂商命名也不统一，所以按
    名称优先顺序挑选。
    """
    if not sensors:
        return {}

    out = {}

    # CPU：优先封装/SoC 温度，其次主板插座温度；不同平台命名不同。
    temp = _find(sensors, "Temperatures",
                 ("Core (Tctl/Tdie)", "CPU Package", "CPU (Tctl/Tdie)",
                  "Core Average"))
    if temp is None:
        temp = _find(sensors, "Temperatures", ("CPU",))
    if temp is not None:
        out["cpu_temp"] = round(temp[1])

    power = _find(sensors, "Powers", ("Package", "CPU Package"))
    if power is not None:
        out["cpu_power"] = round(power[1], 1)

    clock = _find(sensors, "Clocks",
                  ("Cores (Average)", "Core #1", "CPU Core #1"))
    if clock is not None:
        out["cpu_freq"] = round(clock[1])

    gpu_hw = _lhm_gpu_hardware(sensors)
    if gpu_hw is None:
        return out

    hit = _find(sensors, "Temperatures", ("GPU Core", "GPU Temperature"),
                gpu_hw)
    if hit is None:
        # AMD 独显常有 Hot Spot；集显通常没有温度传感器。
        hit = _find(sensors, "Temperatures", ("GPU Hot Spot",), gpu_hw)
    if hit is not None:
        out["gpu_temp"] = round(hit[1])

    hit = _find(sensors, "Temperatures",
                ("GPU Memory Junction", "Memory Junction", "VRAM"), gpu_hw)
    if hit is not None:
        out["gpu_vram_temp"] = round(hit[1])

    used = _find(sensors, "Data", ("GPU Memory Used", "D3D Dedicated Memory Used"),
                 gpu_hw)
    total = _find(sensors, "Data", ("D3D Dedicated Memory Total", "GPU Memory Total"),
                  gpu_hw)
    if used is not None and total is not None and total[1] > 0:
        out["gpu_mem_used"] = round(_to_gb(used[1], used[2]), 2)
        out["gpu_mem_total"] = round(_to_gb(total[1], total[2]), 1)

    # 占用：任务管理器口径。集显只报各引擎，取最忙的一个。
    hit = _find(sensors, "Load", ("GPU Core", "GPU Total", "D3D 3D"), gpu_hw)
    if hit is not None:
        out["gpu"] = round(hit[1])
    else:
        engines = [value for hw, group, name, value, _ in sensors
                   if hw == gpu_hw and group == "Load"]
        if engines:
            out["gpu"] = round(min(100.0, max(engines)))
    return out


class LhmDirect:
    """用 LibreHardwareMonitorLib.dll 在本进程内读传感器（pythonnet）。

    这样**不需要 LHM 进程、不需要 Web 服务器、也不会多一个托盘图标**：驱动
    （PawnIo 等）装在系统里，任何进程都能通过这个库读；代价是首次装载驱动需要
    一次管理员权限。读到的字段与 Web 服务器路径完全一致，两条路互为后备。
    """

    # 传感器类型 → 分组名（与 Web 服务器 JSON 的分组一致，便于复用挑选逻辑）
    GROUP_OF_TYPE = {
        "Temperature": "Temperatures",
        "Power": "Powers",
        "Clock": "Clocks",
        "Load": "Load",
        "Data": "Data",
        "SmallData": "Data",
        "Throughput": "Throughput",
    }
    # 这些类型的数值单位是 MB（与 JSON 里的 "… MB" 一致）
    MB_TYPES = {"Data", "SmallData"}

    def __init__(self, dll_path: str | None = None):
        self.available = False
        self.error = ""
        self._computer = None
        self._dll = dll_path

    def open(self) -> bool:
        dll = self._dll or next(iter(_lhm_lib_candidates()), None)
        if dll is None:
            self.error = "找不到 LibreHardwareMonitorLib.dll"
            return False
        try:
            import clr                        # pythonnet

            clr.AddReference(dll)
            from LibreHardwareMonitor.Hardware import Computer

            computer = Computer()
            computer.IsCpuEnabled = True
            computer.IsGpuEnabled = True
            computer.IsMotherboardEnabled = True
            computer.IsControllerEnabled = True
            computer.Open()
        except Exception as exc:              # 任何失败都请调用方退回后备路径
            self.error = f"{type(exc).__name__}: {exc}"
            return False

        self._computer = computer
        self.available = True
        return True

    def close(self) -> None:
        if self._computer is not None:
            try:
                self._computer.Close()
            except Exception:                 # noqa: BLE001
                pass
            self._computer = None
        self.available = False

    def read(self) -> dict:
        if not self.available:
            return {}
        sensors: list[tuple[str, str, str, float, str]] = []
        for hardware in self._computer.Hardware:
            self._collect(hardware, sensors)
        return _extract_sensors(sensors)

    def _collect(self, hardware, out) -> None:
        hardware.Update()
        name = hardware.Name
        for sensor in hardware.Sensors:
            if sensor.Value is None:
                continue
            kind = str(sensor.SensorType)
            group = self.GROUP_OF_TYPE.get(kind, kind)
            unit = "MB" if kind in self.MB_TYPES else ""
            out.append((name, group, sensor.Name, float(sensor.Value), unit))
        for sub in hardware.SubHardware:
            self._collect(sub, out)



def _busiest_engine(header: list[str], row: list[str]) -> int | None:
    """把一行 PDH 采样按引擎类型汇总，返回最忙引擎的占用率（0-100）。

    `\\GPU Engine(*)` 会为每个 (进程, 引擎) 各给一列，单看一列没有意义；
    按引擎类型求和后取最大值，与任务管理器的口径基本一致。
    """
    totals: dict[str, float] = {}
    for column, value in zip(header[1:], row[1:]):
        match = re.search(r"engtype_([^)]+)", column)
        if match is None:
            continue
        try:
            totals[match.group(1)] = totals.get(match.group(1), 0.0) + float(value)
        except ValueError:
            continue
    if not totals:
        return None
    return int(min(100.0, max(totals.values())) + 0.5)


class DeviceIdentity:
    """设备自我介绍（见设备端 pc_link_set_identity）。

    设备收到状态包后会回一个身份小包，PC 端据此显示「已连接的设备名称」；
    它同时也是真正的连接指示灯：收到过回执才说明设备确实在收包。
    `paired` 表示设备是否已绑定本机的配对码（绑定后设备才显示本机数据）。
    """

    def __init__(self) -> None:
        self.name = ""
        self.model = ""
        self.firmware = ""
        self.ip = ""
        self.paired = False
        self.seen = 0.0                      # 最近一次收到回执的时刻

    def update(self, payload: dict) -> bool:
        """吸收一条回执，返回信息是否有变化。"""
        changed = False
        for key, attr in (("name", "name"), ("model", "model"),
                          ("fw", "firmware"), ("ip", "ip")):
            value = payload.get(key)
            if isinstance(value, str) and value and getattr(self, attr) != value:
                setattr(self, attr, value)
                changed = True
        paired = payload.get("paired")
        if isinstance(paired, bool) and paired != self.paired:
            self.paired = paired
            changed = True
        self.seen = time.monotonic()
        return changed

    def alive(self, timeout_s: float = 10.0) -> bool:
        return bool(self.seen) and (time.monotonic() - self.seen) < timeout_s

    def age_s(self) -> float:
        return time.monotonic() - self.seen if self.seen else -1.0

    def label(self) -> str:
        """一行展示，例如 "PC 状态面板 · ESP-Mosaico · 1.0.0 @ 192.168.1.42"。"""
        if not self.name:
            return ""
        bits = [self.name]
        if self.model:
            bits.append(self.model)
        if self.firmware:
            bits.append(self.firmware)
        text = " · ".join(bits)
        if self.ip:
            text += f" @ {self.ip}"
        return text


def drain_identity(sock, identity: DeviceIdentity) -> bool:
    """读取设备回执（非阻塞），返回是否有信息更新。"""
    changed = False
    while True:
        try:
            data, _source = sock.recvfrom(512)
        except (BlockingIOError, OSError):
            return changed
        try:
            payload = json.loads(data.decode("utf-8", "replace"))
        except ValueError:
            continue
        if isinstance(payload, dict) and payload.get("dev"):
            changed = identity.update(payload) or changed


# 子进程不要弹黑框：打包成 exe（无控制台）后，每次调用都会新开一个控制台窗口，
# 而 GPU 采样每两秒调用一次 typeperf —— 不隐藏的话就会不停闪黑框。
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)


def _quiet_flags() -> int:
    return CREATE_NO_WINDOW if platform.system() == "Windows" else 0


class GpuUsage:
    """Windows GPU 占用率，使用系统自带的 typeperf，无需第三方软件。

    typeperf 单次采样约需两秒，因此在后台线程中循环采样、缓存最近结果；
    上报时只读缓存，不阻塞主循环（GPU 数值约每两秒刷新一次）。
    """

    def __init__(self):
        self._value: int | None = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._exe = shutil.which("typeperf") if platform.system() == "Windows" else None

    @property
    def available(self) -> bool:
        return self._exe is not None

    def start(self) -> None:
        if not self.available or self._thread is not None:
            return
        self._thread = threading.Thread(target=self._loop, name="gpu-pdh", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=3.0)
            self._thread = None

    def value(self) -> int | None:
        with self._lock:
            return self._value

    def _loop(self) -> None:
        while not self._stop.is_set():
            value = self._sample_once()
            if value is not None:
                with self._lock:
                    self._value = value
            self._stop.wait(GPU_IDLE_PAUSE_S if value is not None else GPU_RETRY_PAUSE_S)

    def _sample_once(self) -> int | None:
        try:
            proc = subprocess.run([self._exe, GPU_COUNTER, "-sc", "1"],
                                  capture_output=True, text=True, timeout=15,
                                  creationflags=_quiet_flags())
        except (OSError, subprocess.SubprocessError):
            return None
        if proc.returncode != 0:
            return None
        rows = [row for row in csv.reader(io.StringIO(proc.stdout)) if row]
        if len(rows) < 2:
            return None
        header = rows[0]
        # typeperf 之后还会输出中文汇总行，按列数过滤出真正的数据行。
        data = [row for row in rows[1:] if len(row) == len(header)]
        if not data:
            return None
        return _busiest_engine(header, data[-1])


class Collector:
    def __init__(self, lhm_url: str | None, gpu: GpuUsage | None = None,
                 direct: "LhmDirect | None" = None, code: str = ""):
        self.lhm_url = lhm_url
        self.direct = direct
        self.gpu = gpu
        self.code = code
        self._net = (time.monotonic(), *self._net_counters())
        psutil.cpu_percent(interval=None)  # 预热，首次非零
        self.hostname = socket.gethostname()
        self.os_name = self._os_name()

    @staticmethod
    def _net_counters():
        io = psutil.net_io_counters()
        return io.bytes_recv, io.bytes_sent

    @staticmethod
    def _os_name() -> str:
        if platform.system() == "Windows":
            rel = platform.release()
            return f"Windows {rel}"
        return f"{platform.system()} {platform.release()}"

    def sample(self) -> dict:
        now = (time.monotonic(), *self._net_counters())
        (down, up), self._net = _net_rates(self._net, now)

        mem = psutil.virtual_memory()
        payload = {
            "v": 2,
            "code": self.code,      # 设备只接受与自身配对码一致的数据
            "host": self.hostname,
            "os": self.os_name,
            "cpu": round(psutil.cpu_percent(interval=None)),
            "mem_used": _gb(mem.used),
            "mem_total": _gb(mem.total),
            "net_down": down,
            "net_up": up,
            "uptime": int(time.time() - psutil.boot_time()),
            "ts": int(time.time()),
        }

        # CPU 温度：psutil 在 Linux 上可用；Windows 上通常为空，由 LHM 补充。
        temp = None
        try:
            sensors = psutil.sensors_temperatures()
            for key in ("coretemp", "k10temp", "cpu_thermal", "acpitz"):
                if sensors.get(key):
                    temp = sensors[key][0].current
                    break
        except (AttributeError, OSError):
            pass
        if temp is not None:
            payload["cpu_temp"] = round(temp)

        # GPU 占用（Windows 性能计数器；采样在后台线程完成，这里只读缓存）
        if self.gpu is not None:
            gpu_load = self.gpu.value()
            if gpu_load is not None:
                payload["gpu"] = gpu_load

        if self.direct is not None and self.direct.available:
            lhm = self.direct.read()
        elif self.lhm_url:
            lhm = _lhm_read(self.lhm_url)
        else:
            lhm = {}
        if lhm:
            # 补充温度/功率/频率/显存；已有的实时值（如 GPU 占用）不被覆盖。
            payload.update({k: v for k, v in lhm.items() if k not in payload})

        return payload


# ----------------------------------------------------------------- 发送
def make_sender(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    # 非阻塞：发送后顺带收设备回执，不拖慢上报节奏。
    sock.setblocking(False)
    if args.host:
        targets = [(args.host, args.port)]
    else:
        targets = [("255.255.255.255", args.port), (subnet_broadcast(), args.port)]
    return sock, targets


def subnet_broadcast() -> str:
    try:
        hostname = socket.gethostname()
        local = socket.gethostbyname(hostname)
        octets = local.split(".")
        if len(octets) == 4 and not local.startswith("127."):
            return ".".join(octets[:3]) + ".255"
    except OSError:
        pass
    return "255.255.255.255"


def main() -> int:
    parser = argparse.ArgumentParser(description="ESP-Mosaico PC 状态上报端")
    parser.add_argument("--host", help="设备 IP；省略则广播到本网段")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--interval", type=float, default=DEFAULT_INTERVAL,
                        help="上报间隔秒数，默认 1.0")
    parser.add_argument("--lhm", nargs="?", const="",
                        help="LibreHardwareMonitor 数据地址（用于温度/功率/显存）；"
                             "不带值时自动探测")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--count", type=int, default=0, help="发送次数后退出（0=持续）")
    parser.add_argument("--code", default="",
                        help="固定使用这个 4 位配对码（默认随机生成并保存到 .pc_status_code）")
    args = parser.parse_args()

    # 传感器：优先进程内直读 LibreHardwareMonitorLib.dll（不需要 LHM 进程、
    # 不占托盘）；失败才退回 Web 服务器（必要时自动拉起 LHM）。
    direct = LhmDirect()
    lhm_url = None
    source = "无"
    if direct.open():
        source = f"进程内直读（{os.path.basename(direct._dll or LHM_LIB_NAME)}）"
    else:
        lhm_url = args.lhm or ensure_lhm(verbose=False)
        if lhm_url:
            source = f"LibreHardwareMonitor Web 服务器（{lhm_url}）"

    gpu = GpuUsage()
    gpu.start()

    collector = Collector(lhm_url, gpu, direct if direct.available else None,
                          pairing_code(args.code))
    sock, targets = make_sender(args)
    print(f"[pc-status] 主机 {collector.hostname} ({collector.os_name})")
    print(f"[pc-status] 目标 {', '.join(f'{h}:{p}' for h, p in targets)}，间隔 {args.interval}s")
    print(f"[pc-status] 配对码 {collector.code}  ← 在设备「设备配对」页输入这 4 位数")
    print(f"[pc-status] GPU 占用: "
          f"{'typeperf 性能计数器' if gpu.available else '不可用（仅 Windows 支持）'}")
    print(f"[pc-status] 温度/功率/显存: {source}")
    if source == "无":
        print(f"[pc-status] （直读失败原因: {direct.error}；"
              "可运行 python fetch_lhm.py 获取 DLL）", file=sys.stderr)

    sent = 0
    identity = DeviceIdentity()
    last_label = ""
    last_paired = None
    try:
        while True:
            payload = collector.sample()
            data = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            for host, port in targets:
                try:
                    sock.sendto(data, (host, port))
                except OSError as exc:
                    print(f"[pc-status] 发送到 {host}失败: {exc}", file=sys.stderr)
            if drain_identity(sock, identity):
                last_label = identity.label()
                print(f"[pc-status] 已连接设备: {last_label}")
            if identity.paired != last_paired:
                last_paired = identity.paired
                if last_paired:
                    print(f"[pc-status] 设备已绑定本机配对码 {collector.code}，"
                          "切换为正常上报节奏")
                else:
                    print("[pc-status] 设备尚未绑定本机：请在设备上输入配对码 "
                          f"{collector.code}（当前按 2s 间隔试探上报）")
            sent += 1
            if args.verbose or sent == 1:
                print(f"[pc-status] {payload}")
            if args.count and sent >= args.count:
                break
            # 未配对的电脑降频发包：够设备发现配对码即可，不抢占显示。
            time.sleep(args.interval if identity.paired
                       else max(2.0, args.interval))
    except KeyboardInterrupt:
        print("\n[pc-status] 已停止")
    finally:
        if last_label:
            print(f"[pc-status] 最后连接的设备: {last_label}")
        direct.close()
        gpu.stop()
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())


# --------------------------------------------------------------- mirror
# The panel's third root page shows the desktop. Frames travel on their own
# UDP port so the status stream keeps its own (tiny) cadence.
MIRROR_PORT = 33341
MIRROR_WIDTH = 480
MIRROR_HEIGHT = 420
MIRROR_CHUNK = 1200
MIRROR_FPS = 8
MIRROR_MAX_BYTES = 128 * 1024


class MirrorSender:
    """Stream this desktop to the panel (480x420, letterboxed, JPEG, chunked).

    Wire format (little endian): 'P' 'C' | type | frame_id | index | count |
    payload_len, followed by up to MIRROR_CHUNK payload bytes. The device
    reassembles frames and hands the newest one to ESP-GSP for decoding.
    """

    def __init__(self, host, port=MIRROR_PORT, fps=MIRROR_FPS, quality=70,
                 bbox=None):
        self.host = host
        self.port = port
        self.fps = max(1, min(30, int(fps)))
        self.quality = quality
        self.bbox = bbox          # (left, top, right, bottom) or None
        self.sent_frames = 0
        self.sent_bytes = 0
        self.last_error = ""
        self._thread = None
        self._stop = threading.Event()

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="mirror", daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        thread = self._thread
        self._thread = None
        if thread:
            thread.join(timeout=2.0)

    @property
    def running(self):
        return bool(self._thread and self._thread.is_alive())

    def _encode(self, shot):
        from PIL import Image
        import io as _io

        shot = shot.convert("RGB")
        shot.thumbnail((MIRROR_WIDTH, MIRROR_HEIGHT), Image.BILINEAR)
        canvas = Image.new("RGB", (MIRROR_WIDTH, MIRROR_HEIGHT), (0, 0, 0))
        canvas.paste(shot, ((MIRROR_WIDTH - shot.width) // 2,
                            (MIRROR_HEIGHT - shot.height) // 2))
        quality = self.quality
        while True:
            buffer = _io.BytesIO()
            canvas.save(buffer, "JPEG", quality=quality)
            data = buffer.getvalue()
            if len(data) <= MIRROR_MAX_BYTES or quality <= 30:
                return data
            quality -= 10

    def _run(self):
        from PIL import ImageGrab

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        frame_id = 0
        try:
            while not self._stop.is_set():
                started = time.monotonic()
                try:
                    # Capture one monitor: an ultrawide / multi-monitor
                    # desktop would letterbox down to a thin strip.
                    shot = (ImageGrab.grab(bbox=self.bbox) if self.bbox
                            else ImageGrab.grab())
                    data = self._encode(shot)
                except Exception as exc:            # screen locked, DPI change…
                    self.last_error = str(exc)
                    time.sleep(0.5)
                    continue
                count = max(1, (len(data) + MIRROR_CHUNK - 1) // MIRROR_CHUNK)
                frame_id = (frame_id + 1) & 0xFFFF
                sent = True
                for index in range(count):
                    chunk = data[index * MIRROR_CHUNK:(index + 1) * MIRROR_CHUNK]
                    header = struct.pack("<2sBHHHH", b"PC", 0, frame_id,
                                         index, count, len(chunk))
                    try:
                        sock.sendto(header + chunk, (self.host, self.port))
                    except OSError as exc:
                        self.last_error = str(exc)
                        sent = False
                        break
                if sent:
                    self.sent_frames += 1
                    self.sent_bytes += len(data)
                    self.last_error = ""
                elapsed = time.monotonic() - started
                self._stop.wait(max(0.0, 1.0 / self.fps - elapsed))
        finally:
            sock.close()
