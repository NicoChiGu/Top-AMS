# G-code 配置

完整的 A1 mini 通讯、切片选料、通道标定、自动换料和故障排查步骤见：

- [A1 mini + Top-AMS：Bambu Studio 完整用户操作流程](./A1mini_BambuStudio用户操作流程.md)
- [A1 mini 与 Top-AMS 通讯、切片选料及自动换料完整教程](./A1mini通讯与切片换料完整教程.md)

当前版本的关键结论：

- 推荐使用 [A1mini换料_topams版本.gcode](./A1mini换料_topams版本.gcode) 完整替换独立 Top-AMS 打印机预设中的“换料 G-code”，不要只在官方 AMS 流程前简单添加两行。
- 当前仓库不替换机器启动 G-code；保留所用 Bambu Studio 版本的 A1 mini 默认启动内容。`A1mini快速启动.gcode` 不是 Top-AMS 专用首料脚本。
- 切片器内部的 `next_extruder` 从 0 开始，模板用 `next_extruder + 1` 映射到 Top-AMS 通道 1–8。
- 当前固件把热床目标温度 **1–16°C** 视为协议保留范围；17°C 不会触发换料。
- 4/6/8 通道固件只能选择实际装配的通道。
- 首个打印耗材不会由切片器自动同步，打印前必须在 Web 中确认“当前通道”与热端内真实耗材一致。
- 切片器冲刷体积可以正常生效，但需要结合进料时间和实际材料做小模型标定。
- 正常工作流不需要 G-code 生成器或自动改写式后处理脚本；后续更适合先增加只读校验器。
