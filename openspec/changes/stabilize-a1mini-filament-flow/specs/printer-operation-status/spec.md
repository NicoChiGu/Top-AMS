## ADDED Requirements

### Requirement: 解码并保留打印机操作原始状态
系统 SHALL 按高 8 位主状态和低 8 位子步骤解码 `ams_status`，并 SHALL 同时保留原始整数、主码、子码、中文标签和已知标记。

#### Scenario: 收到 AMS 260
- **WHEN** MQTT 报告 `ams_status=260`
- **THEN** 系统显示“AMS 260（回抽当前耗材，Top-AMS 正在退线）”且将主码解析为 1、子码解析为 4

#### Scenario: 收到未知状态
- **WHEN** MQTT 报告未收录的主码或子步骤
- **THEN** 系统显示原始码以及“未知状态：主码 X，子码 Y”，不得套用其他已知含义

### Requirement: 解码相关打印机故障并建立安全联锁
系统 SHALL 解析非零 `print_error` 和 `hms[]`，并 SHALL 为已知卡料、等待出料确认和未知严重故障提供原始码、中文原因、建议动作与严重度。

#### Scenario: 收到 HMS 07FF8010
- **WHEN** MQTT HMS 重建短码为 `07FF8010`
- **THEN** 页面显示外挂料盘或耗材卡住的中文原因和处理动作，系统停止电机并保持打印暂停

#### Scenario: 收到未知非零打印错误
- **WHEN** `print_error` 非零且不在已知表中
- **THEN** 页面同时显示十进制和十六进制原值，系统按严重故障停止流程

### Requirement: 实时发布打印机操作状态
系统 SHALL 在 AMS 状态、打印机故障或首料就绪状态变化时广播 `printer_operation_status`，并 SHALL 避免在值未变化时重复广播。

#### Scenario: 短暂 AMS 步骤发生在快照周期之间
- **WHEN** 两次五秒诊断快照之间 AMS 子步骤发生变化
- **THEN** 已连接页面通过轻量消息实时更新动作条和时间轴

### Requirement: 扩展兼容诊断快照
系统 SHALL 保留 `diagnostics_snapshot.system.ams_status` 原始整数，并 SHALL 增加 `ams_status_info`、`printer_fault`、`printer_status_ready`、`channel_confirmed`、`nozzle_actual_c`、`motor_owner` 和 `flow_phase`。

#### Scenario: 旧页面读取诊断快照
- **WHEN** 旧页面只读取 `system.ams_status`
- **THEN** 字段保持原始整数且无需理解新增对象

