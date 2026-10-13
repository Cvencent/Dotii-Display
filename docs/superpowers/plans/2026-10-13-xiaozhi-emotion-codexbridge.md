# Dotii 小智表情 + CodexBridge 配额 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 Dotii 固件加入原生表情引擎、小智客户端与触摸联动，把 Codex 配额/用量通过 CodexBridge 通用协议下发，并把管理中心重构为 小智/小智表情/自定义/Codex/设置（去掉 Bambu）。

**Architecture:** 固件端新增 `main/emotion/` 目录，包含目录、动画原语、状态机、形象包、渲染、路由器六个纯 C 模块；表情数据与形象均为只读表，可在主机上用 gcc 单测。CodexBridge 在既有唯一协议上扩展 `quota` 能力与 `codex_status` 消息。管理中心沿用 schema_version=1 快照，删除 Bambu，新增 xiaozhi 配置段。

**Tech Stack:** ESP-IDF 5.5.3 / ESP32-S3 / LVGL 9.4 / cJSON；Python 3.11 stdlib；原生 JS 前端。

**Spec:** `docs/superpowers/specs/2026-10-13-xiaozhi-emotion-codexbridge-design.md`

## Global Constraints

- 表情 ID 为两位十进制字符串，编号永不重排；未知名回落到 `02` 待机。
- 表情数值一律可选，缺失显示 `--`，不推算。
- 设备协议仍为 `schema_version = 1`；新增字段必须可选。
- 管理接口只接受回环地址；设备接口校验令牌。
- 固件主机单测必须用 gcc 可编译（不依赖 ESP-IDF），放在 `test_apps/emotion/`。
- 全部 ASCII 源码；中文仅出现在用户可见字符串与文档。
- 不保留协议版本分支或并行握手。

## Review Focus

- 未知/非法表情 ID 或名字：必须回落 `02` 且不崩溃（emotion_catalog）。
- 动画原语在 dt=0、极大 t、负相位：必须返回有界值，不产生 NaN。
- 状态机在无输入、长时间运行：必须收敛到待机，不无限累积。
- 配额字段缺失或类型错误：显示 `--`，不得报错或外推。
- 关闭模块（xiaozhi/custom/dotii）：对应后台与页面必须真正停止/隐藏。

---

### Task 1: 表情目录与动画原语（主机可测）

**Files:**
- Create: `main/emotion/emotion_catalog.h`, `main/emotion/emotion_catalog.c`
- Create: `main/emotion/animation.h`, `main/emotion/animation.c`
- Test: `test_apps/emotion/test_emotion_catalog.c`, `test_apps/emotion/test_animation.c`, `test_apps/emotion/run_tests.c`

**Interfaces:**
- Produces: `const emotion_def_t *emotion_catalog_by_id(const char *id)`;
  `const emotion_def_t *emotion_catalog_by_name(const char *name)`;
  `const emotion_def_t *emotion_catalog_fallback(void)`; `size_t emotion_catalog_count(void)`.
- Produces: `float animation_eval(anim_type_t type, float t_seconds, float amp, float period, float phase)`.
- `emotion_def_t` fields: `id`(char[3]), `name`(const char*), `group`(emotion_group_t),
  `transition_ms`(uint16), `gaze`(bool), `openness`(float), `blink_ms_min/max`(int32),
  `anim_count`(uint8), `anims`(emotion_anim_t[3]).

- [ ] Step 1: Write failing test `test_emotion_catalog.c` asserting
  `emotion_catalog_by_id("02")->id == "02"`, `emotion_catalog_by_name("happy")->id == "10"`,
  `emotion_catalog_by_id("99") == NULL`, `emotion_catalog_fallback()->id == "02"`.
- [ ] Step 2: Run `gcc -I main/emotion test_apps/emotion/test_emotion_catalog.c main/emotion/emotion_catalog.c -o t && ./t`; expect fail (symbols undefined).
- [ ] Step 3: Implement catalog table (32 entries, ids 00-41) and lookups.
- [ ] Step 4: Run test; expect PASS.
- [ ] Step 5: Write `test_animation.c` asserting `animation_eval` is bounded in [-1.5,1.5] for
  t in {0, 0.001, 1e6}, period>0, and returns 0 for period<=0.
- [ ] Step 6: Run; expect fail. Implement primitives. Run; expect PASS.
- [ ] Step 7: Commit `feat: add emotion catalog and animation primitives`.

### Task 2: 表情状态机

**Files:**
- Create: `main/emotion/emotion_engine.h`, `main/emotion/emotion_engine.c`
- Test: `test_apps/emotion/test_emotion_engine.c`

**Interfaces:**
- Consumes: Task 1 catalog.
- Produces: `void emotion_engine_init(emotion_engine_t*, uint32_t now_ms)`;
  `bool emotion_engine_set(emotion_engine_t*, const char *id_or_name, uint32_t now_ms)`;
  `void emotion_engine_set_gaze(emotion_engine_t*, float nx, float ny)`;
  `void emotion_engine_tick(emotion_engine_t*, uint32_t now_ms)`;
  `const emotion_engine_state_t *emotion_engine_state(const emotion_engine_t*)`.
- Produces: `bool emotion_engine_expire(emotion_engine_t*, uint32_t now_ms)` returning true when a
  temporary emotion (with `hold_ms>0`) has expired and engine reverted to idle.

- [ ] Step 1: Failing test: set "10", tick 100ms, state id == "10"; set "99" -> state id == "02".
- [ ] Step 2: Run; fail. Implement. Run; pass.
- [ ] Step 3: Failing test: `emotion_engine_set_hold(e,from,"13",2000,now)`, tick now+2100 -> reverts to "02"; gaze clamped to [-1,1].
- [ ] Step 4: Run; fail. Implement hold/expire + gaze clamp. Run; pass.
- [ ] Step 5: Commit `feat: add emotion state machine`.

### Task 3: 形象包

**Files:**
- Create: `main/emotion/character_pack.h`, `main/emotion/character_pack.c`
- Test: `test_apps/emotion/test_character_pack.c`

**Interfaces:**
- Produces: `const character_def_t *character_pack_lookup(const char *id)`;
  `const character_def_t *character_pack_fallback(void)`; `size_t character_pack_count(void)`;
  `const char *character_pack_id_at(size_t index)`.
- `character_def_t` fields: `id`(const char*), `name`(const char*), `body_color`(uint32),
  `eye_color`(uint32), `blush_color`(uint32), `mouth_color`(uint32), `eye_dx`(int16),
  `eye_cy`(int16), `eye_w`(uint8), `eye_h`(uint8), `taper`(float), `tilt`(float),
  `ring_points`(uint16), `ring`(const int16_t*).

- [ ] Step 1: Failing test: `character_pack_lookup("dotii")->id=="dotii"`,
  `character_pack_lookup("nimbo")->id=="nimbo"`, `character_pack_lookup("nope")->id=="dotii"`,
  every pack has `ring_points >= 24` and non-null `ring`.
- [ ] Step 2: Run; fail. Implement 4 packs (dotii/nimbo/twinkle/blob), each with 48 generated ring points.
- [ ] Step 3: Run; pass.
- [ ] Step 4: Commit `feat: add switchable character packs`.

### Task 4: 表情路由器（多来源合流）

**Files:**
- Create: `main/emotion/emotion_router.h`, `main/emotion/emotion_router.c`
- Test: `test_apps/emotion/test_emotion_router.c`

**Interfaces:**
- Consumes: Task 2 engine.
- Produces: `void emotion_router_init(emotion_router_t*, emotion_engine_t*)`.
- Produces: `void emotion_router_set_source(emotion_router_t*, emotion_source_t src, const char *emotion, uint16_t hold_ms, uint32_t now_ms)`;
  `void emotion_router_clear_source(emotion_router_t*, emotion_source_t src, uint32_t now_ms)`;
  `void emotion_router_tick(emotion_router_t*, uint32_t now_ms)`;
  `const char *emotion_router_active_id(const emotion_router_t*)`.
- `emotion_source_t` enum order defines priority: `EMOTION_SRC_TOUCH`(0) >
  `EMOTION_SRC_XIAOZHI`(1) > `EMOTION_SRC_BUSINESS`(2). Lower index = higher priority.

- [ ] Step 1: Failing test: touch="13" hold 1000 at t=0; xiaozhi="30" hold 0; tick t=500 -> active "13";
  tick t=1100 -> active "30"; clear xiaozhi -> active "02".
- [ ] Step 2: Run; fail. Implement priority + per-source expiry. Run; pass.
- [ ] Step 3: Commit `feat: add multi-source emotion router`.

### Task 5: 固件接入（构建 + 页面 + 触摸）

**Files:**
- Modify: `main/CMakeLists.txt` (add emotion sources + include dir)
- Create: `main/emotion/emotion_render.h`, `main/emotion/emotion_render.c`
- Modify: `main/state_ui.c` (render via engine; touch -> router)
- Modify: `main/app_state.h`, `main/app_state.c` (add `xiaozhi_enabled`, `character_id`, `emotion_map`)

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces: `void emotion_render_attach(emotion_render_t*, lv_obj_t *parent)`;
  `void emotion_render_update(emotion_render_t*, const emotion_engine_t*, const character_def_t*)`;
  `void emotion_render_detach(emotion_render_t*)`.

- [ ] Step 1: Add sources to `main/CMakeLists.txt` and run `idf.py build`; expect pass.
- [ ] Step 2: Implement emots render mapping engine state -> LVGL parts; wire into `state_ui.c` Dotii page.
- [ ] Step 3: `idf.py build`; expect pass, binary under partition size.
- [ ] Step 4: Run host emotion tests; expect pass.
- [ ] Step 5: Commit `feat: wire native emotion engine into firmware`.

### Task 6: 小智客户端（连接/协议/配置）

**Files:**
- Create: `main/xiaozhi_client.h`, `main/xiaozhi_client.c`
- Modify: `main/CMakeLists.txt`, `main/connectivity.c`, `main/device_config.c/.h`
- Test: `test_apps/emotion/` JSON-parse host test optional

**Interfaces:**
- Produces: `void xiaozhi_client_init(xiaozhi_client_t*)`;
  `void xiaozhi_client_configure(xiaozhi_client_t*, const char *ws_url, const char *token)`;
  `void xiaozhi_client_start(xiaozhi_client_t*)`; `void xiaozhi_client_stop(xiaozhi_client_t*)`;
  `xiaozhi_session_state_t xiaozhi_client_state(const xiaozhi_client_t*)`;
  `const char *xiaozhi_client_last_emotion(const xiaozhi_client_t*)`.

- [ ] Step 1: Implement WebSocket connect + JSON message parse (`emotion`,`text`) using esp_websocket_client; guard with `CONFIG_XIAOZHI_ENABLE` default off.
- [ ] Step 2: `idf.py build`; expect pass.
- [ ] Step 3: Route `xiaozhi_client_last_emotion` into `emotion_router` in `state_ui.c`.
- [ ] Step 4: `idf.py build`; pass.
- [ ] Step 5: Commit `feat: add onboard xiaozhi client`.
- [ ] Note: Opus decode + wake word deferred to follow-up task (spec section 13).

### Task 7: 管理中心去 Bambu + 新导航 + 小智配置

**Files:**
- Delete: `bridge/bambu_client.py`, `bridge/tests/test_bambu_client.py`
- Modify: `bridge/codex_bridge.py` (remove Bambu, add xiaozhi config + module, new nav data)
- Modify: `bridge/web/index.html`, `bridge/web/app.js`, `bridge/web/styles.css`
- Modify: `bridge/tests/test_codex_bridge_config.py`, `bridge/tests/test_module_on_demand.py`

**Interfaces:**
- Produces: `default_xiaozhi_config()` / `validate_xiaozhi_config(payload)` returning
  `{enabled, server_url, wake_word, voice, mcp_enabled}`.
- Snapshot `modules`: `{xiaozhi, custom, dotii}` (no `bambu`).

- [ ] Step 1: Failing test: `validate_xiaozhi_config({})` returns defaults with `enabled False`;
  `validate_snapshot` rejects a snapshot containing `bambu`.
- [ ] Step 2: Run `python -m unittest discover -s bridge\tests -v`; fail.
- [ ] Step 3: Remove Bambu across code/web/tests; add xiaozhi config; update nav to 小智/小智表情/自定义/Codex/设置.
- [ ] Step 4: Run tests + `node --check bridge\web\app.js`; pass.
- [ ] Step 5: Commit `feat: replace Bambu with xiaozhi in management center`.

### Task 8: CodexBridge 配额能力与消息

**Files:**
- Modify (CodexBridge repo): `codexbridge/protocol/capabilities.py`, `codexbridge/codex/collector.py`
- Create (CodexBridge repo): `codexbridge/codex/quota.py`
- Test (CodexBridge repo): `tests/test_quota.py`

**Interfaces:**
- Produces: `FEATURES` includes `"quota"`; `MODEL_DEFAULTS["dotii"]` includes `"quota"`.
- Produces: `quota_from_snapshot(snapshot: dict) -> dict` with optional keys listed in spec section 7.

- [ ] Step 1: Failing test: `quota_from_snapshot` returns only present keys; missing usage -> keys absent;
  bad types ignored.
- [ ] Step 2: Run `python -m unittest discover -s tests -v` in CodexBridge; fail.
- [ ] Step 3: Implement `quota` capability + `codex_status` message emission gated by effective capabilities.
- [ ] Step 4: Run tests; pass.
- [ ] Step 5: Commit `feat: add quota capability and codex_status message`.
