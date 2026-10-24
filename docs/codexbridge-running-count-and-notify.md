# CodexBridge 侧需求：任务运行数与完成计数语义

日期：2026-10-20
致：CodexBridge 维护者
来源：Dotii 固件（本仓库 `main/`）对接 CBDP v1 时发现的问题与期望

## 背景

Dotii 圆屏的 Codex 页面展示「运行中 / 已完成 / 失败」三个计数，并在任务完成时
播放提示音。设备端已经做了本地兜底（从任务列表逐条推导运行数），但下面这些
字段的语义仍需要 Bridge 侧保证一致，否则计数会长期显示 0 或漏掉通知。

## 观察到的现象

设备串口日志中，`codex_state` 消息长期为：

```
codex_state: running=0 completed_unread=0 failed_unread=0
```

即使本地存在可运行的任务，`running` 也没有变为非零。设备因此长期显示
「运行中 0」。请确认 Bridge 侧下发 `codex_state` 时 `running` 的计算是否
覆盖所有“正在执行”的 turn。

## 期望的字段语义

### 1. `codex_state.running`

- 含义：**当前处于执行中（未终结）的 turn 数量**
- 计算：所有 `thread_state == 1`（running）的线程数量
- 时机：该值变化时立即推送一次 `codex_state`；建议同时随任务列表快照推送
- 约束：没有执行中任务时必须为 0，不能沿用旧值

### 2. `codex_state.completed_unread` / `failed_unread`

- 含义：**未被设备读取过的完成 / 失败任务数**（未读计数）
- 递增：每个 turn 进入终态（completed / failed）时**恰好递增 1 次**
- 递减：仅在设备发送已读 ACK（`0x0303` 或 `read_state`）后递减
- 约束：
  - 不允许因快照重发、重连重放、DELTA 重传而重复递增
  - 不允许周期性归零（设备用计数“增加”作为播放完成提示音的唯一触发条件）
  - 进程重启后建议从持久化事件表恢复，避免归零造成漏通知

### 3. 任务列表 `message/task` 的 `status` 字段

设备解析并映射为：

| status 字符串 | 设备枚举 |
| --- | --- |
| `idle` | CODEX_STATUS_IDLE |
| `working` | CODEX_STATUS_WORKING |
| `waiting` | CODEX_STATUS_WAITING |
| `completed` | CODEX_STATUS_COMPLETED |
| `failed` | CODEX_STATUS_FAILED |

设备会按该字段逐条推导「运行中」数量（working + waiting），因此列表里的
`status` 必须随 turn 状态实时更新，不能只下发一次初始值。

### 4. Turn 状态 TLV（`0x0044 CBDP_TLV_STATUS`）

设备端当前映射：

| TLV 值 | 含义 |
| --- | --- |
| 1 | running |
| 2 | completed |
| 3、4 | failed |

如果 Bridge 侧使用不同的枚举值（例如 0=queued、5=...），请在下发前转换为
上述取值，或提供书面枚举说明，设备侧再适配。

## 设备侧已完成的工作（无需 Bridge 配合的部分）

- 从任务列表逐条推导 running 数，作为 `codex_state.running` 缺失/为 0 时的
  本地兜底
- 任务完成/失败时播放本地合成提示音（升调=完成，降调=失败）
- 提示音触发条件：`completed_unread` / `failed_unread` 相比上一次快照**增加**

## 验收方式

1. 启动一个 Codex 任务 → 设备「运行中」应立即 ≥ 1
2. 任务完成 → 「已完成」+1，设备播放一次升调提示音
3. 任务失败 → 「失败」+1，播放一次降调提示音
4. 设备确认已读后 → 未读计数回落，不重复播放提示音
5. 断线重连/快照重发 → 计数不重复累加、不归零


## 补充：传输层 seq 跳号（2026-10-20 实测）

设备抓包发现：握手完成后 Bridge 的第一帧业务消息 `seq=4`，而设备期望 `seq=3`
（WELCOME=1、AUTH_OK=2 之后的下一号）。设备此前严格执行 `§13.2` 会因此断开，
表现为 `codex_state` 永远收不到、页面计数显示 `--`。

设备端已改为**容忍跳号并重同步**（以新帧号为基准继续），但请 Bridge 侧确认：

- 业务帧 seq 是否从 AUTH_OK 收到后连续递增（3、4、5…）
- 是否存在某类消息（如内部心跳/控制帧）占用了 seq 但不发送、导致序号空洞
- 若存在空洞，请确保重传机制最终补齐，或直接跳过并不要求设备重传

同时观察到 `peer ERROR code=0x0015 request=3 msg=INTERNAL`（非致命，设备已忽略），
建议 Bridge 侧查一下该 request 的处理逻辑。
