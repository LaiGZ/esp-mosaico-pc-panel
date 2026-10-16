# UI 字体说明

`pc-ui.ttf` / `pc-ui-bold.ttf` 由 `make_fonts.py` 自动生成，只包含 `../main.json`
场景里实际用到的字形（174 个），因此每个文件仅约 32 KB。

## 生成方式

```sh
python ui/fonts/make_fonts.py
```

脚本会：读取 `ui/main.json` 中所有静态文本 → 合并可打印 ASCII 与少量符号
（`° ℃ ↓ ↑ · … —`）→ 用 fontTools 从系统 CJK 字体中抽取子集。

**修改界面文字后必须重新运行**，否则新增的中文字形不会被包含，屏幕会显示空白方块。

## 字体来源与授权

默认源字体为 Windows 自带的 **DengXian（等线）**：`C:\Windows\Fonts\Deng.ttf`（常规）与
`Dengb.ttf`（粗体）。

> ⚠️ DengXian 属于微软随系统授权的字体，**不可再分发**。当前配置仅适合本机开发与调试。

对外发布前请改用可自由分发的 CJK 字体，例如
[Noto Sans SC](https://github.com/notofonts/noto-cjk)（SIL OFL 1.1）：把
`make_fonts.py` 里的 `SRC_REGULAR` / `SRC_BOLD` 指向下载的 TTF，重新生成即可。
