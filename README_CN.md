# Claude Desktop Buddy — RootMaker 移植版

[English](README.md)

这是 Anthropic 官方项目 [claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy) 在 **Ryzobee RootMaker** 开发板（ESP32-S3）上的移植版本。

原版固件运行在 M5StickC Plus 上。本版本用 Ryzobee BSP 替换了板级驱动，保留了全部应用逻辑，并将原版蜂鸣器升级为 **CI1302 音频芯片**——同时支持**内置预存音频**（按 ID 触发）和**实时 PCM 流式输出**，音效更加丰富。

![设备运行效果](docs/device.jpg)

---

## 硬件

| 组件 | 规格 |
|------|------|
| 主控 | Ryzobee RootMaker（ESP32-S3） |
| 屏幕 | 240×240 ST7789 SPI LCD（LovyanGFX 驱动） |
| 触摸 | CST816T |
| IMU | LIS2DWTR / LIS2DW12 三轴加速度计 |
| RGB 灯 | WS2812B（GPIO 45） |
| 音频 | CI1302 音频芯片（UART，TX GPIO 37 / RX GPIO 36） |
| 按键 | 单按键（GPIO 0） |

---

## 与原版的主要区别

- **开发板**：Ryzobee RootMaker（ESP32-S3），取代 M5StickC Plus
- **屏幕**：240×240 方形屏，原版为 135×240
- **音频**：原版蜂鸣器升级为 CI1302 音频芯片，支持**内置预存音频文件**（按 ID 播放，如 `cmd_1.mp3` ~ `cmd_4.mp3`）和**实时 PCM 流式传输**（合成波形）两种模式，音效更丰富
- **按键**：单按键布局（短按 / 长按），原版为双按键
- **BSP**：使用 [Ryzobee Arduino 库](https://github.com/Ryzobee/Ryzobee_arduino_esp32) 进行硬件抽象

---

## 依赖库

编译前请在 Arduino IDE 中安装以下库：

| 库名 | 最低版本 | 来源 |
|------|---------|------|
| arduino-esp32 | ≥ 3.3.7 | [espressif/arduino-esp32](https://github.com/espressif/arduino-esp32) |
| Ryzobee | 最新版 | [Ryzobee/Ryzobee_arduino_esp32](https://github.com/Ryzobee/Ryzobee_arduino_esp32) |
| LovyanGFX | ≥ 1.2.19 | [lovyan03/LovyanGFX](https://github.com/lovyan03/LovyanGFX) |
| Adafruit NeoPixel | ≥ 1.12.3 | [adafruit/Adafruit_NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) |
| LIS2DW12 | ≥ 2.1.1 | [stm32duino/LIS2DW12](https://github.com/stm32duino/LIS2DW12) |
| AnimatedGIF | ≥ 2.2.0 | [bitbank2/AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) |

---

## Arduino IDE 配置

在 Arduino IDE 中选择开发板时，使用以下参数：

| 参数 | 值 |
|------|----|
| 开发板 | RootMaker（通过 Ryzobee 开发板包） |
| Flash 模式 | QIO 80MHz |
| Flash 大小 | 16MB |
| PSRAM | QSPI PSRAM |

---

## 编译与烧录

1. 安装上述所有依赖库。
2. 在 Arduino IDE 首选项中添加 Ryzobee 开发板包 URL。
3. 打开 `cc_hardware_buddy.ino`。
4. 选择 RootMaker 开发板并配置上述参数。
5. 点击 **上传**。

---

## 与 Claude 配对

1. 在 Claude 桌面端开启开发者模式：**Help → Troubleshooting → Enable Developer Mode**
2. 打开：**Developer → Open Hardware Buddy…**
3. 点击 **Connect**，从列表中选择你的设备（设备名为 `Claude-XXXX`）。
4. macOS 首次连接时会请求蓝牙权限，允许即可。

配对成功后，双方同时在线时会自动重连。

---

## 按键操作

| 按法 | 普通 / 宠物 / 信息页 | 审批界面 |
|------|---------------------|---------|
| **短按** | 切换屏幕 / 翻页 | **批准** |
| **长按** | 打开菜单 / 确认 | **拒绝** |

---

## 状态说明

| 状态 | 触发条件 |
|------|---------|
| `sleep` | 蓝牙未连接 |
| `idle` | 已连接，无待处理事项 |
| `busy` | 同时有 3 个及以上会话运行中 |
| `attention` | 等待权限审批（RGB 灯闪烁提示） |
| `celebrate` | 每累计 50,000 tokens 触发一次 |
| `dizzy` | 设备被摇晃 |
| `heart` | 在 5 秒内完成审批 |

---

## ASCII 宠物

内置 19 种角色：水豚、蝾螈、史莱姆、仙人掌、鹅、幽灵、蘑菇、机器人、蜗牛、乌龟、龙、肥猫、鸭子、猫、章鱼、猫头鹰、企鹅、兔子等。通过 **设置 → ascii pet** 或菜单切换。

---

## GIF 自定义角色

将角色包文件夹拖入 Hardware Buddy 窗口的拖放区，应用会通过蓝牙将文件推送到设备，设备实时切换为 GIF 模式，无需重新烧录。

角色包格式：一个包含 `manifest.json` 和各状态 GIF 文件（宽 96px）的文件夹，状态包括 `sleep`、`idle`、`busy`、`attention`、`celebrate`、`dizzy`、`heart`。

详细格式说明请参考[原版项目](https://github.com/anthropics/claude-desktop-buddy)。

---

## 项目结构

```
cc_hardware_buddy.ino   — 主循环、状态机、UI 渲染
src/
  ble_bridge.h/.cpp     — Nordic UART BLE 服务
  buddy.h/.cpp          — ASCII 角色调度与渲染
  buddies/              — 每个角色一个 .cpp（共 19 种）
  character.h/.cpp      — GIF 解码与渲染
  data.h                — 通信协议、JSON 解析
  persona_state.h       — 状态枚举
  speaker.h/.cpp        — CI1302 音频芯片驱动（内置音频 + 实时 PCM 流式输出）
  stats.h               — NVS 持久化统计与设置
  xfer.h                — BLE 文件夹推送接收器
```

---

## 许可证

MIT 协议，详见 [LICENSE](LICENSE)。

基于 [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)（MIT 协议）开发。
