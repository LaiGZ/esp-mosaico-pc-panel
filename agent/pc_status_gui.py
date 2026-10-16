#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PC 状态面板的图形界面上报端。

在命令行上报程序（pc_status_agent.py）之上加一个窗口：实时显示本机指标、
设备连接状态和发送计数，可随时开始/停止上报。采集与发送逻辑全部复用
pc_status_agent，界面只是它的一个前端。

用法：
    python pc_status_gui.py                  # 读取上次的目标地址
    python pc_status_gui.py --host 192.168.1.42
    python pc_status_gui.py --lhm http://127.0.0.1:18086/data.json

设置（目标地址、端口、间隔）保存在脚本同目录的 .pc_status_gui.json。
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox

# 打包成 --windowed 的 exe 后没有控制台，print 会因为 sys.stdout 为 None 而报错。
if sys.stdout is None:
    sys.stdout = open(os.devnull, "w", encoding="utf-8")
    sys.stderr = sys.stdout

try:
    from pc_status_agent import (Collector, DeviceIdentity, GpuUsage, LhmDirect,
                                 app_dir, drain_identity, ensure_lhm, pairing_code)
except ImportError:  # pragma: no cover - 依赖缺失时给出可执行的提示
    print("缺少依赖或找不到 pc_status_agent.py。请先安装：\n"
          "    python -m pip install psutil", file=sys.stderr)
    raise

# 与设备界面一致的深色配色。
BG = "#070A0F"
CARD = "#0E151D"
CARD_TOP = "#121A23"
BORDER = "#1E2A36"
FIELD = "#101820"
TEXT = "#E7EEF5"
MUTED = "#8A9AAB"
TRACK = "#1C2732"
CPU = "#FF4C01"
MEM = "#22D3EE"
GPU = "#A78BFA"
NET_DOWN = "#22D3EE"
NET_UP = "#C4B5FD"
OK = "#2BD97B"
BAD = "#FF4D4D"

FONT = "Microsoft YaHei UI"
# 设置文件放在 exe（或脚本）旁边，打包后不会随临时解包目录消失。
SETTINGS = os.path.join(app_dir(), ".pc_status_gui.json")


def _text(value: str) -> tuple:
    return (FONT, value)


def _tray_image(colour: str, size: int = 64):
    """托盘图标：深色圆角底 + 一个状态点（绿=已连接，红=未连接）。"""
    from PIL import Image, ImageDraw

    red, green, blue = (int(colour[i:i + 2], 16) for i in (1, 3, 5))
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((2, 2, size - 3, size - 3), radius=14,
                           fill=(14, 21, 29, 255), outline=(30, 42, 54, 255),
                           width=2)
    draw.ellipse((size // 2 - 11, size // 2 - 11, size // 2 + 11, size // 2 + 11),
                 fill=(red, green, blue, 255))
    return image


class Bar(tk.Canvas):
    """细长的进度条，和设备卡片上的样式一致。"""

    def __init__(self, master, accent: str, height: int = 8):
        super().__init__(master, height=height, bg=CARD, highlightthickness=0,
                         bd=0)
        self._accent = accent
        self._fraction = 0.0
        self._track = self.create_rectangle(0, 0, 0, 0, fill=TRACK, width=0)
        self._fill = self.create_rectangle(0, 0, 0, 0, fill=accent, width=0)
        self.bind("<Configure>", lambda _event: self._redraw())

    def set(self, fraction: float) -> None:
        self._fraction = max(0.0, min(1.0, fraction))
        self._redraw()

    def _redraw(self) -> None:
        width = max(self.winfo_width(), 1)
        height = max(self.winfo_height(), 1)
        self.coords(self._track, 0, 0, width, height)
        self.coords(self._fill, 0, 0, int(width * self._fraction), height)


class Card(tk.Frame):
    """一张指标卡片：标题、主数值、进度条和一行补充说明。"""

    def __init__(self, master, title: str, accent: str):
        super().__init__(master, bg=CARD, highlightbackground=BORDER,
                         highlightthickness=1)
        self._accent = accent

        head = tk.Frame(self, bg=CARD)
        head.pack(fill="x", padx=14, pady=(12, 0))
        tk.Label(head, text=title, bg=CARD, fg=MUTED, font=_text(11)).pack(side="left")
        self.badge = tk.Label(head, text="", bg=CARD, fg=accent, font=_text(11))
        self.badge.pack(side="right")

        body = tk.Frame(self, bg=CARD)
        body.pack(fill="x", padx=14, pady=(2, 0))
        self.value = tk.Label(body, text="--", bg=CARD, fg=TEXT,
                              font=(FONT, 30, "bold"))
        self.value.pack(side="left")
        self.unit = tk.Label(body, text="", bg=CARD, fg=MUTED, font=(FONT, 12))
        self.unit.pack(side="left", padx=(6, 0), pady=(12, 0))

        self.bar = Bar(self, accent)
        self.bar.pack(fill="x", padx=14, pady=(6, 0))
        self.note = tk.Label(self, text="", bg=CARD, fg=MUTED, font=_text(10),
                             anchor="w")
        self.note.pack(fill="x", padx=14, pady=(6, 12))

    def show(self, value: str, unit: str = "", fraction: float = 0.0,
             note: str = "", badge: str = "") -> None:
        self.value.configure(text=value)
        self.unit.configure(text=unit)
        self.bar.set(fraction)
        self.note.configure(text=note)
        self.badge.configure(text=badge, fg=self._accent)


class AgentApp:
    def __init__(self, root: tk.Tk, host: str, port: int, interval: float,
                 source_label: str, direct: "LhmDirect | None",
                 saved_code: str = ""):
        self.root = root
        self.source_label = source_label
        self.direct = direct
        self.gpu = GpuUsage()
        self.gpu.start()
        self.code = pairing_code(saved_code)
        self.collector = Collector(None, self.gpu, direct, self.code)
        self.interval = interval

        self._lock = threading.Lock()
        self._latest: dict | None = None
        self._sent = 0
        self._error = ""
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self.identity = DeviceIdentity()

        # 托盘：关闭窗口 = 缩到托盘继续上报，只有托盘里一个小图标。
        self.tray = None
        self._want_show = False
        self._want_quit = False
        self._tray_notified = False

        self._build(host, port)
        self._start_tray()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(250, self._tick)

    # ---------------------------------------------------------------- 托盘
    def _start_tray(self) -> None:
        try:
            import pystray

            self.tray = pystray.Icon(
                "pc_status", _tray_image(BAD), "PC 状态面板",
                pystray.Menu(
                    pystray.MenuItem("显示窗口", self._tray_show, default=True),
                    pystray.MenuItem("退出", self._tray_quit),
                ))
            self.tray.run_detached()
        except Exception:                     # 没有 pystray 就退回到“关闭即退出”
            self.tray = None

    def _tray_show(self, *_args) -> None:
        self._want_show = True                # 由 Tk 线程的 tick 真正执行

    def _tray_quit(self, *_args) -> None:
        self._want_quit = True

    def _hide_to_tray(self) -> None:
        self.root.withdraw()
        if self.tray is not None and not self._tray_notified:
            self._tray_notified = True
            try:
                self.tray.notify("仍在后台向设备上报，双击托盘图标可重新打开。",
                                 "PC 状态面板")
            except Exception:                 # 通知失败不影响使用
                pass

    # ---------------------------------------------------------------- 界面
    def _build(self, host: str, port: int) -> None:
        root = self.root
        root.title("PC 状态面板 · 上报端")
        root.configure(bg=BG)
        root.minsize(560, 660)

        head = tk.Frame(root, bg=BG)
        head.pack(fill="x", padx=20, pady=(18, 6))
        tk.Label(head, text=self.collector.hostname, bg=BG, fg=TEXT,
                 font=(FONT, 18, "bold")).pack(anchor="w")
        tk.Label(head, text=f"{self.collector.os_name} · 本机上报端",
                 bg=BG, fg=MUTED, font=_text(10)).pack(anchor="w")

        # 已连接设备 / 连接信息：名称来自设备回执，收到回执才说明设备真的在收包。
        link = tk.Frame(root, bg=CARD, highlightbackground=BORDER,
                        highlightthickness=1)
        link.pack(fill="x", padx=20, pady=(10, 0))
        top = tk.Frame(link, bg=CARD)
        top.pack(fill="x", padx=14, pady=(10, 0))
        self.link_dot = tk.Canvas(top, width=12, height=12, bg=CARD,
                                  highlightthickness=0, bd=0)
        self.link_dot.pack(side="left", padx=(0, 8))
        self.link_shape = self.link_dot.create_oval(1, 1, 11, 11, fill=BAD,
                                                    width=0)
        self.link_label = tk.Label(top, text="未连接设备", bg=CARD, fg=TEXT,
                                   font=(FONT, 11, "bold"), anchor="w")
        self.link_label.pack(side="left")
        self.link_detail = tk.Label(link, text="", bg=CARD, fg=MUTED,
                                    font=_text(9), anchor="w", justify="left")
        self.link_detail.pack(fill="x", padx=(34, 14), pady=(2, 0))
        self.link_local = tk.Label(link, text="", bg=CARD, fg=MUTED,
                                   font=_text(9), anchor="w", justify="left")
        self.link_local.pack(fill="x", padx=(34, 14), pady=(2, 0))
        # 配对码要显眼：设备那边就靠它认出这台电脑。
        self.pair_label = tk.Label(link, text="", bg=CARD, fg=OK,
                                   font=(FONT, 11, "bold"), anchor="w",
                                   justify="left")
        self.pair_label.pack(fill="x", padx=(34, 14), pady=(4, 10))

        cards = tk.Frame(root, bg=BG)
        cards.pack(fill="both", expand=True, padx=20, pady=(10, 0))
        cards.columnconfigure(0, weight=1, uniform="card")
        cards.columnconfigure(1, weight=1, uniform="card")

        self.card_cpu = Card(cards, "CPU", CPU)
        self.card_mem = Card(cards, "内存", MEM)
        self.card_gpu = Card(cards, "GPU", GPU)
        self.card_net = Card(cards, "网络", NET_DOWN)
        self.card_cpu.grid(row=0, column=0, sticky="nsew", padx=(0, 8), pady=(0, 8))
        self.card_mem.grid(row=0, column=1, sticky="nsew", padx=(8, 0), pady=(0, 8))
        self.card_gpu.grid(row=1, column=0, sticky="nsew", padx=(0, 8), pady=(8, 0))
        self.card_net.grid(row=1, column=1, sticky="nsew", padx=(8, 0), pady=(8, 0))

        target = tk.Frame(root, bg=BG)
        target.pack(fill="x", padx=20, pady=(16, 0))
        tk.Label(target, text="设备地址", bg=BG, fg=MUTED,
                 font=_text(10)).pack(side="left")
        self.var_host = tk.StringVar(value=host)
        tk.Entry(target, textvariable=self.var_host, width=18, bg=FIELD, fg=TEXT,
                 insertbackground=TEXT, relief="flat", font=_text(11)).pack(
                     side="left", padx=(8, 4), ipady=4)
        tk.Label(target, text="端口", bg=BG, fg=MUTED,
                 font=_text(10)).pack(side="left", padx=(8, 0))
        self.var_port = tk.StringVar(value=str(port))
        tk.Entry(target, textvariable=self.var_port, width=7, bg=FIELD, fg=TEXT,
                 insertbackground=TEXT, relief="flat", font=_text(11)).pack(
                     side="left", padx=(6, 0), ipady=4)

        buttons = tk.Frame(root, bg=BG)
        buttons.pack(fill="x", padx=20, pady=(14, 0))
        self.btn_toggle = tk.Button(buttons, text="开始上报", command=self._toggle,
                                    bg=CARD_TOP, fg=TEXT, activebackground=BORDER,
                                    activeforeground=TEXT, relief="flat",
                                    font=(FONT, 11, "bold"), padx=22, pady=7,
                                    cursor="hand2")
        self.btn_toggle.pack(side="left")
        tk.Label(buttons, text=f"间隔 {self.interval:g}s", bg=BG, fg=MUTED,
                 font=_text(10)).pack(side="left", padx=(12, 0))

        self.status = tk.Label(root, text="未开始", bg=BG, fg=MUTED,
                               font=_text(10), anchor="w", justify="left")
        self.status.pack(fill="x", padx=20, pady=(10, 4))
        self.sensors = tk.Label(root, text=self._sensor_summary(), bg=BG,
                                fg=MUTED, font=_text(9), anchor="w",
                                justify="left")
        self.sensors.pack(fill="x", padx=20, pady=(0, 16))

    def _sensor_summary(self) -> str:
        gpu = ("GPU 占用: typeperf 性能计数器" if self.gpu.available
               else "GPU 占用: 不可用（仅 Windows 支持）")
        return f"{gpu}\n温度/功率/显存: {self.source_label}"

    # ---------------------------------------------------------------- 上报
    def _toggle(self) -> None:
        if self._thread and self._thread.is_alive():
            self._stop.set()
            self.btn_toggle.configure(text="开始上报")
            self._set_status("已停止", MUTED)
            return

        host = self.var_host.get().strip()
        try:
            port = int(self.var_port.get().strip())
            assert 0 < port < 65536
        except (ValueError, AssertionError):
            messagebox.showerror("地址无效", "端口需要是 1-65535 之间的数字。")
            return
        if not host:
            messagebox.showerror("地址无效", "请填写设备 IP（设备屏幕上显示的本机地址）。")
            return

        self._save_settings(host, port)
        self._stop.clear()
        self._sent = 0
        self._error = ""
        self.identity = DeviceIdentity()      # 换了地址就是别的设备
        self._thread = threading.Thread(target=self._run, args=(host, port),
                                        name="sender", daemon=True)
        self._thread.start()
        self.btn_toggle.configure(text="停止上报")
        self._set_status(f"正在上报到 {host}:{port} …", OK)

    def _run(self, host: str, port: int) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.setblocking(False)      # 顺带收设备回执，不让它拖慢上报
        try:
            while not self._stop.is_set():
                payload = self.collector.sample()
                try:
                    sock.sendto(json.dumps(payload, separators=(",", ":")).encode("utf-8"),
                                (host, port))
                    error = ""
                except OSError as exc:
                    error = str(exc)
                drain_identity(sock, self.identity)
                with self._lock:
                    self._latest = payload
                    self._error = error
                    if not error:
                        self._sent += 1
                # 未配对的电脑降频上报：够设备发现配对码即可，不争显示。
                self._stop.wait(self.interval if self.identity.paired
                                else max(2.0, self.interval))
        finally:
            sock.close()

    def _set_status(self, text: str, colour: str) -> None:
        self.status.configure(text=text, fg=colour)

    # ---------------------------------------------------------------- 刷新
    def _tick(self) -> None:
        if self._want_quit:
            self._want_quit = False
            self._quit()
            return
        if self._want_show:
            self._want_show = False
            self.root.deiconify()
            self.root.lift()
            try:
                self.root.focus_force()
            except tk.TclError:
                pass

        with self._lock:
            payload = self._latest
            sent = self._sent
            error = self._error

        if payload is not None:
            self._render(payload)
        self._render_link(sent)
        self._refresh_tray()
        if self._thread and self._thread.is_alive():
            if error:
                self._set_status(f"已发送 {sent} 包 · 发送失败: {error}", BAD)
            else:
                self._set_status(
                    f"已发送 {sent} 包 · 最后更新 {time.strftime('%H:%M:%S')}", OK)
        self.root.after(250, self._tick)

    def _refresh_tray(self) -> None:
        """托盘图标用颜色表示连接状态，提示文字带上设备名。"""
        if self.tray is None:
            return
        alive = self.identity.alive()
        try:
            self.tray.icon = _tray_image(OK if alive else BAD)
            name = self.identity.name or "未连接设备"
            self.tray.title = f"PC 状态面板 · {name}"
        except Exception:                     # 托盘不可用时静默
            pass

    def _render_link(self, sent: int = 0) -> None:
        """设备名称来自设备回执；超时未收到就说明设备没在收包。"""
        target = f"{self.var_host.get().strip()}:{self.var_port.get().strip()}"
        alive = self.identity.alive()
        known = bool(self.identity.name)
        if alive and known:
            self.link_dot.itemconfigure(self.link_shape, fill=OK)
            self.link_label.configure(text="已连接设备：" + self.identity.name,
                                      fg=TEXT)
            detail = " · ".join(x for x in (
                ("型号 " + self.identity.model) if self.identity.model else "",
                ("固件 " + self.identity.firmware) if self.identity.firmware else "",
                ("设备 IP " + self.identity.ip) if self.identity.ip else "",
                f"最后响应 {self.identity.age_s():.1f} 秒前"))
            self.link_detail.configure(text=detail)
        elif known:
            self.link_dot.itemconfigure(self.link_shape, fill=BAD)
            self.link_label.configure(text="设备无响应：" + self.identity.name,
                                      fg=BAD)
            self.link_detail.configure(
                text=f"最后响应 {self.identity.age_s():.0f} 秒前 · "
                     "检查设备是否开机、是否和本机在同一局域网")
        else:
            self.link_dot.itemconfigure(self.link_shape, fill=BAD)
            self.link_label.configure(text="未连接设备", fg=MUTED)
            self.link_detail.configure(text="设备收到数据后会回报自己的名称与型号")

        self.link_local.configure(
            text=f"本机 {self.collector.hostname}（{self.collector.os_name}）"
                 f" → 目标 {target} · 已发送 {sent} 包 · 间隔 {self.interval:g}s")

        if alive and self.identity.paired:
            self.pair_label.configure(
                text=f"配对码 {self.code} · 设备已绑定本机，正在显示本机数据",
                fg=OK)
        elif alive:
            self.pair_label.configure(
                text=f"配对码 {self.code} · 请在设备上输入这 4 位完成配对",
                fg="#FFB020")
        else:
            self.pair_label.configure(
                text=f"配对码 {self.code}（设备离线，无需处理）", fg=MUTED)

    def _render(self, p: dict) -> None:
        cpu = int(p.get("cpu", 0))
        badge = f"{p['cpu_temp']}°C" if "cpu_temp" in p else ""
        bits = []
        if "cpu_temp" in p:
            bits.append(f"温度 {p['cpu_temp']}°C")
        if "cpu_power" in p:
            bits.append(f"功率 {p['cpu_power']:.0f}W")
        if "cpu_freq" in p:
            bits.append(f"{p['cpu_freq'] / 1000:.2f}GHz")
        self.card_cpu.show(str(cpu), "%", cpu / 100,
                           " · ".join(bits) or "无传感器数据", badge)

        used, total = p.get("mem_used", 0.0), p.get("mem_total", 0.0)
        percent = (used / total) if total else 0.0
        self.card_mem.show(f"{used:.1f}", f"/ {total:.0f} GB", percent,
                           f"已用 {percent * 100:.0f}%")

        gpu = p.get("gpu")
        badge = f"{p['gpu_temp']}°C" if "gpu_temp" in p else ""
        notes = []
        if "gpu_mem_used" in p and "gpu_mem_total" in p:
            notes.append(f"显存 {p['gpu_mem_used']:.2f} / {p['gpu_mem_total']:.1f} GB")
        if "gpu_vram_temp" in p:
            notes.append(f"显存温度 {p['gpu_vram_temp']}°C")
        self.card_gpu.show("--" if gpu is None else str(int(gpu)), "%",
                           0.0 if gpu is None else gpu / 100,
                           " · ".join(notes) or ("无 GPU 数据" if gpu is None
                                                 else "无显存数据"),
                           badge)

        down, up = p.get("net_down", 0.0), p.get("net_up", 0.0)
        self.card_net.show(f"{down:.1f}", "MB/s ↓", min(down / 100.0, 1.0),
                           f"上传 {up:.1f} MB/s")

    # ---------------------------------------------------------------- 其他
    def _save_settings(self, host: str, port: int) -> None:
        try:
            with open(SETTINGS, "w", encoding="utf-8") as fh:
                json.dump({"host": host, "port": port, "interval": self.interval,
                           "code": self.code},
                          fh)
        except OSError:
            pass    # 设置保存失败不影响上报

    def _on_close(self) -> None:
        """关闭窗口：有托盘就缩到托盘继续上报，否则退出。"""
        if self.tray is not None:
            self._hide_to_tray()
            return
        self._quit()

    def _quit(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)
        if self.direct is not None:
            self.direct.close()
        self.gpu.stop()
        if self.tray is not None:
            try:
                self.tray.stop()
            except Exception:
                pass
            self.tray = None
        self.root.destroy()


def _load_settings() -> dict:
    try:
        # utf-8-sig：用记事本编辑过的设置文件可能带 BOM，普通 utf-8 会解析失败
        with open(SETTINGS, encoding="utf-8-sig") as fh:
            data = json.load(fh)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def main() -> int:
    saved = _load_settings()
    parser = argparse.ArgumentParser(description="PC 状态面板图形界面上报端")
    parser.add_argument("--host", default=saved.get("host", ""),
                        help="设备 IP；留空可在窗口里填写")
    parser.add_argument("--port", type=int, default=int(saved.get("port", 33340)))
    parser.add_argument("--interval", type=float,
                        default=float(saved.get("interval", 1.0)))
    parser.add_argument("--lhm", nargs="?", const="",
                        help="LibreHardwareMonitor 数据地址；不带值时自动探测")
    parser.add_argument("--code", default="",
                        help="固定使用这个 4 位配对码（默认随机生成，保存在 exe 旁 .pc_status_code）")
    args = parser.parse_args()

    # 传感器：优先进程内直读 DLL（不需要 LHM 进程、不占托盘），失败才退回 Web 服务器。
    direct = LhmDirect()
    if direct.open():
        source = f"进程内直读（{os.path.basename(direct._dll or 'LibreHardwareMonitorLib.dll')}）"
    else:
        lhm_url = args.lhm or ensure_lhm(verbose=False)
        source = (f"LibreHardwareMonitor Web 服务器（{lhm_url}）" if lhm_url
                  else f"不可用（{direct.error}；运行 python fetch_lhm.py 安装）")

    root = tk.Tk()
    app = AgentApp(root, args.host, args.port, args.interval, source,
                   direct if direct.available else None,
                   args.code or saved.get("code", ""))
    if args.host:
        app._toggle()       # 命令行给了地址就直接开跑
    else:
        app._set_status("请填写设备屏幕上显示的 IP，然后点击“开始上报”", MUTED)
    if app.tray is None:
        app.status.configure(
            text=app.status.cget("text") + "（未安装 pystray，关闭窗口即退出）")
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
