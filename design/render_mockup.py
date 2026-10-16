# -*- coding: utf-8 -*-
"""Render ESP-Mosaico PC status panel design mockups (480x480) to PNG.

Design artifact only -- not part of the device firmware build.
Run: python projects/pc_display/design/render_mockup.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.dirname(os.path.abspath(__file__))
S = 2  # supersample factor for smooth edges/text

C = dict(
    bg_top=(22, 32, 43), bg_bot=(7, 10, 15),
    card_top=(18, 26, 35), card_bot=(14, 21, 29),
    line=(30, 42, 54), txt=(231, 238, 245), dim=(138, 154, 171),
    accent=(255, 76, 1), cyan=(34, 211, 238), green=(43, 217, 123),
    amber=(255, 176, 32), red=(255, 77, 77), violet=(196, 181, 253),
    track=(28, 39, 50), white=(255, 255, 255),
)

FONT = r"C:\Windows\Fonts\msyh.ttc"
FONT_BD = r"C:\Windows\Fonts\msyhbd.ttc"
_cache = {}


def f(size, bold=False):
    key = (size, bold)
    if key not in _cache:
        _cache[key] = ImageFont.truetype(FONT_BD if bold else FONT, int(size * S))
    return _cache[key]


def rrect(d, box, radius, fill=None, outline=None, width=1):
    x0, y0, x1, y1 = [v * S for v in box]
    d.rounded_rectangle((x0, y0, x1, y1), radius=radius * S, fill=fill,
                        outline=outline, width=int(width * S))


def txt(d, xy, s, size, color, bold=False, anchor="la"):
    d.text((xy[0] * S, xy[1] * S), s, font=f(size, bold), fill=color, anchor=anchor)


def vgrad(img, box, c0, c1):
    x0, y0, x1, y1 = box
    h = y1 - y0
    d = ImageDraw.Draw(img)
    for i in range(h):
        t = i / max(1, h - 1)
        col = tuple(int(c0[k] + (c1[k] - c0[k]) * t) for k in range(3))
        d.rectangle((x0, y0 + i, x1, y0 + i + 1), fill=col)


def base_screen():
    img = Image.new("RGB", (480 * S, 480 * S), C["bg_bot"])
    vgrad(img, (0, 0, 480 * S, 480 * S), C["bg_top"], C["bg_bot"])
    d = ImageDraw.Draw(img)
    rrect(d, (0, 0, 480, 480), 26, outline=(38, 49, 61), width=1)
    return img, d


def card(d, box):
    x0, y0, x1, y1 = box
    rrect(d, box, 16, fill=C["card_bot"], outline=C["line"], width=1)
    return box


def header(d, title, sub, status, dot_color, status_color=None):
    txt(d, (18, 19), title, 20, C["txt"], bold=True)
    txt(d, (18, 43), sub, 12, C["dim"])
    sw = d.textlength(status, font=f(12.5)) / S
    sx = 462 - sw
    d.ellipse((sx - 17 * S, 25 * S, sx - 8 * S, 34 * S), fill=dot_color)
    txt(d, (sx, 22), status, 12.5, status_color or C["dim"])


def bar(d, box, pct, color):
    x0, y0, x1, y1 = box
    rrect(d, box, 4, fill=C["track"])
    w = (x1 - x0) * min(100, max(0, pct)) / 100.0
    if w > 1:
        rrect(d, (x0, y0, x0 + w, y1), 4, fill=color)


def temp_badge(d, right_x, top_y, text):
    tw = d.textlength(text, font=f(13, True)) / S
    x0 = right_x - tw - 15
    rrect(d, (x0, top_y, right_x, top_y + 22), 8,
          fill=(45, 36, 20), outline=(120, 88, 40), width=1)
    txt(d, (right_x - 7, top_y + 4), text, 13, C["amber"], bold=True, anchor="ra")


# ---------------------------------------------------------------- screen 1
def screen_main():
    img, d = base_screen()
    header(d, "我的电脑", "Windows 11 · DESKTOP-8K2QM", "已连接 · 1s", C["green"])
    gx0, gy0, gx1, gy1, gap = 18, 76, 462, 380, 12
    cw = (gx1 - gx0 - gap) / 2.0
    ch = (gy1 - gy0 - gap) / 2.0
    xs = [gx0, gx0 + cw + gap]
    ys = [gy0, gy0 + ch + gap]

    # CPU
    b = (xs[0], ys[0], xs[0] + cw, ys[0] + ch)
    card(d, b)
    d.rounded_rectangle(((b[0] + 15) * S, (b[1] + 17) * S, (b[0] + 22) * S, (b[1] + 24) * S),
                        radius=2 * S, fill=C["accent"])
    txt(d, (b[0] + 28, b[1] + 13), "CPU", 13, C["dim"])
    temp_badge(d, b[2] - 14, b[1] + 11, "56°C")
    txt(d, (b[0] + 15, b[1] + 66), "42", 46, C["txt"], bold=True)
    nx = b[0] + 15 + d.textlength("42", font=f(46, True)) / S + 6
    txt(d, (nx, b[1] + 92), "%", 17, C["dim"], bold=True)
    bar(d, (b[0] + 15, b[3] - 20, b[2] - 15, b[3] - 13), 42, C["accent"])

    # Memory
    b = (xs[1], ys[0], xs[1] + cw, ys[0] + ch)
    card(d, b)
    txt(d, (b[0] + 15, b[1] + 13), "内存", 13, C["dim"])
    d.rounded_rectangle(((b[0] + 56) * S, (b[1] + 17) * S, (b[0] + 62) * S, (b[1] + 23) * S),
                        radius=2 * S, fill=C["cyan"])
    txt(d, (b[0] + 15, b[1] + 66), "11.2", 46, C["txt"], bold=True)
    nx = b[0] + 15 + d.textlength("11.2", font=f(46, True)) / S + 6
    txt(d, (nx, b[1] + 92), "/ 32 GB", 15, C["dim"], bold=True)
    bar(d, (b[0] + 15, b[3] - 34, b[2] - 15, b[3] - 27), 35, C["cyan"])
    txt(d, (b[0] + 15, b[3] - 22), "已用 35%", 12.5, C["dim"])

    # Disk
    b = (xs[0], ys[1], xs[0] + cw, ys[1] + ch)
    card(d, b)
    txt(d, (b[0] + 15, b[1] + 13), "磁盘 C:", 13, C["dim"])
    d.rounded_rectangle(((b[0] + 74) * S, (b[1] + 17) * S, (b[0] + 80) * S, (b[1] + 23) * S),
                        radius=2 * S, fill=C["green"])
    txt(d, (b[0] + 15, b[1] + 66), "512", 46, C["txt"], bold=True)
    nx = b[0] + 15 + d.textlength("512", font=f(46, True)) / S + 6
    txt(d, (nx, b[1] + 92), "/ 1024 GB", 15, C["dim"], bold=True)
    bar(d, (b[0] + 15, b[3] - 34, b[2] - 15, b[3] - 27), 50, C["green"])
    txt(d, (b[0] + 15, b[3] - 22), "可用 512 GB", 12.5, C["dim"])

    # Network
    b = (xs[1], ys[1], xs[1] + cw, ys[1] + ch)
    card(d, b)
    txt(d, (b[0] + 15, b[1] + 13), "网络", 13, C["dim"])
    d.rounded_rectangle(((b[0] + 46) * S, (b[1] + 17) * S, (b[0] + 52) * S, (b[1] + 23) * S),
                        radius=2 * S, fill=C["violet"])
    txt(d, (b[0] + 15, b[1] + 46), "↓ 下载", 14, C["dim"])
    txt(d, (b[2] - 15, b[1] + 42), "12.4", 22, C["cyan"], bold=True, anchor="ra")
    txt(d, (b[0] + 15, b[1] + 78), "↑ 上传", 14, C["dim"])
    txt(d, (b[2] - 15, b[1] + 74), "0.8", 22, C["violet"], bold=True, anchor="ra")
    txt(d, (b[0] + 15, b[3] - 22), "MB/s · Intel Wi-Fi 6", 12, C["dim"])

    # footer
    txt(d, (18, 392), "GPU", 12.5, C["dim"])
    txt(d, (18 + d.textlength("GPU ", font=f(12.5)) / S, 392), "23%", 12.5, C["txt"], bold=True)
    txt(d, (18 + d.textlength("GPU 23% ", font=f(12.5, True)) / S + 8, 392), "· 46°C", 12.5, C["dim"])
    txt(d, (18 + d.textlength("GPU 23% · 46°C   ", font=f(12.5)) / S + 24, 392), "运行", 12.5, C["dim"])
    txt(d, (18 + d.textlength("GPU 23% · 46°C   运行 ", font=f(12.5)) / S + 24, 392),
        "3天 4小时", 12.5, C["txt"], bold=True)
    txt(d, (462, 392), "21:03:44", 12.5, C["dim"], anchor="ra")
    return img


# ---------------------------------------------------------------- screen 2
def screen_connecting():
    img, d = base_screen()
    header(d, "我的电脑", "DESKTOP-8K2QM", "未连接", C["red"], status_color=C["red"])
    cx, cy = 240, 232
    d.arc((cx * S - 27 * S, cy * S - 27 * S, cx * S + 27 * S, cy * S + 27 * S),
          start=-80, end=200, fill=C["accent"], width=int(4 * S))
    d.arc((cx * S - 27 * S, cy * S - 27 * S, cx * S + 27 * S, cy * S + 27 * S),
          start=210, end=330, fill=(30, 42, 54), width=int(4 * S))
    txt(d, (cx, 300), "正在等待电脑…", 26, C["txt"], bold=True, anchor="ma")
    txt(d, (cx, 342), "请确认电脑端「状态上报」程序已启动", 15, C["dim"], anchor="ma")
    txt(d, (cx, 366), "并处于同一局域网", 15, C["dim"], anchor="ma")
    pill = "本机 192.168.1.42 · 已重试 3 次"
    pw = d.textlength(pill, font=f(13)) / S + 28
    rrect(d, (cx - pw / 2, 406, cx + pw / 2, 434), 14, fill=C["card_bot"], outline=C["line"])
    txt(d, (cx, 412), pill, 13, C["dim"], anchor="ma")
    return img


# ---------------------------------------------------------------- screen 3
def screen_wifi():
    img, d = base_screen()
    header(d, "连接无线网络", "选择 2.4GHz Wi-Fi 并输入密码", "配置中", C["amber"], status_color=C["amber"])
    rows = [("MyHome-2.4G", "-42 dBm", C["green"]),
            ("Office-Guest", "-68 dBm", C["amber"]),
            ("TP-LINK_5G", "-85 dBm", C["red"])]
    y = 92
    for name, rssi, col in rows:
        rrect(d, (18, y, 462, y + 56), 16, fill=C["card_bot"], outline=C["line"])
        d.arc((32 * S, (y + 20) * S, 48 * S, (y + 36) * S), start=210, end=330,
              fill=col, width=int(2.5 * S))
        txt(d, (58, y + 18), name, 16, C["txt"])
        txt(d, (446, y + 20), rssi, 14, col, anchor="ra")
        y += 68
    txt(d, (240, 388), "点击网络名称 → 输入密码 → 保存并重启", 13, C["dim"], anchor="ma")
    return img


def labeled(img, cap):
    pad, caph = 14, 30
    w, h = img.width // S, img.height // S
    out = Image.new("RGB", (w + pad * 2, h + pad * 2 + caph), (21, 24, 29))
    out.paste(img.resize((w, h), Image.LANCZOS), (pad, pad + caph))
    d = ImageDraw.Draw(out)
    d.text((pad, 10), cap, font=ImageFont.truetype(FONT, 15), fill=(154, 167, 180))
    return out


def main():
    screens = [
        (screen_main(), "① 主界面 · 已连接（PC 状态实时刷新 1s）"),
        (screen_connecting(), "② 状态页 · 正在等待电脑"),
        (screen_wifi(), "③ 首次配网（触摸操作）"),
    ]
    for i, (img, _) in enumerate(screens, 1):
        img.resize((480, 480), Image.LANCZOS).save(os.path.join(OUT, f"mockup_{i}.png"))
    frames = [labeled(img, cap) for img, cap in screens]
    gap = 16
    W = sum(f.width for f in frames) + gap * (len(frames) + 1)
    H = max(f.height for f in frames) + gap * 2
    strip = Image.new("RGB", (W, H), (15, 17, 20))
    x = gap
    for fr in frames:
        strip.paste(fr, (x, gap))
        x += fr.width + gap
    strip.save(os.path.join(OUT, "mockup_all.png"))
    print("written:", os.path.join(OUT, "mockup_all.png"))


if __name__ == "__main__":
    main()
