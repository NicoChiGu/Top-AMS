# Top-AMS
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=c%2B%2B&logoColor=white)
![VSCode](https://img.shields.io/badge/IDE-VSCode-007ACC?logo=visual-studio-code&logoColor=white)
![ESP-IDF](https://img.shields.io/badge/Framework-ESP--IDF-green?logo=espressif&logoColor=white)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/nccrrv/Top-AMS)
![GitHub License](https://img.shields.io/github/license/nccrrv/Top-AMS)
## 简介
- 本项目为拓竹打印机的第三方多色换色工程,追求更好的换色过程,更多的自定义配置以及更实惠的成本
- 原理为在拓竹换色gcode中插入热床温度改变并暂停,热床温度会改变为对应的耗材通道,然后esp通过mqtt订阅得知热床温度,接着使用mqtt操作进退料并控制相应电机,最后恢复暂停继续打印
- 目前支持PCB设计最多八个通道,但理论无通道数量上限
## 指南
首先需要准备以下硬件
### 主控模块
- [合宙esp32C3](https://wiki.luatos.com/chips/esp32c3/board.html)
- [PCB版](https://oshwhub.com/eda_xnlouvih/top-ams-8-tong-dao)(如果你只需要双色,也可以考虑直接在面包板上接线)
<!-- - 电机芯片<br>之后放上PCB版的嘉立创链接以及电机芯片的具体型号  -->
-
  | 通道  | 前向GPIO | 后向GPIO |    备注     |
  | :---: | :------- | :------- | :---------: |
  | 通道1 | GPIO2    | GPIO3    |
  | 通道2 | GPIO10   | GPIO6    |
  | 通道3 | GPIO5    | GPIO4    |
  | 通道4 | GPIO8    | GPIO9    |
  | 通道5 | GPIO0    | GPIO1    |
  | 通道6 | GPIO20   | GPIO21   |
  | 通道7 | GPIO12   | GPIO13   | 和LED灯冲突 |
  | 通道8 | GPIO18   | GPIO19   |  和USB冲突  |

- 使用通道7前,需要在web界面触发一次电机运行,激活后会使原先的所有灯语控制失效,避免干扰电机运行
- typeC口如果是由会持续协商充电协议的充电器供电,或者使用espidf调试刷入等,GPIO18,19就会有电平变化<br>
  经典版带串口芯片的不会有这个问题
### 上下料模块
- 上下料模块有多种电机方案,这些方案只是硬件设计不同,在与主控的接线上没有不同
- [起源N20](hard/N20电机方案/README.md)
  - 项目最早的设计方案
- [TT电机](https://makerworld.com.cn/zh/models/1418429-gua-pei-topduo-se-da-yin-de-ttji-chu-ji-v2-0#profileId-1540158)
  - 使用成本更低的TT电机
- [N20D](https://makerworld.com.cn/zh/models/1464399-n20dian-ji-8tong-ji-chu-ji#profileId-1594882)
  - 成对N20电机设计,有适配A1龙门的支架

### 刷入固件
- [固件刷入教程](https://docs.espressif.com/projects/esp-test-tools/zh_CN/latest/esp32/production_stage/tools/flash_download_tool.html)
- 固件刷完要重启
### esp配网
- 使用微信小程序 **一键配网**
- 配网协议选择 **SmartConfig** 
- 填入Wifi信息配网
### 连接打印机MQTT
- 在路由器中查看esp32的ip,登入esp32的web管理页面
  - 如果打印机的ip变动,这里也需要重新连接,可以在路由器中设置MAC地址绑定
- MQTT密码为打印机局域网模式里的密码,**局域网模式开关不影响连接**,建议直接在机器的小屏幕上查看
- 设备序列号除了在机器上直接查看外,也可以在 BambuStudio-设备-固件更新-序列号 中查看
### 配置打印机gcode
- 普通用户请按[A1 mini + Top-AMS：Bambu Studio完整操作流程](doc/A1mini_BambuStudio用户操作流程.md)配置；
  需要了解实现和排障时再查看[通讯、切片选料及自动换料完整教程](doc/A1mini通讯与切片换料完整教程.md)
- 默认保留 Bambu Studio 官方 A1 mini 机器启动 G-code；仓库中的 [A1mini启动_原版.gcode](doc/A1mini启动_原版.gcode) 已同步到官方 `20260513` 参考版本
- 需要自动处理首料时，可显式选择 [A1mini启动_topams首料_20260513.gcode](doc/A1mini启动_topams首料_20260513.gcode)；它只在官方 `M620 M` 前加入首料通道标记、暂停和热床恢复，不作为默认模式
- 推荐使用
  [A1mini换料_topams版本.gcode](doc/A1mini换料_topams版本.gcode)完整替换独立 Top-AMS 打印机预设中的换料 G-code
  - 目前只有A1mini的,但是其他打印机原理上也完全通用,只用改下几个数字就好,欢迎加群测试  
- 普通换料标记使用 **1~8**，可选自动首料标记使用 **9~16**；实际可用数量严格取决于 4/6/8 通道构建
- 退料前的回抽参数会自动读取使用的耗材配置,可在耗材配置内更改
- 切片软件的冲刷体积配置也能正常生效
- 冲刷体积一部分流量会被用于进料,请自行测试合适的冲刷体积
### 开始打印前
- 需要先将待换料的料线推进至能在 **最大进料时间** 内到达打印机耗材传感器的位置；传感器提前确认后会提前停机
- 默认官方启动模式要求预装首卷料，并在页面确认真实当前通道；固件不会再猜测为通道 1
- 卡料、未确认出料、命令回执超时、传感器不转换或 MQTT 超过 8 秒无数据时，电机会停止且打印保持暂停，不会自动重试或自动续打

## 其他  
### 兼容性测试
- A1 mini 固件版本1.04.00
- A1      固件版本1.04.00
- P1S
- 1.05固件加了鉴权,现不支持(?可能开启局域网模式加开发者模式后可以,待测试)
### 讨论
- Q群:8820913⑨九,注明来意
### 配套设施
- [迷你三通](https://makerworld.com.cn/zh/models/1289990-3tong-mini-wu-xu-qi-dong-jie-tou-chao-si-hua#profileId-1383310)
- MW上也有其他没有及时更新到本页面的优秀配套设施,请多加搜索
### 代办
- web心跳机制
- 使用小绿点的自适应进料时间
- 自动续料
    - 思路:默认会续当前通道的下一个通道的料,在切片时候就要注意好.软件架构上,维护一个长度为使用的通道数(通过是否是NC脚判断)的布尔向量,记忆当前通道是否被续料过,这个记忆状态会在本次打印任务结束后重置
- 异常处理
  - mqtt断开
    - mqtt状态的更新和灯语
  - 因为料线刚好没了,无法退线的的报错
- 更小白的固件刷入教程
  
