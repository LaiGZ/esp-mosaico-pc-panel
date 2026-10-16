# PC 状态面板 · Mosaico Ideas 发布材料

> 目标站点：<https://mosaico-ideas.espressif.com>
> 应用版本：**1.0.0** ｜ 设备：ESP-Mosaico（ESP32-S31）｜ 材料生成时间：2026-10-16
> 状态：**材料已备齐并通过本地校验，尚未上传**（上传会生成草稿，是否提交审核由你来点）

---

## 0. 一页速览

| 表单栏位 | 你要交的东西 | 在哪里 |
| --- | --- | --- |
| ② 固件文件 → **添加 Iris 固件** | `pc_status_panel-1.0.0.irisfw`（1.22 MiB） | `D:\laigz\esp32\tools\mosaico-ideas\` |
| ③ 项目描述 | 下面的 Markdown 全文 | 见 [第 2 节](#2-材料二项目描述markdown) |
| ① 项目信息（标题 / 分类 / 标签 / 许可 / 外链 / 封面） | 见 [第 3 节](#3-表单其余字段) 与 [第 4 节](#4-封面图片) | 机器可读版：`projects/pc_display/mosaico-ideas.json` |

---

## 1. 材料一：固件（Iris 包）

### 交付文件

| 项目 | 值 |
| --- | --- |
| 路径 | `D:\laigz\esp32\tools\mosaico-ideas\pc_status_panel-1.0.0.irisfw` |
| 大小 | 1,279,838 B（1.22 MiB；平台上限 32 MiB） |
| SHA-256 | `36095adc2c7a7f3573b43b8d20d9cb73e9304f9e3ab9587337cf373dbb23d53f` |
| 版本 | `1.0.0` |
| 形态 | 无签名 `.irisfw`，内部 schema `esp-iris-system-update/v1` |
| 源包 | `D:\laigz\esp32\vibe\projects\pc_display\build\pc_display-system-update.irisfw` |

### 上传到哪一栏

选 **「添加 Iris 固件」**。

**不要选「添加 BIN 固件」**：BIN 那一栏只接收单个应用镜像，装不出本设备需要的完整布局。
这个 Iris 包里已经带齐「分区表 + 应用 + `ui_apps` 数据镜像」，正是平台接受的形态。
也正因为 `ui_apps` 分区带了数据镜像，不会触发「分区缺少数据镜像」的风险确认。

### 包内组件（本地 `bundle inspect` 结果）

| 组件 | 目标地址 | 大小 | SHA-256 |
| --- | --- | --- | --- |
| `partition_table` | `0x8000` | 4,096 B | `4f8786d1…c7231` |
| `application`（→ `ota_0`） | `0x210000` | 1,812,464 B | `5c91737a…0c0a4` |
| `data`（→ `ui_apps`） | `0xF00000` | 329,232 B | `51552c17…a3187` |

- 芯片：ESP32-S31（`chip_id = 32`），Flash 16 MiB，`flags = 0`
- 不含 `bootloader` / `recovery` / 平台身份，`signature_verified = false`（平台接受无签名包）
- 兼容要求：`minimum_recovery_version = 0.1`
- 包内 `release = 1.0.0`，与构建产物 `build/project_description.json` 的
  `project_version` 一致（版本三来源一致，上传时无需另给 `--version`）

### 平台本地校验（已跑通）

与 `python mosaico.py project upload` 上传前完全相同的检查，不联网：

```bash
# 包内容检查（IRIS_PYTHON = 工作区固定版本）
<iris-python> <workspace>/submodule/esp-mosaico-utils/ESP-Iris/components/esp_iris/tools/esp_iris.py \
    bundle --json inspect <bundle>

# 平台规则校验（mosaico_cli.inspect_bundle_plan + validate_bundle）
python <workspace>/.../precheck_bundle.py
```

结果：`平台本地校验通过 ✓`。本地通过不等于服务器必定接受，最终以平台校验为准。

### 需要重打包时

```bash
# 只打包、不碰设备（工作区自己的打包入口）
python <workspace>/.../build_bundle.py          # 内部调用 system-update-bundle 目标
```

关键：ESP-IDF 工具路径为 `D:\esp\.espressif`（`IDF_TOOLS_PATH`）。当前 shell 若没设这个变量，
`idf_tools.py export` 会报「tool … has no installed versions」而打包失败。完整调用方式：

```powershell
$env:IDF_TOOLS_PATH = 'D:\esp\.espressif'
$env:IDF_PATH       = 'D:\esp\esp-idf-s31'
python <workspace>/.../build_bundle.py
```

---

## 2. 材料二：项目描述（Markdown）

把下面整段粘到「③ 项目描述」的**编辑**页签（右上是 Markdown 预览）。图片来源见第 4 节。

````markdown
# PC 状态面板

把 Windows 电脑的实时硬件状态显示在 ESP-Mosaico 的 480×480 触摸屏上：**CPU / 内存 / GPU / 网络**
四张卡片，点开还有折线趋势。设备通过 Wi-Fi 直接接收电脑上报的状态数据 —— **不投屏、不传图像**，
每秒一包（约 200 字节），对电脑和网络的负担都很小。

![主面板](design/dash_gpu.png)

## 面板上有什么

| 卡片 | 显示内容 |
| --- | --- |
| **CPU** | 占用率（大号数字）+ 温度、功率、频率 |
| **内存** | 已用容量 / 总容量、已用百分比 |
| **GPU** | 占用率 + 显存占用 |
| **网络** | 实时下载 / 上传速率（MB/s） |

- **详情页**：点任意卡片进入。顶部是实时读数，下面两张折线图 —— CPU 是「温度 + 占用」、
  内存是「占用 + 已用容量」、显卡是「温度 + 显存」、网络是「下载 + 上传」，每张图都标注本次
  窗口内的最低 / 最高值。
- **设备信息页**：设备名称、型号、固件版本、设备 IP、运行时长、可用内存，以及电脑连接状态
  （Wi-Fi 信号强度、电脑名、系统、电脑 IP、累计收到的包数）。
- **触摸配网**：第一次开机在屏幕上选 2.4G 网络、用软键盘输入密码，凭据保存在设备里，断电不丢。
  Wi-Fi 列表里会标出已连接的网络与信号强度。
- **自动转屏**：按重力方向自动旋转，横放竖放都合适。
- **时钟与运行时长**：顶部显示电脑名与系统，右下角显示时钟（时间由电脑下发校准，不需要板载 RTC）。

![连接无线网络](design/wifi_page.png)

![CPU 详情页](design/detail_cpu.png)
![网络详情页](design/detail_net.png)
![设备信息页](design/info_page.png)

## 电脑端（Windows 客户端）

- 一个窗口 + **一个托盘图标**：关掉窗口就缩到托盘继续上报，托盘颜色表示连接状态，右键可
  「显示窗口 / 退出」。
- 传感器**在客户端进程内直读**（`LibreHardwareMonitorLib` + pythonnet），不需要单独安装或常驻
  LibreHardwareMonitor，托盘里不会多出别的图标。
- 采集方式：`psutil`（CPU / 内存 / 网络）+ `typeperf`（GPU 占用）+ LibreHardwareMonitor
  （温度、功率、显存）。
- 可以打包成**单文件 exe**，拷到其他电脑上双击即用，目标电脑不用装 Python。

![PC 端客户端](design/gui_window.png)

## 快速上手

1. **给设备联网**：设备开机若未联网会进入配网页 —— 选择 2.4G Wi-Fi、输入密码。
2. **在电脑上运行客户端**：填入设备屏幕上显示的 IP（端口默认 `33340`），点「开始上报」。
3. **配对**：客户端窗口里有一个 4 位配对码；设备屏幕上点「匹配」，用数字键盘输入这 4 位码。
   配对成功后设备只显示这台电脑的数据，同一网段有多台电脑也不会串台。想换电脑时，在设备信息页
   点「重新配对」再输入新电脑的码即可。
4. 完成后设备每秒刷新一次；如果电脑侧 5 秒没有数据，设备会显示「等待连接」。

## 硬件与运行条件

| 项目 | 要求 |
| --- | --- |
| 开发板 | ESP-Mosaico（ESP32-S31，480×480 触摸屏） |
| 网络 | 设备使用 2.4G Wi-Fi；设备与电脑需能互通（默认 UDP 端口 `33340`），跨网段时在客户端里填设备 IP |
| 电脑 | Windows 10 / 11（x64） |
| 设备端依赖 | `espressif/esp-gsp 1.5.1`、`lvgl 9.5.0`、ESP-Mosaico BSP、ESP-Iris |

## 已知限制

- 电脑端目前只有 **Windows** 版本。温度 / 功率 / 显存等需要 LibreHardwareMonitor 的内核驱动，
  **第一次读取会弹一次 UAC 授权**；不授权也能用，只是这几项显示为 `--`。
- GPU 占用率走 Windows 性能计数器（`typeperf`），个别显卡 / 驱动不提供该计数器时显示 0。
- 网络速率只统计物理网卡，已排除回环与虚拟网卡。
- 一次只绑定一台电脑；多台电脑之间目前需要手动「重新配对」切换。
- 目前只在一台 Windows 10 电脑 + 一块 ESP-Mosaico 上实测通过（面板、详情页、配对、托盘客户端
  均已跑通）；其他 Windows 版本、显卡品牌与路由器环境尚未逐一验证。

## 许可与致谢

- 应用代码：Apache-2.0（见仓库 `LICENSE`）。界面字体是从系统字体切出的子集，再分发前请替换，
  说明见 `ui/fonts/NOTICE.md`。
- 移植自开源项目 [ESP32C3-PC-Wireless-Display](https://github.com/WuyongLab/ESP32C3-PC-Wireless-Display)
  的「电脑硬件状态显示」能力，按 480×480 触摸屏重新设计了界面，并去掉了桌面投屏部分。
- 传感器读取使用 [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)
  的库（MPL-2.0）。
````

---

## 3. 表单其余字段

机器可读版本：`projects/pc_display/mosaico-ideas.json`（schema `mosaico-ideas/project-upload/v1`，已通过 CLI 本地校验）。

| 字段 | 值 |
| --- | --- |
| 标题 | `PC 状态面板` |
| 分类 | `display`（显示）、`communication`（通信） |
| 扩展板 | 无 —— 不需要 camera / ethernet / battery |
| 标签 | 电脑监控、硬件状态、CPU、内存、GPU、网络速率、温度、Windows、Wi-Fi、UDP、桌面小屏、托盘 |
| 许可 | Apache-2.0（应用代码）。界面字体为系统字体子集，再分发前需替换，说明见 `ui/fonts/NOTICE.md`。 |
| 外链 | label：`GitHub：esp-mosaico-pc-panel`；url：`https://github.com/LaiGZ/esp-mosaico-pc-panel`；platform：`github` |
| 更新说明（changelog） | 首个版本：四张状态卡片与详情趋势页、设备信息页、Wi-Fi 触摸配网、4 位配对码绑定电脑、重力自动转屏；电脑端提供窗口 + 托盘客户端，传感器进程内直读，可打包单文件 exe。 |

---

## 4. 封面图片

6 张，全部是**真机/真客户端截图**（在 `projects/pc_display/design/`，也已在 `mosaico-ideas.json` 的 `covers` 中声明）。

| # | 文件 | 内容 |
| --- | --- | --- |
| 1（主图） | `design/dash_gpu.png` | 主面板：四张卡片 + 电脑名 + 运行时长 + 时钟 |
| 2 | `design/detail_cpu.png` | CPU 详情页：实时读数 + 温度 / 占用两张折线 |
| 3 | `design/detail_net.png` | 网络详情页：下载 / 上传两张折线 |
| 4 | `design/info_page.png` | 设备信息页：设备信息 + 电脑连接状态 |
| 5 | `design/gui_window.png` | Windows 客户端：已连接设备信息 + 四张卡片 + 设备地址栏 |
| 6 | `design/wifi_page.png` | 触摸配网：Wi-Fi 列表（已连接网络带标记与信号强度） |

⚠️ **这些截图里含真实信息**：电脑名 `DESKTOP-ISVHFCN`、局域网 IP（`192.168.30.207` /
`192.168.211.74`）、附近 Wi-Fi 名称（xtdevice / CDS / CDS-GUEST / MAKAPLANET …）。
如果这个项目要对**所有人公开**，建议先在图片上做遮挡（或换一套脱敏截图）——需要的话我可以直接生成遮挡版。

---

## 5. 上传步骤

### 方式 A：网页手工（第一次发布推荐）

1. 打开 <https://mosaico-ideas.espressif.com>，新建项目，填第 3 节的字段、传第 4 节的封面。
2. 「② 固件文件」→ **添加 Iris 固件** → 选
   `D:\laigz\esp32\tools\mosaico-ideas\pc_status_panel-1.0.0.irisfw`。
3. 「③ 项目描述」→ 编辑页签 → 粘贴第 2 节的 Markdown 全文。
   *注意*：编辑器不会自动读取本地图片，正文里的 6 张图需要用编辑器的图片按钮重新上传一次
   （内容与封面相同）；不然正文图会显示为裂图。想省这一步就用方式 B。
4. 核对版本号填 `1.0.0`，保存为草稿，确认无误后提交审核。

### 方式 B：CLI（自动上传封面/正文图并改写图片链接）

```bash
cd <workspace>            # 含 mosaico.py 的工作区根目录
python mosaico.py account login --server https://mosaico-ideas.espressif.com --open-browser
#  → 你在浏览器里确认授权，不要让任何脚本代你批准

python mosaico.py project upload \
  --project projects/pc_display \
  --server https://mosaico-ideas.espressif.com \
  --create
```

- `--create` 表示**新建**应用（不要用于更新已有 UUID）；交互环境下不加它 CLI 会问你。
- 这条命令**默认会重新构建**，然后上传成一个**草稿**。
- 想用现成的包（不重建）：加 `--skip-build`；想指定包：`--bundle <abs path>`（两者互斥）。
- 只有在"已确认服务器、账号、版本和创建动作"时才加 `--yes`；它不是安全检查的替代品。
- 上传成功后 CLI 会回报 `application_id` / `revision_id` / `version` / `bundle_sha256` /
  `status: "draft"` / `draft_url` —— **草稿 ≠ 已发布**，公开上架仍需你在网站上提交审核。

---

## 6. 发布前待决定 / 待办

| # | 事项 | 说明 |
| --- | --- | --- |
| 1 | **GitHub 外链指向私有仓库** | 现在页面上的外链会 404。要么把仓库转公开（我可以一条命令改），要么把 `external_links` 删掉 |
| 2 | **描述正文里的图片** | 手工上传的话需要再传一次；或直接走方式 B |
| 3 | **截图脱敏** | 见第 4 节的 ⚠️；要公开建议遮挡，我可以生成遮挡版 |
| 4 | 内存 / 显卡详情页截图 | 目前没有可用截图（详见第 7 节），要补需要临时打开 `main.c` 的 `DEBUG_OPEN_DETAIL_ON_BOOT` 编译并烧一次设备 |
| 5 | changelog 措辞 | 现在是「首个版本」；如果平台要求更短，可以只留第 1 行 |

---

## 7. 本地校验与变更记录

**已完成（全部不联网、不写设备）：**

- `system-update-bundle` 按当前源码增量重建 → 三个组件 SHA-256 与上一版完全一致（可复现）。
- `esp_iris.py bundle --json inspect` 检查通过：schema / 芯片 / 组件 / flags / 版本均符合平台要求。
- `mosaico_cli` 的 `inspect_bundle_plan` + `validate_bundle` 平台规则校验通过。
- `load_manifest` 元数据校验通过：6 张封面、6 个待上传文件（120,030 B）、README 2,466 字符。
- 截图纠正：`design/detail_mem.png`、`detail_gpu.png` 实际拍的是**网络**详情页；
  `pair_page.png`、`match_page.png` 实际是配网失败 / 连接中页 —— 已改名为
  `debug_net_detail_1.png`、`debug_net_detail_2.png`、`debug_wifi_link_failed.png`、
  `debug_wifi_connecting.png`，避免被误当成对应页面使用。
  （副作用：暂时没有内存 / 显卡详情页的可用截图，所以正文只放了 CPU 与网络两张详情图。）
- 应用目录补了 `LICENSE`（Apache-2.0，与仓库内一致）；`README.md` 首句改成「本项目把……」。
- GitHub 仓库同步：`5c2fd4b`（初始）→ `f32a365`（发布元数据 + 截图改名）→ `1ff0e1c`（描述排版）。

**没有做：** 没有登录 Mosaico Ideas、没有上传、没有下载/擦写设备、没有提交审核、没有改任何生产配置。

---

## 8. 相关文件索引

| 文件 | 用途 |
| --- | --- |
| `D:\laigz\esp32\tools\mosaico-ideas\pc_status_panel-1.0.0.irisfw` | **要上传的固件** |
| `D:\laigz\esp32\tools\mosaico-ideas\Mosaico-Ideas-发布材料.md` | 本文档（发布工作目录副本） |
| `vibe\projects\pc_display\docs\mosaico-ideas-release.md` | 本文档（工作区/仓库正式位置） |
| `vibe\projects\pc_display\mosaico-ideas.md` | 项目描述源文件（`content.readme`） |
| `vibe\projects\pc_display\mosaico-ideas.json` | 上架元数据（标题/分类/标签/许可/外链/封面/changelog） |
| `vibe\projects\pc_display\build\pc_display-system-update.irisfw` | 构建产出的源包 |
| `vibe\projects\pc_display\design\*.png` | 封面与截图 |
| `vibe\projects\pc_display\README.md` | 项目完整技术文档（架构、协议、已知坑） |

---

## 附：实测情况（供审核说明，不要写成预期能力）

**已实测通过：** 设备端面板四张卡片实时刷新；CPU / 网络详情页折线；设备信息页；Wi-Fi 触摸配网；
4 位配对码绑定（含错误码提示、重新配对）；重力转屏；电脑端窗口 + 托盘客户端；打包成单文件 exe 后
在另一台电脑上接入并驱动设备刷新（该 exe 已修复"无控制台时子进程闪黑框"问题）。

**未验证：** Windows 11、AMD / Intel 核显等其它 GPU 的性能计数器可用性；多台电脑同时在线的切换体验；
长期稳定性（数天连续运行）；其它路由器/AP 环境。

**设备与客户端身份（本次实测）：** ESP-Mosaico（ESP32-S31，480×480，CO5300 触摸屏），
应用 `1.0.0`，固件模式 `normal`，`crash_count 0`；客户端 Windows 10 x64。
