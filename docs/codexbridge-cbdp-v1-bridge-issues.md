# CodexBridge 侧问题与疑问（固件 CBDP v1 联调记录）

- 日期：2026-10-18
- 固件侧：Dotii（ESP32-S3）CBDP v1 panel 客户端
- Bridge 侧：本机运行中的 CodexBridge.exe（未做任何改动，仅只读观察）
- 说明：以下均为固件实测观察结果，供 Codex Bridge 维护者判断。本文档不构成对 Bridge 代码的修改。

## 结论摘要

固件侧本轮共修复 3 个自身问题，会话已从"每 2-4 秒断开一次"变为稳定（150 秒 soak 仅 1 次断开后自动重连）。
联调中发现 3 个 **Bridge 侧与协议文档不一致** 的点，以及 1 个 **Bridge 侧行为疑问**，列在下方。

---

## 一、DELTA 帧缺少协议要求的头部字段（需 Bridge 侧确认）

**协议要求（CBDP-V1-PROTOCOL.md §15.7）：**

```
DELTA(
  event_id, global_seq, topic_id, topic_seq, revision,
  event_type, qos, event_payload
)
```

即 `event_id`(0x0029)、`global_seq`(0x0035)、`topic_seq`(0x0034)、`revision`(0x0036) 应在 DELTA 的 header TLV 中。

**实测（固件串口抓到的真实帧，TLV 列表）：**

```
RX type=0x0a flags=0x0400 seq=0 ack=0 code=0x0000 qos=0 topic=0x00000000
header TLVs = 0037/2 0038/614          <- 只有 event_type 和 event_payload
payload TLVs = 0021/4 0022/36 0025/4 0026/54 0040/64 0041/64 0042/254
               0043/62 0044/2 0045/1 004b/8 004c/8 0055/1
```

- DELTA 的 header 里**只有** `0x0037 event_type` 和 `0x0038 event_payload`。
- 没有 `0x0029 event_id`、`0x0034 topic_seq`、`0x0035 global_seq`、`0x0036 revision`。
- 固定头 `topic_id` 为 `0x00000000`，而不是约定的线程 topic（`0x1xxxxxxx` 或 `0x5xxxxxxx`）。
- 这些信息在 `event_payload` 内层也没有出现（内层只有 thread/message/title/body/timestamp 等业务字段）。

**影响：** 按协议严格实现、且把 `topic_seq/global_seq` 当作必填的客户端会直接拒绝该帧。
固件本轮已改为"仅 `event_type` 必填，其余可选"，从而可以工作；但这样一来
**固件无法进行 §15.7 要求的 topic_seq 去重和 revision 顺序校验**，也无法按 §15.8
上报有意义的 `cursor`（当前 cursor 只能回 0，即"只做传输层 ACK、不推进业务 cursor"）。

**希望 Bridge 侧确认：**
1. DELTA 是否应该补齐 `event_id/global_seq/topic_seq/revision` 与真实 `topic_id`？
2. 如果这是有意的简化，请更新协议文档，明确这些字段对 DELTA 是可选的，并说明客户端应如何做去重与断点恢复。

---

## 二、DELTA 的 `topic_id` 恒为 0

**实测：** 所有 DELTA 帧固定头 `topic_id = 0x00000000`（见上方抓包）。

**协议要求（§15.1）：** 控制主题为 `0`；线程主题为 `0x10000000..0x1FFFFFFF`；
`system.thread_summaries` 固定为 `0x50000003`。事件应带其所属 topic。

**影响：** 设备无法按 topic 分桶、无法为不同 scope 维护独立 cursor，
也就无法实现 §15.8 的 "`(device_id, topic_id, scope)` 维 cursor"。固件的
`send_ack` 因此只能发送不带 cursor 的纯传输层 ACK。

**希望 Bridge 侧确认：** DELTA 是否会带真实 `topic_id`；若不打算带，请在文档中写明替代方案。

---

## 三、DELTA 使用 `seq=0`，与 ACK 语义冲突（需明确）

**实测：** DELTA 帧的 `seq = 0`（见上方抓包，flags=0x0400 EVENT）。

**协议要求（§13.1）：** "`PING`、`PONG`、`ACK`、`CLOSE` 和发现消息不占用传输序号，发送时 `seq = 0`"，
即暗示其余需要可靠投递的帧应当分配并递增 `seq`。而 §13.2/§13.4 要求接收方对
`seq` 连续性做校验与重传处理。

**影响：** `seq=0` 的 DELTA 无法参与 §13.2 的累计 ACK 与 §13.4 的重传。
固件此前把 `seq==0` 一律视为非法帧，导致每帧都回 `ERROR(INVALID_FRAME)`，
并因此被 Bridge 断开——固件侧已修复（`seq==0` 按"不占用序号"处理，不推进
`s_rx_expected`）。但这意味着 **DELTA 目前没有传输层可靠性保障**。

**希望 Bridge 侧确认：** 事件类帧是否应分配递增 `seq`；若有意使用 `seq=0`，
请明确事件的可靠性由 cursor/重传之外的其他机制保证。

---

## 四、Bridge 对错误的响应（行为疑问，非缺陷结论）

**实测序列（固件侧抓包）：**

1. Bridge 先连续推送 13-14 个 `DELTA(0x0A)` 帧。
2. 固件（旧版本）对每帧回 `ERROR code=0x0005 INVALID_FRAME`。
3. Bridge 随即回 `ERROR code=0x000E UNSUPPORTED_FEATURE request=0 msg="UNSUPPORTED_FEATURE"` 并关闭连接。

**疑问：** 当设备回 `INVALID_FRAME` 时，Bridge 报 `UNSUPPORTED_FEATURE` 并断开，
而不是 `INVALID_FRAME` 或 `PROTOCOL_VERSION`。这个错误码映射关系是否是有意设计？
希望 Bridge 侧确认，或补充规则说明。

---

## 五、已由固件侧修复的本方问题（供对照，非 Bridge 问题）

以下均为固件自身缺陷，已在固件侧修复并加了回归测试，无需 Bridge 改动：

1. `seq==0` 被误判为非法帧 → 已改为控制/事件帧不占用传输序号。
2. DELTA 头部字段按必填解析 → 已改为仅 `event_type` 必填。
3. 线程列表容量上限被当作 OOM 上报 → 已改为容量满时良性跳过。

修复后固件侧验证结果：QEMU 单元测试 **46 Tests / 0 Failures**；真机 150 秒 soak
仅 1 次断开且自动重连（修复前为每 2-4 秒断开一次）。

---

## 六、能力协商观察

**实测：** Bridge 返回的 `AUTH_OK.effective_caps = 0x8F`（= status|text|tasks|results|read_ack），
而固件声明的 `supported_caps` 包含 subscribe_summary/subscribe_live/multi_page/device_config/quota 等。

**推断（需 Bridge 侧确认）：** Bridge 里 Dotii 设备的 `enabled` 仍是旧的能力字符串集合
（`quota/read_ack/results/status/tasks/text`），求交后把新的能力位全部剪掉，
导致设备无法上报订阅意图、收不到 SNAPSHOT，只能被动接收 DELTA。

若是如此，建议 Bridge 侧把该设备的 enabled 集合迁移到 CBDP v1 能力位。

---

## 七、CodexBridge 侧处理结果（2026-10-18）

以下对应前面四条问题。Bridge 已改，设备端仍有少量必须配合的项，列在每条后面。

### 7.1 已由 Bridge 修复

| 条目 | Bridge 改动 | 代码位置 |
|---|---|---|
| 一、DELTA 缺少 `event_id/global_seq/topic_seq/revision` | CBDP 会话不再走兼容翻译生成 DELTA；所有业务事件从 Event Spine 出站，`_delta_frame()` 统一补齐 6 个 header TLV | `codexbridge/hub.py`（`_broadcast_messages` / `_deliver_cbdp_event` / `replay_state`）、`codexbridge/devices/session.py`（`send_event`）、`codexbridge/cbdp/gateway.py`（`delta_frame`） |
| 二、`topic_id` 恒为 0 | 同上，DELTA 的固定头 `topic_id` 取 `Event.topic_id`（线程 topic `0x1xxxxxxx` 或 system topic），不再使用 `CompatContext` 的默认 0 | `codexbridge/cbdp/gateway.py::_delta_frame` |
| 三、`seq=0` | DELTA 由 `_next_seq()` 分配非零 `bridge_seq`；兼容翻译器产出的 DELTA 在 CBDP 会话上被直接丢弃，不会上线路 | `codexbridge/cbdp/gateway.py::_next_seq`、`codexbridge/devices/session.py::send` |
| 四、`INVALID_FRAME` 被回成 `UNSUPPORTED_FEATURE` | `handle_frame()` 新增 `TYPE_ERROR` / `TYPE_CLOSE` 分支：记录对端错误并保持连接，不再回错误 | `codexbridge/cbdp/gateway.py::_handle_peer_error` / `_handle_peer_close` |
| 六、`effective_caps=0x8F` | 能力词表统一为 CBDP 9.2 名称；`quota` 作为 `account`(bit 42) 别名；`protocol=cbdp` 首次登记默认启用设备声明的全部（已实现）能力 | `codexbridge/protocol/capabilities.py`、`codexbridge/cbdp/capabilities.py`、`codexbridge/devices/registry.py` |

协议文档同步更新：

- `docs/CBDP-V1-PROTOCOL.md`
  - **§9.2 能力位图后**：新增“两套词表的统一（v1.0 实现修正）”（第 1302 行起）
  - **§15.7 DELTA 后**：新增“实现要求（不因传输 profile 放宽）”（第 1859 行起）
  - **§21.3 错误分类**：补充“收到对端 ERROR 时不得回发 ERROR”（第 3464 行）
  - **§24.2 信封映射后**：新增“兼容网关不生成 DELTA 信封”（第 3578 行）
  - **§15.1 末尾**：新增“线程对象必须带 `topic_id`”（tag `0x0030`）
- `PROTOCOL.md` §15 协议演进：同步上述三条规则（第 1057 行起）

回归测试：`python -m unittest discover -s tests -v` → **685 tests, OK**。

### 7.2 还需要设备端配合的项

Bridge 修完后，DELTA 信封已经合规，但设备要真正收到并推进 cursor，固件侧还需处理以下内容：

1. **订阅真正需要的 topic。** Bridge 现在按协议只把事件发给**订阅了对应 `topic_id + scope`** 的设备。当前固件只订阅了：
   - `0x50000003`（`system.thread_summaries`，summary）
   - `0x50000002`（`system.resync`，live）

   消息事件位于**线程 topic**（`0x1xxxxxxx`）。固件需要在选中/打开线程后，按该线程的 `topic_id` 发送对应 `SUBSCRIBE`：
   - `scope = thread (1)`：接收 `message.appended` / `message.updated` 等线程事件；
   - `scope = summary (0)`：接收 `message.read` 等摘要事件（如需要已读同步）。

   只订阅 system topic 的话，Bridge 不会再把线程消息作为 DELTA 推给它。同一 `topic_id` 可以同时订阅多个 scope。

2. **从线程对象拿到 `topic_id`（Bridge 已补）。** Bridge 现在在 `threads.list` / `thread.read` 的 thread 对象（`object_type = 0`）里返回该线程的稳定 `topic_id`（tag `0x0030`，见 `codexbridge/cbdp/gateway.py::_encode_thread_object`）。固件需要在解析 thread 对象时读取并保存它，后续用它发送 `SUBSCRIBE(topic_id, scope = thread)`。协议已同步到 `docs/CBDP-V1-PROTOCOL.md` §15.1 末尾。

3. **按 §15.8 用 `cursor` ACK。** DELTA 现在带真实 `topic_seq/global_seq`，固件可以在应用事件后回带 `cursor` 的 `ACK`。这会让 Bridge 的 `SubscriptionManager.ack()` 推进 checkpoint，重连后从 cursor 恢复，而不是每次从 0 回放。

4. **不要再把 `seq==0` 当作“不占用序号”的特例。** Bridge 之后的 DELTA 都会带非零 `seq`。固件应恢复正常 §13.2 校验：`seq < expected` 丢弃并重 ACK，`seq > expected` 请求 resync；`seq==0` 只用于 PING/PONG/ACK/CLOSE/发现消息。

5. **保留对端 ERROR 的非致命处理。** Bridge 已不再回 ERROR，但固件收到 Bridge 的 `ERROR` 时建议也按 §21.3 分类：`INVALID_FRAME` 可继续连接，只有 `UNAUTHORIZED/AUTH_EXPIRED/TOKEN_ROTATED` 才断开。当前固件任何 peer `ERROR` 都 `ESP_ERR_INVALID_STATE` 拆会话，仍可能因一次可恢复错误掉线。

6. **`quota` 建议改成 `account`。** Bridge 已做别名兼容，但固件的 `CAPABILITY_NAMES` 把 bit 42 报成 `quota`。建议固件改用协议名 `account`，避免以后其他 Bridge 实现不认别名。

7. **（可选）补 `retransmit`。** 传输层重传是发送方职责。Bridge 侧目前只分配 `seq`，尚未实现 §13.3/§13.4 的窗口与重传定时器；设备端当前也不需要额外改动。如果后续要真正开启重传，请以协议 §13 为准。

---


---

## 八、设备端处理结果与复测（2026-10-19）

### 8.1 设备端已按 7.2 完成配合

| 7.2 条目 | 固件改动 |
|---|---|
| 1. 订阅线程 topic | 新增 `codex_bridge_client_subscribe_thread()`；打开线程详情时同时订阅 `scope=thread`（消息事件）和 `scope=summary`（`message.read`），按 thread_id 去重 |
| 2. 读 thread 对象 `topic_id` | 投影层解析 tag `0x0030`，新增 `codex_bridge_projection_thread_topic()` |
| 3. cursor ACK | 按 `(topic_id, scope)` 维护 cursor；DELTA 应用成功后回带，`message.read` 定向到 `summary` scope，其余线程事件定向到 `thread` scope |
| 4. `seq==0` | 恢复 §13.2 校验（重复丢弃重 ACK、缺口请求 resync） |
| 5. ERROR 非致命 | `error_is_fatal()`：仅 UNAUTHORIZED/PROTOCOL_VERSION/AUTH_EXPIRED/TOKEN_ROTATED 断开；并对带 `request_id` 的 ERROR 取消该请求的重传 |
| 6. `quota`→`account` | bit 42 已改名 |
| 7. 传输层重传 | 本轮不做（按要求） |

### 8.2 复测确认 7.1 修复已生效

Bridge 已替换为新构建（`D:\codexBrigexx\CodexBridge\CodexBridge.exe`，2026-10-19 06:45）。
串口实测：

```text
I codex_bridge: CBDP v1 online profile=panel caps=0x2c000008bc8f heap=5819 psram=3459992
I codex_bridge: TX SUBSCRIBE topic=0x50000003 scope=0 qos=1 cursor=0
I codex_bridge: TX SUBSCRIBE topic=0x50000002 scope=2 qos=0 cursor=0
I codex_bridge: first DELTA applied (topic=0x1000000d seq=1 cursor=13861)
```

- DELTA 带**非零 `seq=1`** ✓
- DELTA 带**真实线程 topic `0x1000000d`** ✓
- 能力集：`0x2c000008bc8f` ✓（已移除未实现的 `cross_device_active` / `resume` / `events_since`）
- Bridge 侧 `devices.json` 中 Dotii 的 `protocol` 已是 `cbdp`，能力表已换成 CBDP 名称 ✓
- 60 秒观测：0 次 `session closed`、0 次 `frame failed`、0 次 `sequence gap` ✓

### 8.3 仍待 Bridge 侧确认的两点（非阻塞）

**1. `RESULT` 与 `ERROR` 帧使用 `seq=0`，且 `RESULT` 不带 `ack_seq`。**

实测 BRIDGE 对两次 `SUBSCRIBE` 回 `RESULT(code=0x0000)`，但 `seq=0`、`ack_seq=0`。
按 §13.1，`seq=0` 只保留给 `PING/PONG/ACK/CLOSE`/发现消息，`RESULT` 不在其中；
`ack_seq=0` 也意味着设备无法用累计 ACK 清除已发送的 SUBSCRIBE。

固件已容错：`RESULT`/`ERROR` 若带 `request_id` 就按该 id 取消待重传帧。
但协议层面仍建议明确：**`RESULT`/`ERROR` 是否应带 `seq` 与 `ack_seq`？**

**2. `active_state.read` 返回 `unsupported_method`（0x000E）。**

固件发出 `COMMAND(active_state.read, method_id=0x00B1, request_id=5)` 后收到：

```text
W codex_bridge: peer ERROR code=0x000e request=5 msg=unsupported_method (kept)
```

固件已按 §21.3 视为非致命，并记录该 Bridge 不支持此方法，后续会话不再重复请求。
当前不影响线程列表、线程详情和实时 DELTA，但跨设备活动线程状态仍不可用。

**请确认：** 这是预期的"本轮不实现"行为吗？如果是，建议在问题文档和
`CBDP-V1-PROTOCOL.md` 的 §28.8 能力矩阵里明确 `active_state.read` 的实现状态，
避免设备把 `cross_device_active` 能力声明为已启用却拿不到结果。

### 8.4 `s_wireless_tx_id` 与本仓库无关

该符号位于 AI Passport 固件（`firmware/main/coding_bridge.c`）。
本工程（Dotii）没有 `SEND_*` / `AUDIO_*` 发送路径。
已核对的 AI Passport 两个副本均已包含修复（`next_tx_id()`，
`send_audio_begin()` 为每段录音分配新 `tx_id`）。

### 8.5 最终复测（2026-10-19 16:09）

- QEMU 单元测试：`48 Tests / 0 Failures`（新增能力声明真实性、线程 topic 双 scope / `message.read` ACK 定向用例）。
- 完整固件编译通过：`state_display.bin`，大小 `0x37ebb0`（3,664,816 bytes）。
- COM3 烧录：`Hash of data verified`。
- 真机 40 秒串口观测（恢复源码后重编并重新烧录）：

```text
I codex_bridge: CBDP v1 online profile=panel caps=0x2c000008bc8f heap=5819 psram=3459992
I codex_bridge: first DELTA applied (topic=0x1000000d seq=1 cursor=13861)
```

- `CBDP v1 online`：1 次
- `first DELTA applied`：1 次，topic 为非零线程 topic、seq=1、cursor 非零
- `CBDP session closed`：0 次
- `frame failed`：0 次
- `sequence gap`：0 次
- `heartbeat timeout`：0 次
- Bridge 在对端 ERROR 后不再回发 ERROR：0 次 `peer ERROR`
### 8.6 设备端自查发现：能力声明与已实现命令不一致

本轮复测还发现一个**固件侧问题**，不是 Bridge 缺陷：HELLO 声明了
`cross_device_active`、`resume`、`events_since`，但固件当前只发送
`active_state.read`，没有实现 `active_state.set` / `events.since` 的完整路径。
Bridge 因此会认为这些能力已启用，而设备实际拿不到跨设备活动状态。

设备端处理建议：已在本次收尾中从 `SUPPORTED_CAPS` 移除对应位，并增加回归测试锁定该约束；
后续只有补齐 `active_state.set` / `events.since` 后才可重新声明。

## 附：证据来源

- 固件串口：`COM3`（Dotii，MAC 44:bd:8d:60:7f:8c）
- Bridge 只读数据库：`%LOCALAPPDATA%\CodexBridge\cbdp-events.sqlite3`
- 抓包脚本：`D:/dotii/.codx/capture7.py`（只读串口）
- 相关固件源码：`main/codex_bridge_client.c`、`main/codex_bridge_projection.c`
