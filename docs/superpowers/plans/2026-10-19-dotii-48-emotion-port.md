# Dotii 48 表情替换固件形象 - 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 用 output/emotion-preview 的 48 表情词汇表整体替换固件 aora 球形形象，小智页面按事件实时渲染，host 测试 + 视觉对照 + 固件编译全过。

**Architecture:** node 生成器把 expressions.js 编译为 C 数据表（dotii_data）；新引擎求值时间线/振荡/眨眼（dotii_engine）；纯 C 光栅核心（dotii_draw，无 LVGL 依赖，host 可跑）+ LVGL 胶水（dotii_render）；router/mood/state_ui 接线保留，旧 face/pose/geometry/character_pack/render/animation 删除。

**Tech Stack:** ESP-IDF (D:\aipassport\esp-idf) + LVGL canvas RGB565、node vm（生成器）、zig cc host 测试（test_apps/emotion/run_tests.ps1）、Playwright 已有网页预览作视觉基准。

**Spec:** docs/superpowers/specs/2026-10-19-dotii-48-emotion-port.md

## Global Constraints

- 协议契约：两位数字 id 00-47 永不重排；英文 name 与网页版完全一致；旧 36 表情的全部 name 必须能映射到新表（alias 全覆盖）。
- 来源优先级 touch > xiaozhi > business 不变；mood 判定逻辑不动。
- 不做 git 提交（工作区有用户未提交改动，一切只落盘）。
- 内存：沿用单块 466x466 RGB565 canvas（PSRAM），不新增大块缓冲。
- 生成器输出文件头必须标注 "generated from output/emotion-preview/expressions.js"。

## Review Focus

- 旧名缺映射（如 panicked/busy/recalling/refusing）→ 期望 alias 表逐项覆盖并有测试钉死。
- 时间线 t=1 回绕与 K(1) 端点值 → 期望循环后回到 K(0) 基态，无跳变。
- 眨眼只作用于 open/wide 眼形 → 期望 star/heart/x/closed 等眼形不被眨眼破坏。
- 特效时间相位（zzz 上浮、进度环、声波）→ 期望动画循环平滑。
- 删除旧文件后的残留引用（state_ui/bridge 管理中心字段）→ 期望固件编译零警告、旧字段被忽略不崩溃。

---

## Task 0: 共享契约 + 生成器（主线自己完成，串行，阻塞后续并行任务）

**Files:** `main/emotion/dotii_data.h`, `tools/gen_dotii_catalog.mjs`, `main/emotion/dotii_data.c`(生成), `main/emotion/dotii_draw_stub.c`(临时桩), `test_apps/emotion/test_dotii_catalog.c`

- [x] 写 dotii_data.h：枚举（dotii_eye_shape_t 14 值 / brow 7 / mouth 10 / effect 25 / channel 16 / group 4）、结构体 dotii_keyframe_t/at/values、dotii_osc_t、dotii_expr_t（id/name/label/group/accent/duration/base/timeline/motion/effects）、dotii_pose_t（16 通道 float + blink + dim）、目录访问 API、alias 查询 API、特效计数常量。
- [x] 写 tools/gen_dotii_catalog.mjs：vm 加载 expressions.js → window.DOTII_EXPRESSION_LIBRARY → 枚举映射（眼/眉/嘴/特效/通道字符串→枚举值，未知值直接报错）→ 生成 dotii_data.c（const 表 + alias 表 36 旧名→新 name）。
- [x] 跑生成器，node 校验：48 项、id 顺序 00-47、每项 timeline 至少 2 帧、effects/motion 非空合法。
- [x] 写 test_dotii_catalog.c（数据校验：数量、id 唯一、通道范围、枚举合法、alias 全部能 by_name 命中），zig cc 编译运行通过。
- [x] dotii_draw_stub.c：实现 dotii_draw.h 声明的绘制入口（先画脸盘+双眼占位），供 Task B 固件编译，Task A 会整体替换。

## Task A: 渲染核心 + 帧转储（子代理 A 并行）

**Files（写权限仅限）:** `main/emotion/dotii_draw.c`（替换 stub）, `test_apps/emotion/test_dotii_frames.c`, `test_apps/emotion/run_frames.ps1`, `output/emotion-preview/_fw_frames/`（输出）

**只读参考:** `main/emotion/dotii_data.h`, `output/emotion-preview/face-renderer.js`, `output/emotion-preview/expressions.js`, `output/emotion-preview/_contact_sheet.png`（视觉基准）

- [x] 按逐原语对照 face-renderer.js 实现：脸盘（accent 径向光晕+rim+暗角）、14 眼形、7 眉形、10 嘴形、腮红、25 特效；全笔画光栅（弧线=多段线、圆环=双圆、星/心=多边形），无字体依赖。
- [x] 动画相位与网页一致：zzz 上浮/进度环转/声波呼吸按 now_ms 驱动，周期取自数据。
- [x] test_dotii_frames.c：host 编译（zig cc，无 LVGL），48 表情各渲 1 帧 + idle/listening/music 各 4 个动画采样帧，写 PNG（自写无压缩 BMP 或 PPM+Pillow 转存均可，落 _fw_frames/）并合成 _fw_contact_sheet.png。
- [x] run_frames.ps1 一键重跑；主线目检对照网页版后验收，微调迭代。

## Task B: 引擎 + 接线 + 旧系统删除（子代理 B 并行）

**Files（写权限仅限）:** `main/emotion/dotii_engine.c/.h`, `main/emotion/emotion_router.c/.h`(适配), `main/emotion/dotii_render.c/.h`, `main/state_ui.c`, `main/xiaozhi_client.c`(仅策略函数), `main/CMakeLists.txt`, `test_apps/emotion/run_tests.ps1`, `test_apps/emotion/test_dotii_engine.c`, `test_apps/emotion/test_emotion_router.c`(改), `test_apps/emotion/test_xiaozhi_client.c`(改)，删除 `main/emotion/emotion_face.*` `emotion_pose.*` `emotion_geometry.*` `character_pack.*` `emotion_render.*` `emotion_catalog.*` `animation.*` 与 `test_emotion_catalog.c` `test_animation.c` `test_character_pack.c` `test_emotion_face.c` `test_emotion_pose.c`

**只读参考:** `main/emotion/dotii_data.h`, spec, `output/emotion-preview/face-renderer.js`

- [x] dotii_engine：时间线关键帧 easeInOut 插值（t 循环回绕）、motion 正弦振荡叠加（幅值封顶）、眨眼叠加（仅 open/wide 眼形，周期复用网页参数 2500-5500ms）、dim 处理；接口 init/set/set_hold/set_gaze/tick 与旧 engine 对齐（set 接受两位 id 或 name）。
- [x] emotion_router 适配新引擎类型；来源优先级与 hold 语义不变。
- [x] dotii_render.c：LVGL attach/update/detach，466x466 RGB565 PSRAM canvas + 脏矩形，调 dotii_draw 入口。
- [x] state_ui：attach 换 dotii_render；渲染循环接 dotii_engine state；删除 character_pack 引用；touch/business/xiaozhi 事件 id 全部换成新表（xiaozhi 状态映射按 spec §6）；管理中心旧形象字段解耦（保留解析、不再消费）。
- [x] xiaozhi_client_emotion_policy：输出改用新 name/两位 id；服务端 llm.emotion 走 alias 表。
- [x] 删除旧文件与旧测试；CMakeLists 更新；run_tests.ps1 更新（catalog/engine/router/policy 用例），全绿。
- [x] idf.py build（powershell: `D:\aipassport\esp-idf\export.ps1` 后 `idf.py build`，workdir D:\dotii）编译通过。

## Task C: 集成验收（主线完成）

- [x] 全量 host 测试 run_tests.ps1 绿。
- [x] run_frames.ps1 出 48 帧 contact sheet，与网页版逐格目检（允许笔画级微调循环，回 A/B 或自己修）。
- [x] idf.py build 最终确认。
- [x] 向用户汇报：改了哪些文件、事件→表情映射表、测试与视觉对照结果、烧录验证指引。

