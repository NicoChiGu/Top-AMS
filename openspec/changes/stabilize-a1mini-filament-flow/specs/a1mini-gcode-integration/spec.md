## ADDED Requirements

### Requirement: 官方启动流程保持默认兼容
系统 SHALL 将官方 A1 mini 启动 G-code 和人工预装、确认首料作为默认流程，并 SHALL 不永久关闭打印机缠料、断料或堵料保护。

#### Scenario: 用户未选择自动首料模板
- **WHEN** 用户使用默认官方启动模板开始打印
- **THEN** Top-AMS 不注入首料标记且要求首卷料已装载并确认通道

### Requirement: 提供版本化自动首料模板
项目 SHALL 提供基于官方 20260513 A1 mini 启动模板的可选文件，并 SHALL 只在原有 `M620 M` 前插入首料标记、暂停和热床目标恢复三行。

#### Scenario: 切片器首料为通道 3
- **WHEN** 可选模板展开 `{initial_no_support_extruder + 9}` 为 11
- **THEN** Top-AMS 将其识别为首料通道 3，且普通换料标记范围不受影响

### Requirement: 普通换料在暂停前完成安全前导和停靠
普通换料 G-code SHALL 在唯一暂停点前关闭相应检测、设置安全加速度、抬升 Z 并完成 X/Y 停靠，并 SHALL 在恢复段重新启用打印机保护及保留工具状态同步。

#### Scenario: 静态检查普通换料模板
- **WHEN** 发布检查扫描普通换料 G-code
- **THEN** 每个通道标记后恰有一个暂停点，安全命令位于暂停前，`M620/T/M621` 只保留在规定同步位置

