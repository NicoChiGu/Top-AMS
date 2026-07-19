## ADDED Requirements
### Requirement: 提供当前 Wi-Fi 网络信息
系统 SHALL 向已连接的 Web 客户端提供 ESP32 当前 Wi-Fi 连接状态；连接有效时，数据 SHALL 包含当前 SSID、局域网 IP 地址和以 dBm 表示的 RSSI。

#### Scenario: Web 客户端初次连接
- **WHEN** Web 客户端与 ESP32 建立 WebSocket 连接
- **THEN** 系统发送一份当前 Wi-Fi 连接状态快照

#### Scenario: Wi-Fi 已连接
- **WHEN** ESP32 已连接到无线接入点并已取得局域网地址
- **THEN** 状态快照包含当前 SSID、非空局域网 IP 和 RSSI

#### Scenario: Wi-Fi 信息不可用
- **WHEN** ESP32 未连接或某项网络信息暂时不可用
- **THEN** 系统发送明确的连接状态且不使用旧值冒充当前值

### Requirement: 更新动态 Wi-Fi 状态
系统 SHALL 在 Web 页面存续期间以受控频率更新 Wi-Fi 连接状态和信号信息，并 SHALL 避免更新机制阻塞现有控制流程。

#### Scenario: 信号强度发生变化
- **WHEN** ESP32 保持联网且 RSSI 随时间变化
- **THEN** Web 界面在下一次状态刷新后显示新的信号值和等级

#### Scenario: Wi-Fi 连接失效
- **WHEN** ESP32 的 Wi-Fi 连接失效且客户端仍能收到状态更新
- **THEN** Web 界面显示断开状态并清除或标记失效的 SSID、IP 和 RSSI

### Requirement: 在 Web 顶部状态区展示 Wi-Fi 信息
Web 界面 SHALL 在现有设备状态区域展示 Wi-Fi 连接状态、信号强度、SSID 和局域网 IP，并 SHALL 在桌面和移动端保持信息清晰且不妨碍现有 MQTT 状态与页面导航。

#### Scenario: 显示已连接网络
- **WHEN** 页面收到有效的已连接 Wi-Fi 状态
- **THEN** 页面显示信号图形、可读信号等级、RSSI dBm 数值、SSID 和 IP 地址

#### Scenario: 等待首份状态数据
- **WHEN** WebSocket 已开始连接但尚未收到 Wi-Fi 状态快照
- **THEN** 页面显示加载状态而不显示伪造的网络信息

#### Scenario: WebSocket 断开
- **WHEN** 浏览器与 ESP32 的 WebSocket 连接关闭或出错
- **THEN** 页面将 Wi-Fi 信息标记为不可用，避免继续显示为实时状态

#### Scenario: 移动端显示
- **WHEN** 页面在窄屏设备上显示较长的 SSID 或 IP 地址
- **THEN** Wi-Fi 状态区自适应布局且不遮挡 MQTT 状态或主内容
