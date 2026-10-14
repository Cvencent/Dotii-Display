# Dotii 桌面交互屏

Dotii 是一套由 ESP32-S3 圆形 AMOLED 桌面屏、CodexBridge 与小智 AI 组成的开源桌面对话终端。设备固件原生运行小智语音助手，内置可切换形象与表情状态机，既能随对话与触摸切换表情，也能显示 Codex 状态。CodexBridge 是常驻托盘、以通用设备协议提供 Codex 额度、近 7 日 Token、任务状态与多设备已读同步的桥接器；Dotii 管理中心是可安装到本机的独立后台程序，只管小智连接、小智表情、自定义页面、显示设置、BLE 配网与烧录。

![Dotii 桌面交互屏产品渲染图](assets/dotii-product-render.png)

[Windows 下载](https://github.com/Cvencent/Dotii-Display/releases/tag/management-center-v1.1.3) · [固件 1.1.4](https://github.com/Cvencent/Dotii-Display/releases/tag/firmware-v1.1.4) · [MakerWorld 模型与打印文件](https://makerworld.com.cn/zh/models/2918764-dotii-zhuo-mian-jiao-hu-ping#profileId-3421401) · [开发指南](开发指南.md)

## 选择你的系统

| 系统 | 支持范围 | 下载内容 | 当前状态 |
| --- | --- | --- | --- |
| Windows | Windows 10/11 x64 | [CodexBridge Releases](https://github.com/Cvencent/CodexBridge/releases) | 正式版 |
| macOS | Apple Silicon、macOS 13 及以上 | 当前预览包仍由 Dotii-Display Release 提供 | 迁移到 CodexBridge 打包前的过渡版本 |

macOS 预览版目前不支持 Intel Mac。由于尚未经过苹果公证，首次打开时需要在“系统设置 > 隐私与安全性”中手动允许。发布包会包含运行所需组件，普通用户无需安装 Python、Node.js、Codex CLI 或 ESP-IDF。

## 使用前准备

| 类别 | 项目 | 数量 | 购买/资料链接 |
| --- | --- | ---: | --- |
| 核心硬件 | 微雪 Waveshare ESP32-S3-Touch-AMOLED-1.75 开发板（含 1.75 英寸圆形屏，无外壳） | ×1 | [淘宝购买链接](https://e.tb.cn/h.8msjONXF0M0Vdg1?tk=X4anTUTbPSP) · [官方说明文档](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-1.75/) |
| 供电 | 3.7 V、400 mAh 602030 锂电池，带 **MX1.25 2P 插头** | ×1 | [拼多多购买链接](https://mobile.yangkeduo.com/goods.html?ps=nMO2vEcuRY) |
| 结构配件 | Type-C 弯公头转母头，用于底座内的接口引出 | ×1 | [拼多多购买链接](https://mobile.yangkeduo.com/goods.html?ps=m4Bzx5p2iF) |
| 紧固件 | M2 螺丝，长度 4–7 mm 均可 | ×3 | — |
| 3D 打印件 | 按 MakerWorld 模型打印的外壳、底座、按钮等结构件 | ×5 | [MakerWorld 模型、打印文件和装配资料](https://makerworld.com.cn/zh/models/2918764-dotii-zhuo-mian-jiao-hu-ping#profileId-3421401) |
| 连接与网络 | 支持数据传输的 USB 线、2.4 GHz Wi-Fi | 各 ×1 | — |

## Windows 快速上手

1. 从 [CodexBridge Releases](https://github.com/Cvencent/CodexBridge/releases) 下载并运行 `CodexBridge.exe`。它是常驻系统托盘的 Codex 桥接器，负责 Codex 采集与设备协议，默认管理接口为 `http://127.0.0.1:8788`。
2. 从本仓库 Releases 下载 `Dotii-Setup-1.1.3.exe` 并安装到本机；也可以继续使用便携版。它与 CodexBridge 是两个独立程序，只负责设备管理。安装后可在系统托盘右键选择“关于 Dotii 管理中心”查看版本。
3. 用 USB 线连接 Dotii，在管理中心“设置”中识别设备。首次使用时可通过“一键烧录”写入随包固件。
4. 使用蓝牙配网，将 2.4 GHz Wi-Fi 同步到 Dotii，并写入与 CodexBridge 配对的设备令牌。
5. 在管理中心“小智”中填写小智服务端地址与唤醒词，设备即可直接进行语音对话。
6. 在 CodexBridge 托盘中按设备启用 `quota` 能力，Codex 额度与近 7 日 Token 会通过通用协议下发到设备。
`CodexBridge.exe` 常驻托盘，负责 Codex 采集、统一协议、设备能力开关与已读同步；`DotiiManagementCenter.exe` 负责刷机、蓝牙配网、小智、表情与显示设置，关闭后不常驻。Windows 安装版默认安装在 `%LOCALAPPDATA%\Programs\Dotii Management Center`，并提供开始菜单、桌面快捷方式和卸载入口。

### 更新 Windows 管理中心

先从系统托盘彻底退出旧版 CodexBridge，安装新版；再单独更新 `DotiiManagementCenter.exe`。CodexBridge 的设备配对、能力与语音设置保存在 `%LOCALAPPDATA%\CodexBridge`；Dotii 的小智连接、小智表情、自定义页、显示、配网与烧录配置继续保存在 `%LOCALAPPDATA%\StateDisplay`。若新版本包含 Dotii 固件更新，请在管理中心重新执行“一键烧录”。

## macOS 快速上手

1. 从 [macOS Release](https://github.com/Cvencent/Dotii-Display/releases/tag/management-center-v1.1.3) 下载 DMG。当前版本仅支持 Apple Silicon Mac 和 macOS 13 及以上。
2. 打开 DMG，将 `DotiiManagementCenter-1.1.3.app` 拖到“应用程序”。
3. 从“应用程序”打开 Dotii 管理中心。如果系统阻止启动，请打开“系统设置 > 隐私与安全性”，在对应提示旁选择“仍要打开”，然后再次确认。
4. 首次扫描 Dotii 时允许蓝牙权限；连接 Dotii 时按系统提示允许本地网络权限。
5. 应用启动后会驻留在菜单栏。后续烧录、蓝牙配网及模块设置均在管理页面中完成。

不需要也不建议全局关闭 macOS 的安全检查。受管理的公司或学校 Mac 可能禁止打开未经公证的应用。

### 可选：校验下载文件

将 DMG 和 `.dmg.sha256` 文件放在同一目录，在终端进入该目录并执行：

```bash
shasum -a 256 -c DotiiManagementCenter-macOS-arm64-1.1.3.dmg.sha256
```

结果应显示 `OK`；如果校验失败，请不要打开该 DMG。

### 更新与卸载

更新前先从菜单栏退出 Dotii 管理中心，再用新版 App 替换“应用程序”中的旧版。运行配置保存在 `~/Library/Application Support/Dotii`，替换或删除 App 不会自动清除配置。

卸载时先退出应用，再将 App 移到废纸篓。如果不再需要原有配置，可以手动删除上述运行目录；其中可能包含 Wi-Fi 密码、小智服务地址和设备连接信息，请勿上传或分享。

## 主要功能

- **小智 AI**：设备固件内原生运行小智语音助手，直接与配置的小智服务端建立 WebSocket 会话，不依赖电脑端中转。
- **小智表情**：内置表情状态机，随对话、连接和触摸切换不同表情；支持在管理页切换 Dotii、Nimbo、Twinkle、Blob 四种形象。
- **Codex 状态**：由 CodexBridge 通用协议采集并广播额度、近 7 日 Token、额度重置、任务状态、计划进度和用户可见消息；多台设备共享已读状态。
- **自定义页面**：编辑文字、颜色、图片和圆环，保存后同步到 466 × 466 圆屏。
- **设备管理**：提供蓝牙配网、重置配网、显示设置、休眠设置、登录自启动和受保护的一键烧录。

小智默认启用，首次连接时会通过小智官方 OTA 服务获取设备配对码；请在 `xiaozhi.me` 输入设备码完成绑定。Codex 能力仍按设备能力开关启用，未启用前不会读取或下发 Codex 数据。

## 形象与表情来源

Dotii 的原生表情引擎与可切换形象参考并移植自开源项目 [aora-bot](https://github.com/sam70361/aora-bot)（其 `mood-mates` 角色资料）。其中 Dotii 形象为本项目自研，Nimbo、Twinkle、Blob 三个形象与对应表情几何来自 aora-bot。

本项目为个人开源固件项目，仅用于非商业用途。使用或二次分发时，请一并遵守 aora-bot 仓库的授权与署名要求；如 aora-bot 后续更新许可，请以原仓库为准。

## 与 CodexBridge 的分工

Dotii 固件内置一套通用设备协议客户端，任何实现同一协议的设备都可以接入 CodexBridge。CodexBridge 是独立的数据主机，负责采集 Codex 额度、近 7 日 Token、额度重置与任务状态，并按设备订阅的能力（如 `quota`）下发 `codex_status` 消息。

- 小智对话、表情、自定义页面、显示与配网由 Dotii 管理中心负责，写入设备本地配置。
- Codex 数据不再由管理中心采集，也不会经过管理中心转发，而是由设备直接从 CodexBridge 读取。
- 该协议保持通用，后续的 AI Passport 等设备同样可以按能力接入。
- 传输层固定为 UDP `8764` 设备发现与 TCP `8765` 会话，消息为每行一条 UTF-8 JSON，由 CodexBridge 主动连接设备。

## 常见问题

大多数软件问题可以借助 Codex 等 AI 工具排查和解决；如果需要增加新功能，也可以借助 AI 完成开发。

<details>
<summary><strong>管理页面没有自动打开</strong></summary>

再次运行 `DotiiManagementCenter.exe` 即会打开管理页面；也可以在同一台电脑上访问 `http://127.0.0.1:8787`（需要管理中心正在运行）。

</details>

<details>
<summary><strong>Codex 页面没有数据</strong></summary>

先确认 CodexBridge 正在运行，并在托盘中为该设备启用所需能力。接口没有提供的数值会显示为 `--`。

</details>

<details>
<summary><strong>设备会自动熄屏或休眠</strong></summary>

这是固件的电源策略，可以在管理中心的“屏幕与睡眠”里分别设置“用电池”和“充电中”两套时间。默认值是：电池供电 60 秒熄屏、300 秒进入深度休眠；USB 供电只熄屏、不进入深度休眠，这样 Wi-Fi、蓝牙和 CodexBridge 会一直保持在线。进入深度休眠后触摸屏幕或按按键即可唤醒，唤醒后重新联网需要几秒。

</details>

<details>
<summary><strong>小智无法唤醒或没有回复</strong></summary>

先在管理页“小智”中确认服务端地址、唤醒词与音色，并确保设备已连接 2.4 GHz Wi-Fi。表情由设备固件本地渲染，即使语音服务暂时不可用，触摸与状态表情依然会响应。

</details>

<details>
<summary><strong>Codex 额度没有出现在设备上</strong></summary>

Codex 数据由 CodexBridge 提供。请在 CodexBridge 托盘中为这台 Dotii 启用 `quota` 能力，并确认设备已用配对密钥连接。接口没有提供的数值会显示为 `--`，不会自动推算。

</details>

<details>
<summary><strong>管理中心找不到 Dotii</strong></summary>

更换支持数据传输的 USB 线后重新扫描。若设备未进入下载模式，可按开发板说明使用 BOOT 按键后重试。程序只会向识别为 Dotii ESP32-S3 的设备开放烧录。

macOS 用户还应在“系统设置 > 隐私与安全性 > 蓝牙”中确认已允许 Dotii 管理中心使用蓝牙。

</details>

<details>
<summary><strong>蓝牙配网提示配对失败</strong></summary>

1.1.3 起 Windows 会先按“仅加密”等级与 Dotii 建立绑定，再写入加密配置特征。若系统里残留旧的 Dotii 配对，请在“设置 > 蓝牙”中删除后重新扫描再试；macOS 由系统在访问加密特征时自动配对，无需手动操作。

</details>

<details>
<summary><strong>更换路由器或电脑后无法连接</strong></summary>

请长按 Dotii 右侧按钮进入“设置”，再长按“重置配网”1.2 秒。设备会清除 Wi-Fi、管理中心绑定和蓝牙配对后重启；随后在新电脑的管理中心重新扫描并配网，通常无需重新烧录固件。

若系统仍保留旧配对，Windows 1.1.1 会尝试自动恢复；macOS 请在“系统设置 > 蓝牙”中忽略 Dotii 后重新扫描。

</details>

<details>
<summary><strong>如何查找日志</strong></summary>

Windows 的 CodexBridge 日志位于 `%LOCALAPPDATA%\CodexBridge`；Dotii 管理页日志位于 `%LOCALAPPDATA%\StateDisplay\bridge.log`。macOS 过渡版本日志仍位于 `~/Library/Application Support/Dotii/bridge.log`。分享日志前，请检查并移除不希望公开的本机信息。

</details>

## 数据与安全

- 管理网页和管理 API 只允许本机访问；Dotii 读取数据时需要设备访问令牌。
- Wi-Fi 密码、小智服务地址、设备令牌和运行配置保存在本机用户目录，不应上传到公开仓库。
- Codex 额度与用量由 CodexBridge 通过通用协议下发，设备只按自身订阅的能力读取，不接触其他账号数据。
- 常规一键烧录不会擦除 NVS。请勿对未知设备、串口或固件执行烧录。
- Windows 首次允许局域网访问时可能显示防火墙窗口。请允许 CodexBridge 与管理后台访问专用网络，并按实际使用的网络类型授权。

## 开发与二次开发

普通用户不需要准备开发环境。准备修改源码时，请先选择对应文档：

- [通用开发指南](开发指南.md)：项目架构、固件、协议、模块扩展、圆屏交互与通用测试规范。
- [Windows 开发指南](docs/development/windows.md)：Windows 环境、托盘、蓝牙、串口、便携包与发布流程。
- [macOS 开发指南](docs/development/macos.md)：macOS 菜单栏宿主、CoreBluetooth、串口、签名、公证与 DMG 构建。

平台共用固件、后台业务、管理网页和设备协议。平台差异应保留在对应适配器、宿主与打包目录中，不应复制共用业务代码。

固件的内部 RAM 预算很紧：Wi-Fi、BLE 和 AMOLED 面板共用内部 DMA 内存。`sdkconfig.defaults` 因此使用低内存 Wi-Fi 缓冲剖面、让 LVGL 绘制缓冲与 mbedTLS 走 PSRAM，并在 BSP 中静态预留 12 行 DMA 条带。修改 Wi-Fi、蓝牙、LVGL 或 TLS 配置时请保留这些约束，否则设备可能在显示初始化阶段重启。

## 社区贡献与扩展

感谢社区开发者为 Dotii 提供功能扩展与改进。本节列出的实现由贡献者提供；是否已合并及验证范围会单独说明，未合并的功能不属于本项目当前发布版本。

### Z.ai 用量与 Claude Code 工作状态模块

由 [@wfzylcl878-creator](https://github.com/wfzylcl878-creator) 提交，扩展内容包括：

- **Z.ai 用量**：显示 GLM Coding Plan 的 5 小时与周窗口剩余额度、套餐档位和重置时间。
- **Claude Code 工作状态**：通过 hooks 事件显示工作中、完成、等待用户和失败等状态，并支持圆屏页面与 Dotii 表情联动。
- **相关改进**：包含模块状态显示、代理 fake-ip 地址过滤及设置页数据采集状态展示等修复。

**合并与验证状态**：该贡献尚未合并到本项目。维护者目前缺少独立验证条件，暂不合并，仅在此展示并提供参考链接。贡献者在 PR 中报告已完成 macOS 源码测试、固件构建和真机验证，同时明确说明 Windows 尚未实测。

相关链接：[PR #1：新增 Z.ai 用量与 Claude Code 工作状态模块](https://github.com/ZeroOne000011/Dotii-Display/pull/1) · [贡献者仓库](https://github.com/wfzylcl878-creator/Dotii-Display) · [PR 对应实现版本](https://github.com/wfzylcl878-creator/Dotii-Display/tree/529d962aefd3747fc671c16152a1a4df79ec12cb)
