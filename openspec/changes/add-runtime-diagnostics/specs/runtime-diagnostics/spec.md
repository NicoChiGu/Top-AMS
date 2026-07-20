## ADDED Requirements
### Requirement: 提供运行时设备快照
系统 SHALL 向 Web 客户端提供本次启动的固件版本、启动时长、重启原因、当前和最低空闲堆、Wi-Fi 状态、MQTT 健康度、WebSocket 客户端数和关键换料状态，且 SHALL 不包含密码、设备序列号或完整 MQTT 正文。

#### Scenario: Web 客户端请求快照
- **WHEN** Web 客户端发送 `get_diagnostics_snapshot`
- **THEN** 系统只向该客户端发送一份带当前数值和明确空值的 `diagnostics_snapshot`

#### Scenario: MQTT 从陈旧状态恢复
- **WHEN** MQTT 超过 8 秒未收包后再次收到有效数据
- **THEN** 快照清除 stale 状态并更新最近收包时间，日志只记录一次恢复事件

### Requirement: 提供固定容量结构化日志
系统 SHALL 在 RAM 中保留最新 64 条结构化事件，并 SHALL 为每条事件提供序号、启动毫秒、级别、模块、事件码和 UTF-8 消息。

#### Scenario: 日志容量溢出
- **WHEN** 第 65 条事件写入已满缓冲区
- **THEN** 系统覆盖最旧事件、保留最新 64 条并增加覆盖计数

#### Scenario: 客户端连接
- **WHEN** Web 客户端建立 WebSocket 连接
- **THEN** 系统以每批不超过 16 条发送当前日志，之后实时发送新增事件

#### Scenario: 用户清空日志
- **WHEN** Web 客户端确认发送 `clear_diagnostic_logs`
- **THEN** 系统清空全局缓冲并通知所有客户端，同时保留事件序号的单调递增

### Requirement: 展示最近一次自动换料时间轴
系统 SHALL 按触发、退料、退线、加热、进线和恢复六阶段保存并展示最近一次自动换料的阶段状态、耗时、通道和最近超时点。

#### Scenario: 自动换料成功
- **WHEN** 自动换料依次完成所有适用阶段并成功提交恢复命令
- **THEN** 时间轴显示每个阶段的成功或跳过状态以及总耗时

#### Scenario: 终止流程的超时
- **WHEN** 退料等待 120 秒或加热等待 300 秒后超时
- **THEN** 当前阶段标记为错误、后续阶段标记为未到达并保留超时点

#### Scenario: 可继续的退线状态超时
- **WHEN** 退线后等待正常状态 30 秒超时但流程允许继续
- **THEN** 退线阶段标记为警告、记录超时点并继续后续阶段

#### Scenario: 同通道无需换料
- **WHEN** 触发通道与当前通道相同且无需换料
- **THEN** 时间轴保留触发和恢复结果并将不执行的中间阶段标记为跳过
