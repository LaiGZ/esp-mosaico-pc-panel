# ESP-Mosaico PC 状态面板

把 Windows 电脑的实时硬件状态显示在 ESP-Mosaico 的 480×480 触摸屏上：CPU 占用 / 温度 / 功率 /
频率、内存、GPU 占用 / 显存、网络上下行；点卡片进入详情页看双折线趋势。设备通过 Wi-Fi 与电脑
通信，**多台电脑在同一网段时用 4 位配对码绑定**，互不干扰。

本仓库是 ESP-Mosaico 工作区中 `projects/pc_display` 的独立副本，方便单独查看、分发与打包。
完整文档见下方「PC 状态面板（pc_display）」一节。

## 设备端（固件）

```sh
# 1) 先准备好 ESP-Mosaico 工作区（含 submodule 与 pinned ESP-IDF）
# 2) 把本仓库内容放进工作区：
#      <workspace>/projects/pc_display/
# 3) 生成中文字体子集（仓库不包含生成的字体文件）
python ui/fonts/make_fonts.py
# 4) 构建与安装
python mosaico.py project build --project projects/pc_display
python mosaico.py iris system-update --project projects/pc_display
```

## PC 端（状态上报客户端）

图形界面 + 托盘，采集逻辑与设备端协议在同一个 Python 文件里；传感器温度/功率/显存通过
`LibreHardwareMonitorLib.dll` **在客户端进程内**读取，不需要单独运行 LibreHardwareMonitor。

```sh
python -m pip install psutil pythonnet pystray
python agent/fetch_lhm.py        # 下载传感器库到 agent/lhm/
python agent/run_gui.bat         # 或 python agent/pc_status_gui.py
# 想要单文件 exe 分发：python -m pip install pyinstaller && python agent/build_exe.py
```

**配对流程**：客户端显示一个 4 位配对码；设备连上 Wi-Fi 后先出现「连接电脑」页 →
点「匹配」→ 用屏幕数字键盘输入该码 → 绑定完成，设备只显示这台电脑的数据
（想换电脑：设备信息页「重新配对」）。

---

本项目把「[ESP32C3-PC-Wireless-Display](https://github.com/WuyongLab/ESP32C3-PC-Wireless-Display)」
的**电脑硬件状态显示**能力移植到 ESP-Mosaico 开发板上，并按 480×480 触摸屏重新设计了界面。

> 与原项目的差异：**不移植桌面投屏**。设备只接收并显示电脑的状态指标（CPU / 内存 / GPU /
> 网络 / 温度 / 功率 / 显存），不再传输图像，因此带宽与内存占用极低。

> **下载 Windows 客户端**：[Release v1.0.0](https://github.com/LaiGZ/esp-mosaico-pc-panel/releases/tag/v1.0.0)
> —— `pc_status_panel-1.0.0-win64.zip`，单文件 exe，目标电脑不用装 Python。

## 功能

| 功能 | 说明 |
|---|---|
| 状态面板 | CPU（含温度/功率/频率）、内存、GPU（占用 + 显存 + 温度）、网络上下行、运行时长、时钟 |
| 详情页 | 点 CPU / 内存 / GPU / 网络卡片进入：顶部实时读数，下方**两张折线图**，含最低最高统计 |
| 设备识别 | 设备收到状态包后回一个**身份小包**，PC 端因此显示「已连接设备：PC 状态面板」（名称/型号/固件/设备 IP）；这也是真实的连接指示 —— 收到回执才算通 |
| 设备配对 | 两段式：Wi-Fi 连上后显示「连接电脑」引导页（提示打开 PC 客户端 + 「匹配」按钮）→ 点「匹配」进入**数字键盘**输入电脑上显示的 4 位配对码 → 绑定。设备端记录 **码 + 对方地址/主机名**（NVS 持久化），只显示已配对那台电脑的数据；信息页「重新配对」直接进配对码页 |
| PC 端形态 | 一个窗口 + 一个托盘图标：关闭窗口即缩到托盘继续上报（`pystray`），托盘颜色表示连接状态，菜单可「显示窗口 / 退出」 |
| 传感器来源 | 客户端进程内直读 `LibreHardwareMonitorLib.dll`（pythonnet），**不跑 LHM 进程、不占托盘**；不可用时退回 LHM Web 服务器 |
| 实时刷新 | PC 端每秒上报一次，5 秒无数据自动转为「等待连接」 |
| 触摸配网 | 首次开机在屏幕上选择 2.4G 网络、用软键盘输入密码，凭据存入 NVS |
| 自动重连 | 断线后按 5 秒退避重连；网络异常时回到配网页 |
| 时钟 | 用 PC 端时间戳校准，无需板载 RTC |
| 旋转 | BMI270 判断重力方向，四个方向自动转屏（约 1 秒稳定判定） |

## 架构

```
┌── PC 端 pc_status_agent.py ──┐        ┌── 设备端 pc_display ──────────────────┐
│ psutil 采集指标               │  UDP   │ wifi_manager  连接/扫描/凭据(NVS)      │
│ → 扁平 JSON（约 200 字节）     │ ─────▶ │ pc_link       收包 → 解析 → 快照       │
│ 广播或指定 IP，默认 1s         │ :33340 │ pc_panel_ui   ESP-GSP 界面（可移植）   │
└──────────────────────────────┘        └──────────────────────────────────────┘
```

设备端代码分层（`main/`）：

| 文件 | 职责 | 依赖 |
|---|---|---|
| `app_state.h` | 共享数据结构（状态样本、链路/WiFi 状态、AP 列表） | 无 |
| `wifi_manager.c` | Wi-Fi STA、NVS 凭据、扫描、退避重连 | ESP-IDF |
| `pc_link.c` | UDP 接收任务 + 内置极简 JSON 解析 | ESP-IDF |
| `pc_panel_ui.c` | 全部 ESP-GSP 界面与事件分发 | 仅 `esp_gsp.h`（**可移植**） |
| `main.c` | 组装以上模块 + 恢复/OTA 契约 + 界面刷新任务 | ESP-IDF |
| `pc/platform_pc.c` | 模拟器适配（喂入合成数据，循环切换页面） | 仅 GSP |

`pc_panel_ui.c` 不依赖板级、FreeRTOS 或 ESP-Iris API，因此同一份界面逻辑既跑在设备上，
也跑在桌面模拟器里。

## 通信协议

设备监听 **UDP 33340**。PC 端发一个 UTF-8 JSON 对象（`v2`）：

```json
{"v":2,"host":"DESKTOP-8K2QM","os":"Windows 10","cpu":42,"cpu_temp":64,"cpu_power":56.3,
 "cpu_freq":4442,"mem_used":21.3,"mem_total":29.9,"gpu":12,"gpu_mem_used":1.21,
 "gpu_mem_total":2.0,"net_down":12.4,"net_up":0.8,"uptime":276480,"ts":1792049796}
```

| 字段 | 含义 | 来源 |
|---|---|---|
| `cpu` / `mem_used` / `mem_total` / `net_down` / `net_up` | 占用率、内存 GB、网络 MB/s | psutil || `cpu_temp` / `cpu_power` / `cpu_freq` | CPU 温度 °C、封装功率 W、平均频率 MHz | LibreHardwareMonitor |
| `gpu` | GPU 占用 % | Windows 性能计数器（LHM 兜底） |
| `gpu_temp` / `gpu_vram_temp` | GPU / 显存温度 °C（显卡不暴露时不下发） | LibreHardwareMonitor |
| `gpu_mem_used` / `gpu_mem_total` | 显存 GB | LibreHardwareMonitor |
| `uptime` / `ts` / `host` / `os` | 运行时长、PC 时间戳、主机名、系统 | psutil / socket |
| `code` | 本机的 4 位配对码；**设备只接受与自身配对码一致的数据** | 客户端首次运行生成并保存在 `.pc_status_code` |

* 除 `host`/`os` 外所有字段可选；缺失的指标界面显示 `--` 或标注「无数据」。
* **设备身份回执**：设备收到状态包后，向包源地址回一个小包（默认每 3 秒最多一次）：
  ```json
  {"dev":"pc_display","name":"PC 状态面板","model":"ESP-Mosaico","fw":"1.0.0","ip":"192.168.1.42"}
  ```
  PC 端据此显示「已连接设备」，并把它当作**真实连接指示**（只有收到回执才说明设备确实在收包，
  而不是「我发出去了」）。设备 IP 变化（DHCP 续约）时会自动重新发布。
* **上下行只统计物理网卡**。两个坑：① `psutil.net_io_counters()` 的**聚合值**在 Windows 上走
  32 位接口表，实测会间歇性报出几十 GB/s，改用逐网卡 64 位计数累加；② 代理/TUN（如 Mihomo）
  和 WSL/VMware 虚拟网卡会把**同一份流量再算一遍**（实测代理网卡计数是物理网卡的 2.5 倍），
  因此按名称过滤掉虚拟网卡；过滤后若为空（纯 VPN 场景）再退回「除 loopback 外全部」。
  用已知大小的下载做了交叉验证：下 53 MB，统计到 57.5 MB 物理流量（多出的是远程桌面等真实流量）。
* 解析器只识别上述已知键（扁平对象），容忍多余字段，不引入 JSON 依赖。
* 未做认证与加密，仅适合可信局域网（与源项目结论一致）。

## 使用

### 1. PC 端上报程序

```sh
python -m pip install psutil

# 图形界面（推荐日常使用；也可直接双击 agent\run_gui.bat）
python projects/pc_display/agent/pc_status_gui.py --host 192.168.1.42

# 命令行（最省资源，适合开机自启）
python projects/pc_display/agent/pc_status_agent.py --host 192.168.1.42
```

**最省事的用法**：双击 `agent/run_gui.bat` → 在「设备地址」里填设备屏幕上显示的 IP →
点「开始上报」。窗口顶部会显示**已连接设备：PC 状态面板**（拿到设备回执才有这一行，
没拿到会显示「未连接设备 / 设备无响应」）。地址、端口、间隔会记在
`agent/.pc_status_gui.json`，下次打开自动填好。

**配对（多台电脑时必需）**：客户端会显示一个 4 位**配对码**（首次随机生成后固定，存
`agent/.pc_status_code`；想固定成别的码可以用 `--code 1234`，GUI 里也可以直接改
`agent/.pc_status_gui.json` 的 `code` 字段）。设备加入 Wi-Fi 后：

1. 设备显示**「连接电脑」**页 —— 请在电脑上打开客户端（设备会显示本机 IP，填进客户端的
   「设备地址」并点「开始上报」）；
2. 点设备上的**「匹配」**按钮，进入**数字键盘**页；
3. 输入客户端上显示的 4 位配对码，按「配对」完成绑定（输错会提示并自动清空）。

之后设备**只显示这台电脑的数据**（码 + 地址或主机名都要对上，重启/换网络/换 IP 都保持）。
想换电脑：在设备信息页点**「重新配对」**，会直接回到配对码页。未配对的客户端按 2 秒间隔
试探上报（够设备发现配对码，但不争显示），配对成功后回到 1 秒。

**关闭窗口 = 缩到托盘继续上报**（只留一个托盘图标）：托盘图标颜色跟连接状态走
（绿=已连接、红=未连接），悬停显示设备名；菜单里有「显示窗口」「退出」。
没装 `pystray` 时关闭即退出，状态栏会提示。

**GPU 占用**（Windows）用系统自带的 `typeperf` 读性能计数器，**不需要装任何软件**；
`typeperf` 单次采样约 1 个采样周期，因此 GPU 值在后台线程里约每 2 秒刷新一次。

**CPU 温度/功率/频率、显存、GPU 温度** 在 Windows 上只能由带**内核驱动**的读取器提供
（用户态读不了 MSR/PCI），因此用
[LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)（MPL‑2.0）的
**库**而非它的程序：

```sh
python -m pip install pythonnet        # 让 Python 能加载 .NET 库
python projects/pc_display/agent/fetch_lhm.py   # 每个新电脑一次，把库解压到 agent/lhm/
```

客户端随后在**自己进程内**用 `LibreHardwareMonitorLib.dll` 读传感器
（`pc_status_agent.LhmDirect`）：**不跑 LHM 进程、不占 Web 端口、不额外多一个托盘图标**，
托盘里只有我们的客户端。若直读不可用（缺 pythonnet/DLL），会退回原来的 Web 服务器路径，
那时才会自动把 `LibreHardwareMonitor.exe` 最小化拉起。第一次读温度时 Windows 会弹一次
UAC（装载驱动），之后不再需要。

> 本机实测：把 LHM 完全杀掉后，客户端仍拿到 `Core (Tctl/Tdie) 65.6°C / Package 44.0W /
> Cores (Average) 3664MHz / 显存 1359MB`，且 `LibreHardwareMonitor` 进程数为 0。
> 直读的传感器值单位与 Web 路径一致（Data/SmallData 为 MB），所以两条路径共用同一套挑选逻辑。

> 本机实测：8085–8087 被 Docker Desktop 与 WSL 中继占用，LHM 配置成 **18086**；
> `fetch_lhm.py` 会自动挑一个能用的端口（8085→8086→18086→8899）。
> 直接改 LHM 同目录的 `LibreHardwareMonitor.config`（`runWebServerMenuItem` /
> `listenerIp` / `listenerPort` 三项）比点菜单更省事；`listenerIp` 必须是
> `127.0.0.1`、`0.0.0.0` 或本机某个网卡地址，写其它值 LHM 会**静默启动失败**
> （它退回 `+` 前缀，需要管理员权限才注册成功）。

测到没有温度传感器也不用担心：设备会把该项显示为 `--`，详情页的对应折线图保持空白
（AMD 核显常见如此——LHM 的 GPU 节点下只有 Load / Data，没有 Temperatures）。

**让 LHM 随开机启动**（否则重启后温度/功率/显存又会消失）：在
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 下加一个名为
`LibreHardwareMonitor`、值为 exe 全路径的字符串，并在同目录的
`LibreHardwareMonitor.config` 里写 `startMinMenuItem=true`（启动即最小化）
与 `minTrayMenuItem=true`（最小化到托盘）。

### 2. 构建

```sh
# 需要 pinned ESP-IDF（见工作区 AGENTS.md）
python submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py \
    --project projects/pc_display doctor
python submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py \
    --project projects/pc_display build
```

### 3. 模拟器预览

```sh
python mosaico.py project sim --project projects/pc_display --interactive
```

宿主后端需要本地 C 编译器与 `cmake`；场景静态渲染可用
`--scene-only --dump-ppm out.ppm`（使用预编译模拟器，不需要编译器）。

### 4. 安装到设备

```sh
python mosaico.py recover                                  # 空白/未验证设备首次安装
python mosaico.py iris system-update --project projects/pc_display
```

## 界面设计

设计稿在 `design/`：`mockup_*.png` 是**实现前的设计提案**（当时还是磁盘卡片、单图趋势页），
仅作历史参考；与设备一致的当前样张是 `dash_gpu.png`、`info_page.png`、
`detail_cpu/mem/gpu/net.png`、`wait_shot.png`（`python mosaico.py iris screenshot`），
`gui_window.png` 是 PC 端窗口。实际界面由 `ui/main.json` 定义，编译为 ESP-GSP 场景
（139 个对象 / 8 张折线图）。

| 页面 | 内容 |
|---|---|
| 连接电脑页 | Wi-Fi 连上但还没绑定电脑时显示：提示「请在电脑上打开客户端」（已发现电脑时显示其名字）+ 本机 IP + 「匹配」按钮 |
| 设备配对页 | 点「匹配」或「重新配对」后进入：提示输入 4 位配对码 + **3×4 数字键盘**（1-9 / 删除 / 0 / 配对），输错会提示并自动清空 |
| 状态面板 | 顶部主机名 + 连接状态灯，2×2 卡片（CPU / 内存 / **GPU** / 网络），底部为运行时长与时钟；右上角「连接」按钮进入配网 |
| CPU 详情页 | 点 CPU 卡片进入：大号当前占用 + 「温度 / 功率 / 频率」读数行 + **温度、占用两张 60 点折线图**（**橙色，与 CPU 卡片一致**） |
| 内存详情页 | 点内存卡片进入：占用 + 已用/总量，下方**占用、已用 GB 两张折线图**（**青色**） |
| GPU 详情页 | 点 GPU 卡片进入：占用 + 温度/显存读数行，下方**温度、显存两张折线图**（**紫色**） |
| 网络详情页 | 点网络卡片进入：下载速率大号读数 + 上传/总计，下方**下载、上传两张折线图**（青色 / 淡紫，与卡片上的两个数值同色） |
| 设备信息页 | 在状态面板**左右滑动**进入：设备名称/型号/固件/设备 IP/运行时长/内存，以及电脑连接状态、Wi‑Fi 信号、电脑名/系统/电脑 IP/收包数 |
| 等待页 | 「正在等待电脑…」+ 排查提示 + 本机 IP + 「重新配置网络」 |
| 配网页 | 扫描到的网络列表（最多 6 个，当前已配置的网络标注「已连接」）+ 「重新搜索」 |
| 密码页 | 网络名 + 密码输入框 + 屏幕键盘 + 保存/返回 |

四张卡片结构一致：标题（+ 右上角温度角标）→ 大号数值 + 小号单位 → 进度条 → 一行补充说明
（CPU「60W · 4.37GHz」、内存「共 30 GB · 已用 82%」、GPU「显存 1.40 / 2.0 GB」、网络「↓ 23.7 MB/s」）。
数值在固定宽度的框内**右对齐**，单位紧贴在数字右侧，因此 1 位与 3 位数值的间距一致，不会出现
「`7   %`」这种松散排版。

卡片与详情页的配色一一对应（CPU 橙 `#FF4C01`、内存青 `#22D3EE`、GPU 紫 `#A78BFA`）；
折线图的 `min`/`max` 在场景里固定为 0–100（内存「已用 GB」图固定 0–32GB），图上方
的说明行会把真实数值（含单位）写出来，因此缩放不会误导。

**手势**：状态面板与设备信息页是**一对根页面**，在其上左右滑动即可互相切换；其余页面
**向左或向右滑动都是「返回上一页」**——详情页 → 状态面板，设备信息页 → 状态面板，
密码页 → 网络列表，配网页 → 退出配网。进入配网请用状态面板右上角的「连接」按钮
（或等待页的按钮）。

## 内存检查

静态审计之外，固件内置了周期性堆探针 `main/heap_watch.c`（每 10 秒一次），直接输出
可用于判断**泄漏**与**碎片**的三个指标：

```
I (120000) heap_watch: internal: free=... largest=... min_free=... frag=...%
I (120000) heap_watch: psram:    free=... largest=... frag=...%
```

| 指标 | 判读方式 |
|---|---|
| `free` | 长期持续下降 → 泄漏 |
| `min_free` | 历史最低水位；随时间不断被刷新变低同样指向泄漏 |
| `largest` | 「还能不能分配」的关键值；`free` 大而 `largest` 小 → 碎片 |
| `frag` | `100 - largest*100/free`；≥ 50% 时自动 `WARN` |

已按审计结论采取的降碎片措施：扫描缓冲改为**一次性固定分配**（不再每次扫描
`calloc` 变长数组）；界面仅**在数据变化时**才发送 GSP 属性更新（避免 2 Hz × 约 20 个
setter 的持续堆压力）；所有格式化均在栈上完成。UDP 解析器为原地扫描、零分配。

## 注意事项

* **中文字体（最容易踩的坑）**：ESP-GSP 的运行时文字字形**不会**自动全量内嵌，它只烘焙
  「场景静态文字 + 该对象声明的 `font_charset`」。因此：

  1. 任何会在运行时被写入的控件，都必须声明 `font_charset`，且**必须覆盖它可能显示的每个
     字符（含空格）**；只显示数字的控件写 `0123456789.` 即可。字符集内出现但字体文件里没有
     的字形、或字体里有但字符集没声明的字形，设备上都会显示成 `?`。
  2. 修改界面文字、C 源码里的中文字符串、或任何 `font_charset` 之后，**必须**重新运行
     `python ui/fonts/make_fonts.py`。该脚本会一并收集「场景 `text` / `font_charset` /
     `font_charset_file` / `main/*.c`」的字符生成子集，并对缺失字形**直接报错退出**。
  3. 不要在**场景级**声明很大的字符集（例如全量 ASCII + 上百汉字）：实测会触到 GSP 的资源
     上限并导致启动崩溃（`wifi` 任务 abort）。按控件声明小集合是正确做法。
  4. 字形文件仅约 40 KB；发布前请把源字体换成可自由分发的字体，详见 `ui/fonts/NOTICE.md`。

* **空文本会显示 `?`**：GSP 把空字符串渲染成占位符。需要「无数据时不显示」的控件（如 CPU
  温度角标）应声明 `hidden` 并用可见性控制，而不是把文字置空。

* **Windows 上的 gspc**：pinned 的 GSP 编译器会把 `\\?\` 前缀、双重转义的反斜杠路径写进
  depfile，CMake 转换后变成非法的 `/D:/...`，Ninja 会因此报错。`CMakeLists.txt` 在 Windows
  上把 `GSPC_EXECUTABLE` 指向 `tools/gspc_wrap.cmd`（包装 `tools/gspc_depfile_fix.py`）来
  归一化 depfile；显式设置环境变量 `GSPC_EXECUTABLE` 可覆盖此行为。

* **陀螺仪旋转屏幕（已启用）**：`main/orientation.c` 用 BMI270 的**重力方向**判断朝向（连续 2 次采样一致才切换，约 1 秒，避免手抖翻屏），`main/board_display.c` 自建 display presenter，朝向变化时执行
  `esp_gsp_esp_lcd_pause` → `esp_display_presenter_set_rotation` → `resume_paused`（官方契约要求的顺序，恢复时强制整帧重绘）。

  **关键：驱动返回的是原始计数，不是 g。** `bmi2_get_sensor_data()` 的 `acc.x/y/z` 是 ±2G 档下的
  **int16 LSB 计数**（16384 LSB/g）。曾按「单位是 g」写阈值 0.70，结果 `y = -33` 这种 ≈ -0.002 g 的
  残差也满足 `y <= -0.70`，判定永远命中前一条分支——表现为**怎么转都不转屏**（而日志还显示「稳定在 0°」）。
  现在统一除以 `ORIENTATION_ACCEL_LSB_PER_G` 再做判定；换量程时必须同步这个常量。

  **关键：不要用 BSP 的 `bsp_imu_init()`。** 它手工构造 `struct bmi2_dev` 时把 `read_write_len` 设为 32，而 Bosch 驱动自己的默认值是
  **46**（`common.c: READ_WRITE_LEN`，对应 `BMI2_FEAT_SIZE_IN_BYTES`），`bmi2.c` 中还有 `read_write_len < BMI2_FEAT_SIZE_IN_BYTES` 的分支。
  这个不一致会在**固件上传阶段越界写内存**，随后在**无关任务里**（实测是 `wifi`）`abort()`，并且栈已损坏所以拿不到调用栈——非常难查。
  本应用改用官方工厂固件的路径：`bmi270` 组件的封装 API（`bmi270_sensor_create` + `bmi2_set_adv_power_save(BMI2_DISABLE)` + 配置 + `bmi270_sensor_enable`），
  跑在 BSP 已建好的共享 I2C 总线上（`i2c_bus_create()` 对同一 port 返回共享总线，切勿删除），并带 0x68→0x69 地址探测与 5 秒退避重试。

  **轴映射**：BMI270 相对屏幕**旋转了 90°**（官方注释明确说明），因此屏幕纵向重力对应 `acc.y`、横向对应 `acc.x`。
  `orientation.c` 里 `ORIENTATION_ANGLE_FOR_PLUS_X/MINUS_X` 两个常量控制 90°/270° 的对应；若实测方向相反，改这两处即可（一行）。

* **场景是「按顺序绘制」，页面容器必须排在所有顶层面板元素之后**：状态面板的主机名、状态点、
  四张卡片、页脚都挂在根节点（`parent: -1`），而页面（配网/密码/详情/设备信息）是 480×480 的
  全屏容器，靠“排在后面”把状态面板盖住 —— GSP 没有真正的层级遮挡，只有绘制顺序。
  所以状态点（绿 `link_dot_ok` / 红 `link_dot_bad`）必须排在第一个页面容器之前（当前索引 2、3），
  否则会画在页面上（曾在配网页上看到一个多余绿点）。`ui/check_scene.py` 会直接报这类问题。

* **进入配网页会打断当前连接**：WLAN 扫描会让 STA 链路短暂断开，若扫描过于频繁（连续点「重新搜索」），
  可能看到「无线网络连接失败 / 本机 0.0.0.0」。管理器每 5 秒重试、会自动恢复；若长时间不恢复，
  通常是 AP 侧问题。界面判定「已连接」时以**是否拿到 IP** 为准，不只看状态机，避免启动竞态把已联网的设备误判成失败。

* **打包成 exe 后子进程会弹黑框**：GUI 版是 `--windowed`（没有控制台），此时每次用
  `subprocess` 起 `typeperf`（GPU 采样，约 2 秒一次）都会**新开一个控制台窗口**，表现为
  "屏幕不停闪黑框"。脚本方式运行不会出现，因为子进程继承了当前控制台。所有运行期子进程
  （`typeperf`、拉起 LHM）都加了 `creationflags=CREATE_NO_WINDOW`；打包后可这样确认：
  枚举 `typeperf` 进程的窗口，应当**没有任何窗口**。

* **exe 的配置与配对码放在 exe 旁边**：`--windowed --onefile` 会把程序解包到临时目录，
  `__file__` 指向那里，所以配置必须用 `sys.executable` 的目录（`pc_status_agent.app_dir()`）——
  否则配对码每次启动都会重新生成。读取设置/配对码用 `utf-8-sig`，容忍记事本写入的 BOM。

* **日志环会被刷满**：保留的日志环原为 4 KB。曾因为每秒打印一行采样摘要而把启动日志挤掉（导致排查时看不到启动阶段输出）。
  现在采样摘要**每 10 秒一行**，日志环也扩到 **16 KB**；排查期请保持低频日志。

* **屏幕旋转会让 GSP 每帧报一次 `canvas begin failed: -1`**：把设备转到 90°/270° 后，日志里会出现
  每帧一次的该报错（随后 GSP 走回退路径把画面渲染出来）。**实测面板仍然正常**（画面已旋转、右下角时钟照常走动），
  但报错会持续刷屏，所以日志环才扩到 16 KB。排查时若看到这行，先确认画面是否正常，不要以为渲染挂了。

* **截图看不到旋转**：`iris screenshot` 走的是场景镜像（按**逻辑坐标**重新渲染），所以设备物理旋转后
  截图仍是「正的」，也无法用来判断面板是否真的在刷新（镜像会重新画，看起来永远是活的）。
  旋转与流畅度只能在实机上看。

* **改场景后请跑 `python ui/check_scene.py`**：GSP 场景按对象顺序绘制、没有真正的遮挡，而且
  `parent` 是**数组下标** —— 手改或脚本重排对象时极易错挂。真实案例：为了把「连接」页面上的
  多余状态点挪走而重排索引时，**把按钮的文字标签父节点错挂到了状态点上**，结果按钮变成空框、
  「连接」两字跑到屏幕左上角。`ui/check_scene.py` 会检查父节点是否合法（必须挂在容器/按钮上）、
  页面之外是否还有顶层对象（会被画在所有页面之上）、名字是否重复，并打印每个页面的子对象数量 ——
  改完场景跑一遍，能挡住这类"控件跑到别处"的问题。

* **刚切页后截图可能拍到合成残留**：镜像在页面切换的瞬间可能给出一帧混合画面（实测在配网页上看到
  状态面板 + 一个错位的按钮）。这是截图通道的问题，不是界面问题 —— **实机画面才是准的**。
  截图前让页面稳定几秒，或换一张再拍。

* **恢复契约**：应用保留保留态 Recovery 分区前缀，使用 `esp_mosaico_app_recovery` 与
  `iris_ota_support_start()`，OTA 写入仅存在于 Vibe Mode。请勿用 `idf.py flash` 直接烧录。
