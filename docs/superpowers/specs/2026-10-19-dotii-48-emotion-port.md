# Dotii 48 表情词汇表替换固件形象（小智页面）- 设计文档

日期：2026-10-19
状态：已批准（用户确认：旧形象整体替换，不做保留）

## 1. 背景与目标

`output/emotion-preview` 已完成全新 Dotii 表情词汇表网页原型：48 个表情
（基础 10 / 情绪 16 / 对话 12 / 系统 10），统一视觉风格（深色脸盘 + 白色五官 +
accent 色特效），每个表情带 base 状态、关键帧时间线、正弦运动通道与特效。
要求把这套表情移植进 ESP32-S3 固件，**整体替换**现有 aora 球形形象，
在小智页面按事件实时渲染。

成功标准：

1. 小智页面在 466x466 圆屏上原生渲染新形象，观感与网页预览一致。
2. 全部 48 个表情可用，由事件驱动：小智会话状态、服务端情绪、触摸、业务来源。
3. 现有协议契约不破坏：两位数字 id、英文 name（llm.emotion）、来源优先级不变。
4. host 测试 + 视觉帧转储验证 + 固件编译通过。

## 2. 现状

- `main/emotion/` 现有 aora-bot 球形系统：catalog（36 表情）、engine、router、
  render（LVGL canvas + SVG 球体几何 emotion_geometry/face/pose/character_pack）。
- 事件链路已通：xiaozhi_client（状态机 + llm.emotion + 文本意图）→
  xiaozhi_client_emotion_policy → emotion_router（touch > xiaozhi > business）
  → emotion_engine → emotion_render → state_ui 小智页面。
- 新词汇表仅存在于 JS（`output/emotion-preview/expressions.js` +
  `face-renderer.js`），数据结构与旧 catalog 完全不同。

## 3. 方案总览（整体替换）

```
output/emotion-preview/expressions.js
        │  tools/gen_dotii_catalog.py（生成器）
        ▼
main/emotion/dotii_data.c/.h      生成数据：48 表情定义 + 旧名→新名 alias 表
main/emotion/dotii_engine.c/.h    时间线/振荡器求值 → dotii_pose_t（含眨眼）
main/emotion/dotii_draw.c/.h      纯 C 光栅核心（不依赖 LVGL，host 可编译）
main/emotion/dotii_render.c/.h    LVGL 胶水（466x466 canvas + 脏矩形）
```

**删除**：emotion_face.*、emotion_pose.*、emotion_geometry.*、character_pack.*、
旧 emotion_render.*、旧 animation.*（连同对应 host 测试）。

**保留并适配**：emotion_router（来源优先级，引擎指针换成 dotii_engine_t）、
emotion_mood（情绪判定逻辑不变，输出指向新表）、state_ui（attach 换 dotii_render）。
管理中心若仍下发旧形象/角色配置字段，固件侧解耦：字段保留但新渲染不再消费。

## 4. 数据与生成器

- 生成器解析 expressions.js 的纯数据部分（JSON 中转），生成：
  - `dotii_expression_t[48]`：id（基础→情绪→对话→系统顺序分配 00-47，永不重排）、
    name、label、group、accent、duration、base（眼/眉/嘴/特效/朝向）、
    timeline 关键帧（16 通道定点量化）、motion 振荡通道、effects 列表。
  - alias 表：旧协议名→新 name（panicked→surprised、done→success、busy→coding、
    down→sad、waking→wake、sleeping→sleep、focused→thinking 等），保证
    llm.emotion 与现有策略层零改动。
- 16 个动画通道：pupilX/Y、eyeOpen、eyeW/H、browY/tilt、mouthOpen/Curve/W、
  blush、headX/Y/Rot/Scale、dim。
- 14 眼形 / 7 眉形 / 10 嘴形 / 25 特效，全部枚举进 C。

## 5. 渲染移植

- dotii_draw：软件光栅 RGB565，逐原语对齐 face-renderer.js：
  脸盘（accent 径向光晕 + rim）、眼形 14 种（star/heart/target/scan/x 等）、
  眉 7 种、嘴 10 种、腮红、25 种特效（zzz/sparkle/进度环/声波/音符等，全笔画，
  不需要字体）。
- 动画：关键帧 easeInOut 插值 + 正弦振荡 + 眨眼叠加；466x466 全脸重绘用脏矩形。
- dotii_render 接口与旧 emotion_render 对齐（attach/update/detach），state_ui
  最小改动。

## 6. 事件映射（小智页面）

| 事件 | 表情 |
| --- | --- |
| 唤醒词 | wake 唤醒 |
| IDLE/已就绪 | idle 待机 |
| ACTIVATING | ready 就绪 |
| CONNECTING | reconnecting 重连中 |
| LISTENING | listening 聆听 |
| THINKING | thinking 思考 |
| SPEAKING | speaking 说话（随文本身接 greeting/laugh 等） |
| MUSIC | music 音乐播放 |
| ERROR | error 出错 |
| DISABLED/离线 | offline 离线 |
| 服务端 llm.emotion | alias 直通/映射（覆盖旧 catalog 全部 name） |
| 触摸/业务 | 保持来源槽位，id 换新表 |
| 低电量/充电/静音（后续系统事件位） | low_battery/charging/muted |

策略改动收敛在 xiaozhi_client_emotion_policy 与 state_ui 的 business/touch id
函数内。

## 7. 测试计划

1. host 单测（并入 test_apps/emotion/run_tests.ps1）：
   - 生成数据校验：48 项、id 唯一、timeline 单调、通道/形状枚举合法。
   - alias 全覆盖：旧 catalog 每个 name 都有映射。
   - dotii_engine：关键帧到位、循环回绕、眨眼叠加、振荡幅值封顶。
   - router 冒烟：来源优先级在新引擎上不回归。
2. 视觉帧转储：dotii_draw 在 host 渲染 48 表情各 1 帧 + 动画采样，合成
   contact sheet，与网页 `_contact_sheet.png` 对照目检。
3. 固件：`D:\aipassport\esp-idf` export 后 `idf.py build` 编译通过。
4. 真机：用户烧录后在小智页面实际对话验收。

## 8. 风险与取舍

- 25 种特效逐个对齐 JS 画风工作量最大，先笔画骨架后微调，帧转储对照验收。
- 替换属于破坏性变更：管理中心形象切换入口暂时失效（固件忽略旧字段），
  后续如需再立"双形象"项目。
- PSRAM 预算：沿用 466x466 RGB565 单缓冲 canvas 方案，不新增大块内存。

## 9. 不做的事

- 不改 CodexBridge / 管理中心协议。
- 不动 mood 模型的情绪判定逻辑。
- 不做管理中心的双形象切换 UI。
