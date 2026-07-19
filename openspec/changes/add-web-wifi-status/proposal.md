# Change: 在 Web 界面显示 ESP32 Wi-Fi 状态

## Why
当前 Web 界面只能确认浏览器与 ESP32 的 WebSocket 是否连通，无法直接判断设备连接的是哪个无线网络、局域网地址是什么或信号是否稳定。用户排查访问异常和设备掉线时需要从串口或路由器侧查找这些信息。

## What Changes
- 通过现有 WebSocket 状态通道向 Web 客户端提供 ESP32 当前 Wi-Fi 连接状态、SSID、局域网 IP 和 RSSI。
- 在现有顶部设备状态区展示 Wi-Fi 信号强度，并提供 SSID、IP 和可读的信号质量信息。
- 在 WebSocket 初次连接及页面存续期间刷新网络信息，使信号变化和断开状态能够反映在界面中。
- 为连接中、已连接、断开和数据不可用状态提供明确且适合桌面与移动端的显示。

## Impact
- 影响的规范：`web-network-status`
- 影响的代码：Wi-Fi 状态采集、WebSocket 状态同步、Web 顶部状态区和发布说明
- 兼容性：不改变现有 Wi-Fi 配网、MQTT 连接或电机控制行为
