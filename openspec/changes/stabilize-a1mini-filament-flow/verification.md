# Verification record

验证日期：2026-08-10

## 已完成

- OpenSpec：`npx.cmd --yes @fission-ai/openspec@1.8.0 validate stabilize-a1mini-filament-flow --strict`
- 主机测试：
  - `test_printer_flow_host.exe`：AMS/HMS、状态转换新鲜度、回执、温度采样、电机互斥、微动消抖、通道持久化和恢复策略通过。
  - `test_bambu_protocol_host.exe`：包含换行、引号和反斜杠的 G-code JSON 往返通过。
  - `test_diagnostics_host.exe`：诊断环形缓冲、MQTT 陈旧/恢复和换料时间线通过。
- 静态与网页测试：
  - `python main/test/host/validate_gcode_and_web.py`：普通换料、官方/可选首料模板、JavaScript 语法和 4/6/8 通道资源通过。
  - `node main/test/host/test_web_ui.cjs <chrome>`：桌面 1440 px、窄屏 390 px、已知/未知状态、故障条和时间线交互通过。
- ESP-IDF 5.3.2 构建：
  - 4 通道 `Top-AMS.bin`：`0x141f90`，应用分区余量约 14%。
  - 6 通道 `Top-AMS.bin`：`0x141f90`，应用分区余量约 14%。
  - 8 通道 `Top-AMS.bin`：`0x141f80`，应用分区余量约 14%。
  - ESP32-C3 `unit-test-app.bin` 构建成功；ELF 中注册 21 个 Unity 测试。
    单元测试分区表中的两个 64 KiB OTA 槽不足以容纳该镜像，真机测试需按 ESP-IDF unit-test-app 流程烧录到可容纳镜像的 factory 测试分区。

## 尚需硬件完成

- 尚未将 `unit-test-app.bin` 烧录到真实 ESP32-C3，因此这里只确认测试固件可编译、链接和注册，不宣称 Unity 已在芯片上运行。
- 尚未在 A1 mini 上执行官方启动、可选首料、完整双色换料以及料盘阻力、不到传感器、喷嘴未出料和 MQTT 断联故障注入。
- 发布前必须完成上述两项，并确认任何失败均停止电机、保持打印暂停且不会写入错误目标通道。
