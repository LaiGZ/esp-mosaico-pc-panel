# PC 状态面板 · Mosaico Ideas 发布材料

> 目标站点：<https://mosaico-ideas.espressif.com>
> 应用版本：**1.0.0** ｜ 设备：ESP-Mosaico（ESP32-S31）｜ 材料生成：2026-10-16，最近更新：2026-10-19（仓库开源 + GitHub Release v1.0.0）
> 状态：**材料已备齐并通过本地校验，尚未上传**（上传会生成草稿，是否提交审核由你来点）

---

## 0. 一页速览

| 表单栏位 | 你要交的东西 | 在哪里 |
| --- | --- | --- |
| ② 固件文件 → **添加 Iris 固件** | `pc_status_panel-1.0.0.irisfw`（1.23 MiB） | `D:\laigz\esp32\tools\mosaico-ideas\` |
| ③ 项目描述 | 下面的 Markdown 全文 | 见 [第 2 节](#2-材料二项目描述markdown) |
| ① 项目信息（标题 / 分类 / 标签 / 许可 / 外链 / 封面） | 见 [第 3 节](#3-表单其余字段) 与 [第 4 节](#4-封面图片) | 机器可读版：`projects/pc_display/mosaico-ideas.json` |

---

## 1. 材料一：固件（Iris 包）

### 交付文件

| 项目 | 值 |
| --- | --- |
| 路径 | `D:\laigz\esp32\tools\mosaico-ideas\pc_status_panel-1.0.0.irisfw` |
| 大小 | 1,288,694 B（1.23 MiB；平台上限 32 MiB） |
| SHA-256 | `770796f00fa449590740a76a0f8a8bae53c92fa2147f85fb9ce7d04627bd181b` |
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
| `application`（→ `ota_0`） | `0x210000` | 1,829,584 B | `e889da9c…3db2d1` |
| `data`（→ `ui_apps`） | `0xF00000` | 331,664 B | `b7f928a1…a6cef` |

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

**纯描述文件（推荐用于网页粘贴）**：`D:\laigz\esp32\tools\mosaico-ideas\项目描述.md`
—— 约 2.5 KB，只有描述本身：没有表格、没有本地图片引用，整段粘进「③ 项目描述」编辑器不会出现
裂图，也不会因为编辑器不支持表格而显示成一堆 `| --- |`。

**带图版本（CLI 上传用）**：`vibe\projects\pc_display\mosaico-ideas.md`
—— 内容同上，但带 6 张本地图片引用；`python mosaico.py project upload` 会自动上传这些图片
并改写链接。

> 两份只有"图片引用"不同。正文里的图已经在「封面」的 6 张里，所以纯描述不插图不影响观感。
>
> **为什么没有表格**：部分 Markdown 编辑器不启用 GFM 表格扩展，表格会原样显示成 `| --- |`。
> 两份描述现在都用列表写法（`- **CPU** —— …`），任何渲染器都能正常显示。

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
| 1 | ~~GitHub 外链指向私有仓库~~ | ✅ 已解决：仓库已转为公开（<https://github.com/LaiGZ/esp-mosaico-pc-panel>） |
| 2 | **描述正文里的图片** | 手工上传的话需要再传一次；或直接走方式 B |
| 3 | **截图脱敏** | 见第 4 节的 ⚠️；要公开建议遮挡，我可以生成遮挡版 |
| 4 | 内存 / 显卡详情页截图 | 目前没有可用截图（详见第 7 节），要补需要临时打开 `main.c` 的 `DEBUG_OPEN_DETAIL_ON_BOOT` 编译并烧一次设备 |
| 5 | changelog 措辞 | 现在是「首个版本」；如果平台要求更短，可以只留第 1 行 |
| 6 | ~~客户端下载链接~~ | ✅ 已解决：GitHub Release `v1.0.0` 已上传 zip，描述与 `external_links` 都已补好，见第 8 节 |

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
- **仓库开源**：`PATCH /repos/…{"private": false}` → 仓库地址 <https://github.com/LaiGZ/esp-mosaico-pc-panel> 可公开访问。
- **GitHub Release `v1.0.0`**：以当前 `main` 为提交、把 `pc_status_panel-1.0.0-win64.zip`
  作为资产上传；资产直链
  <https://github.com/LaiGZ/esp-mosaico-pc-panel/releases/download/v1.0.0/pc_status_panel-1.0.0-win64.zip>
  （已在项目描述与 `external_links` 中引用）。

**没有做：** 没有登录 Mosaico Ideas、没有上传、没有下载/擦写设备、没有提交审核、没有改任何生产配置。

**2026-10-19 追加：**

- **界面调整**：首页右上角改为**设备电量**（板载 BQ27220 电量计，15 秒轮询、仅变化时刷新；读不到显示 `--`），
  「连接」按钮移到**设备信息页右上角**。场景自检 `178 objects / page_info children 30` 通过，
  已 `system-update` 装到设备并用截图确认（100%，4189 mV / 0 mA / 30.4 °C，即电池是满的），
  封面主图 `design/dash_gpu.png` 已换上带电量显示的新截图。
- **主机环境修复（很重要）**：运行 `mosaico.py` 的 Python 缺少 `pyserial`，导致 ROM 恢复通道
  检测不到设备（报 `No ESP-Mosaico ROM Download Mode interface was detected`）。`pip install pyserial`
  后恢复可用；设备已用 `recover` 从 ROM 下载模式救回（底座固件 0.1.4），NVS 里的 Wi-Fi 凭据与
  配对信息都保留（面板照常收到这台电脑的数据），随后用 `system-update` 安装本应用。

---

## 8. Windows 客户端 exe 包（下载分发）

| 项 | 值 |
| --- | --- |
| 分发压缩包 | `D:\laigz\esp32\tools\pc_status_panel\pc_status_panel-1.0.0-win64.zip`（27.9 MiB） |
| 包内文件 | `pc_status_panel.exe`（28.17 MB）+ `使用说明.txt` + `第三方许可.txt` + `LICENSE.txt` |
| ZIP SHA-256 | `5b3c6ff808a51bb853cde431614418cf8a74cdf457420493438324b3f0e92288` |
| EXE SHA-256 | `93bbfe51dcab016adbd953229f9cfebfb382dec30d39c989caac3254dd71a5bd` |
| 打包时间 | 2026-10-16 18:11 构建的 exe（含"无控制台子进程不闪黑框"修复），2026-10-19 打包 |
| 依赖 | 无 —— Python 运行时、psutil、pythonnet、pystray、LibreHardwareMonitorLib 全部已内置 |

| 分发方式 | **GitHub Release `v1.0.0`**（公开下载） |
| 下载页 | <https://github.com/LaiGZ/esp-mosaico-pc-panel/releases/tag/v1.0.0> |
| 资产直链 | <https://github.com/LaiGZ/esp-mosaico-pc-panel/releases/download/v1.0.0/pc_status_panel-1.0.0-win64.zip> |

**它不进平台的「附件」**：平台限制单个资料 ≤ 20 MiB，而这个 exe 本身已经是压缩过的 onefile 包，
再 zip 也有 27.9 MiB ✗。所以客户端走 **GitHub Release**（资产上限 2 GB），项目描述与
`external_links` 里直接给下载链接。（真要塞进附件只能 UPX 再压，容易招杀软误报，不建议。）

**上架前的准备**（和固件一起打包，用户下载后解压即用）：

1. 解压到一个**可写**目录（不要放 `C:\Program Files`），双击 `pc_status_panel.exe`；
2. 填设备 IP + 端口 33340 → 开始上报 → 用窗口里的 4 位配对码在设备上配对；
3. 首次运行会有 SmartScreen「未知发布者」提示（未签名），以及第一次读温度时的一次 UAC。

---

## 9. 相关文件索引

| 文件 | 用途 |
| --- | --- |
| `D:\laigz\esp32\tools\mosaico-ideas\pc_status_panel-1.0.0.irisfw` | **要上传的固件** |
| `D:\laigz\esp32\tools\mosaico-ideas\Mosaico-Ideas-发布材料.md` | 本文档（发布工作目录副本） |
| `D:\laigz\esp32\tools\pc_status_panel\pc_status_panel-1.0.0-win64.zip` | **Windows 客户端分发包** |
| `vibe\projects\pc_display\docs\mosaico-ideas-release.md` | 本文档（工作区/仓库正式位置） |
| `vibe\projects\pc_display\mosaico-ideas.md` | 项目描述源文件（`content.readme`） |
| `vibe\projects\pc_display\mosaico-ideas.json` | 上架元数据（标题/分类/标签/许可/外链/封面/changelog） |
| `vibe\projects\pc_display\build\pc_display-system-update.irisfw` | 构建产出的源包 |
| `vibe\projects\pc_display\agent\build_exe.py` | 重新打包 exe 的脚本 |
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
