# Dotii 小智 AI + 表情引擎 + CodexBridge 通用协议 - 设计文档

日期：2026-10-13
状态：已批准，进入实现

## 1. 背景与目标

把 Dotii 桌面圆屏从"状态显示器"升级为"能对话、有表情的桌面助手"，并把 Codex 的
额度/用量数据从 Dotii 管理中心搬到独立的 CodexBridge，做成任何设备都能接入的通用
协议（未来 AI Passport 也要接同一套）。

1. 固件内置小智 AI 客户端（不依赖电脑端二次中转），能和小智服务端对话。
2. 固件内置原生表情引擎，把 aora-bot 的 32 种表情体系移植为设备端 C 实现，形象可
   在管理中心切换（Dotii 自研形象 + Aora 角色 + 球形角色）。
3. 小智对话内容、触摸/手势、Codex 业务状态三路来源都能驱动表情。
4. 管理中心去掉拓竹（Bambu）整条线，只保留：小智、小智表情、自定义、Codex、设置。
   Codex 的周剩余、近 7 日 Token、额度重置改由 CodexBridge 通过通用协议直发设备，
   管理中心不再采集 Codex。

## 2. 术语与仓库边界

机器上有两个容易混淆的 bridge，本文严格区分：

- CodexBridge（`D:\CodexBridge`）：唯一 PC 端常驻程序。拥有 Codex 采集、通用设备
  协议宿主（TCP 8765 / UDP 8764）、统一消息与已读、托盘。
- 管理中心（`D:\dotii\bridge`，迁移后由 CodexBridge 托管）：本机网页（8787），负责
  小智配置、表情/形象配置、自定义页、显示设置、BLE 配网与烧录。

面向用户名称继续统一为"Dotii 管理中心"；`bridge_url`、`bridge_token` 只作为协议字段。

## 3. 总体架构

```
                        局域网
小智服务端 -- WebSocket/Opus --+
                               |
CodexBridge -- 通用协议 -------+   Dotii 固件 (ESP32-S3)
  (Codex 采集/配额)   TCP 8765  |   +- xiaozhi_client   对话/唤醒/音频
                               +-->+- emotion_engine    表情状态机（原生 C）
管理中心 --- HTTP/schema v1 ---+   +- emotion_router    多来源合流
  (小智/表情/自定义/设置)          +- emotion_render    LVGL 绘制
                                   +- state_ui          圆屏页面
                                   +- board_input       触摸/按键/手势
```

固件是双数据源：Codex 数据走 CodexBridge 通用协议；小智、表情配置、自定义页、显示
与配网走管理中心 HTTP 快照。两条链路独立，互不阻塞。

## 4. 表情引擎（固件原生 C）

### 4.1 来源与许可

引擎算法与表情数据配置来自 aora-bot（双许可，非商业免费）。本项目为开源固件、非
商业用途，故移植其引擎与 32 表情数据，并在 README 与 NOTICE 中注明来源。形象同时
提供自研形象与 Aora 角色，用户可在管理中心切换。

### 4.2 表情 ID 契约（对外稳定，编号永不重排）

| 区间 | 分组 | 常用 |
| --- | --- | --- |
| 00-09 | 生命周期 | 00 睡眠 · 01 唤醒 · 02 待机 · 03 好奇 · 04 发呆 · 05 加载 |
| 10-29 | 情绪反应 | 10 开心 · 13 惊讶 · 14 害羞 · 15 疲惫 · 16 专注 · 21 生气 |
| 30-49 | 代理工作状态 | 30 思考 · 33 完成 · 34 出错 · 35 等待输入 · 39 回复 · 40 检索 · 41 停止 |
| 50+ | 自定义 | 运行时注册 |

表情 ID 为两位十进制字符串，与 aora-bot 完全一致，便于与网页预览共用同一份数据。

### 4.3 模块划分

- `main/emotion/emotion_catalog.c/.h`：表情目录。按 id 或英文名查表，返回只读的
  `emotion_def_t`（分组、过渡时长、注视开关、眼型池、动画列表、配色语义）。
- `main/emotion/animation.c/.h`：动画原语求值。`sine`/`glance`/`pulse`/`jitter`/
  `scan`/`blink`，输入 (原语, 时间, 相位) 输出标量。纯函数，可在主机上单测。
- `main/emotion/emotion_engine.c/.h`：表情状态机。持有当前表情、进入时间、注视目标、
  眨眼相位；提供 `set_emotion(id/name)`、`tick(dt)`、`state()`。含待机超时回落、
  未知 ID 回落到待机。
- `main/emotion/character_pack.c/.h`：形象包。`character_def_t` = 身体轮廓点环 +
  色板 + 眼型基调 + 五官参数；`character_pack_lookup(id)`。数据驱动。
- `main/emotion/emotion_render.c/.h`：把引擎状态画到 LVGL 对象上（身体->眼环->
  嘴->腮红->特效）。与状态机解耦。
- `main/emotion/emotion_router.c/.h`：多来源合流。优先级：触摸瞬时 > 小智/会话状态 >
  Codex 业务状态 > 待机；带超时回落。

### 4.4 数据驱动

新增表情或形象只写数据（`main/emotion/characters/*.c`），不改引擎。`50+` 段留给
管理中心自定义。

## 5. 小智 AI 客户端

- `main/xiaozhi_client.c/.h`：内置客户端。WebSocket 连接（可配置服务端 URL），处理
  会话握手、文本、服务端下发的 `emotion` 字段与音频流。
- 音频：复用 BSP `bsp_audio_codec_speaker_init()` / `bsp_audio_codec_microphone_init()`
  （ES8311 全双工 I2S）。
- 会话状态（连接/唤醒/聆听/思考/说话/完成/出错）驱动本地表情；服务端 `emotion` 字段
  优先于本地推断。
- 配置：服务端地址、唤醒词、音色、MCP 工具开关由管理中心保存并经 HTTP/BLE 下发。

分期说明：Opus 编解码与唤醒词需要 `esp-sr` 与 Opus 托管组件。本期先交付连接/协议/
表情联动/配置链路与音频管线骨架；Opus 与唤醒词作为明确后续任务（见实现计划 Task 6），
不隐藏为"已完成"。

## 6. 触摸与事件 -> 表情

`board_input.c` 上报手势事件，`emotion_router` 给出瞬时表情（带最短显示时间）：

| 事件 | 表情 |
| --- | --- |
| 轻触 | 13 惊讶 / 触摸回应（可配） |
| 长按 | 35 等待输入（进入聆听） |
| 左/右滑 | 翻页（不切表情） |
| 会话：聆听 | 35 |
| 会话：思考 | 30 |
| 会话：说话 | 39 回复（口型脉冲） |
| 会话：完成 | 33 |
| 会话：出错 | 34 |

映射表在管理中心可配，沿用现有"状态 -> 表情 + 时长"模型；固件本地解析，快照无法
覆盖实时表情。

## 7. CodexBridge 通用协议扩展（配额与用量）

在现有唯一协议上扩展，不新增握手、不新增版本分支：

- 新能力：`quota`。
- 新消息类型 `codex_status`，字段（全部可选，缺失显示 `--`，不外推）：
  - `five_hour_remaining_percent`、`weekly_remaining_percent`
  - `weekly_tokens`、`seven_day_tokens`
  - `five_hour_reset_at`、`weekly_reset_at`（epoch 秒）
  - `plan_type`
  - `task_status`、`thread_id`、`current_action`、`token_count`、`plan_completed`、`plan_total`
- 来源：Codex app-server `account/rateLimits/read` + `account/usage/read`，由
  `codexbridge/codex/` 扩展一个配额视图产出。
- 下发条件：设备在 `bridge_auth_ok.capabilities` 声明 `quota` 且被授权。
- 任何设备（Dotii、AI Passport）实现同一套即可接入。

## 8. 管理中心改造

### 8.1 删除拓竹（Bambu）

移除：导航标签与面板、`bridge/bambu_client.py`、MQTT/TLS 与相机/FFmpeg 链路、快照中
的 Bambu 字段与 `modules.bambu`、相关 API 与测试。删除前用引用搜索确认。

### 8.2 新导航

小智 / 小智表情 / 自定义 / Codex / 设置

- 小智：服务端地址、唤醒词、音色、MCP 工具开关、连接状态。
- 小智表情：形象选择、表情映射与时长、静态预览。
- 自定义：不变。
- Codex：只读展示配额与用量 + 与 CodexBridge 的配对入口；数据不再由管理中心采集。
- 设置：Wi-Fi、显示、休眠、烧录、日志、重置配网，去掉 Bambu 项。

### 8.3 模块开关

快照 `modules` 去掉 `bambu`，保留 `xiaozhi`、`custom`、`dotii`。Codex 开关搬到
CodexBridge。

## 9. schema_version 兼容

设备协议仍为 `schema_version = 1`。新增字段一律可选：旧固件忽略新字段，新固件对缺失
字段回退安全默认值。不兼容变更才提升版本号。

## 10. 安全

- 管理接口只接受回环访问；设备接口校验令牌。
- 字符串、数组、分片有长度上限；日志不写令牌。
- 小智服务端 URL 允许 http/https 与 ws/wss，默认值不含任何密钥。

## 11. 测试与验收

主机单测（Python）：CodexBridge 配额消息编解码与能力过滤、管理中心去 Bambu 后的
快照/配置校验、导航与 API 回归。

固件主机单测（C，gcc）：表情目录查表、动画原语求值、状态机转换与回落、形象包查表、
路由优先级。

构建验收：`idf.py build` 通过；`node --check bridge/web/app.js` 通过。

真机验收（后续执行）：小智对话与表情联动、触摸手势、形象切换、圆屏显示、Bridge 配额显示。

## 12. 非目标

- 不改自定义页编辑能力。
- 不做云端中转；小智与 Bridge 均在同一局域网/直连。
- 本期不做 Opus 与唤醒词真机验收（列为后续任务）。
- 不保留任何设备协议版本分支或并行握手。

## 13. 后续任务

1. 接入 `esp-sr` 唤醒词与 Opus 编解码，完成小智双向语音真机联调。
2. AI Passport 固件最小迁移到同一套通用协议 + `quota` 能力。
3. 更多形象包与自定义表情编辑（管理中心）。
