# 视频脚本：Claude Desktop Buddy × RootMaker 开发板演示
**时长：约 3–4 分钟**

---

## 【开场 / 0:00–0:20】设备亮相

**画面：** 桌面特写，RootMaker 开发板正面朝上，屏幕点亮，显示一个小动画角色处于 **idle（待机）** 状态，轻微眨眼、四处张望。旁边是笔记本电脑，打开 Claude 桌面应用。

**旁白（或字幕）：**
> 这是 claude-desktop-buddy 项目——一个可以感知 Claude 运行状态的硬件伴侣。原版运行在 M5StickC Plus 上，我把它移植到了自己设计的 RootMaker 开发板。

---

## 【第一段 / 0:20–0:50】蓝牙连接

**画面：** 镜头对准电脑屏幕，展示 Claude 桌面应用菜单操作。
操作路径：Help → Troubleshooting → Enable Developer Mode → Developer → Open Hardware Buddy…

**旁白：**
> 先在 Claude 桌面端开启开发者模式，打开 Hardware Buddy 窗口，点击 Connect，从列表里选中 RootMaker 设备。

**画面：** 蓝牙配对成功的瞬间——开发板屏幕上的角色从 **sleep（睡眠）** 状态切换为 **idle（待机）**，眼睛睁开，开始活动。WS2812B RGB 灯亮起。

**旁白：**
> 连接成功，角色醒来了。断开时它会进入睡眠，重新连接自动唤醒。

---

## 【第二段 / 0:50–1:20】Claude 工作中 → busy 状态

**画面：** 在 Claude 桌面端输入一个较长的提问并发送。

**画面切回开发板：** 屏幕上的角色切换为 **busy（工作中）** 动画——角色表现出忙碌、冒汗的状态。

**旁白：**
> 一旦 Claude 开始处理任务，开发板实时同步状态——角色进入"工作模式"，直到任务完成。

---

## 【第三段 / 1:20–1:55】权限审批提醒 → attention 状态

**画面：** 在 Claude 桌面端触发一个需要权限确认的操作（比如文件访问）。

**画面切到开发板：** 角色立刻切换为 **attention（提醒）** 状态，神情警觉；WS2812B 指示灯**快速闪烁**提示。

**旁白：**
> 当 Claude 等待你的权限审批时，设备会立刻给出提醒——这样你不用盯着屏幕也不会错过。

**画面：** 手指点击 RootMaker 触摸屏（CST816T）上的**批准按钮**。

**画面切回电脑：** Claude 继续执行任务。

**画面回到开发板：** 如果在 5 秒内完成审批，角色进入 **heart（爱心）** 状态，飘出爱心动画。

**旁白：**
> 审批直接在设备上完成，5 秒内响应还会有特别反馈。

---

## 【第四段 / 1:55–2:25】彩蛋演示

**画面 1 — 摇一摇：** 拿起开发板轻轻晃动，LIS2DWTR 加速度计检测到震动，角色进入 **dizzy（晕眩）** 状态，螺旋眼、摇摇晃晃。

**旁白：**
> 内置加速度计——摇一摇，角色也会晕。

**画面 2 — 里程碑庆祝：** 展示角色进入 **celebrate（庆祝）** 动画（每累计 50,000 tokens 自动触发）。

**旁白：**
> 每用满 5 万 tokens，它还会自己庆祝一下。

---

## 【第五段 / 2:25–2:50】GIF 角色演示

**画面：** 在 Hardware Buddy 窗口，把自定义角色包文件夹拖入 drop zone，进度条推送中，设备上角色实时替换为新 GIF 角色。

**旁白：**
> 角色也可以换——拖入自定义 GIF 包，通过蓝牙直接推送到设备，无需重新烧录。

---

## 【第六段 / 2:50–3:40】代码简介

**画面：** 切到代码编辑器或 IDE，镜头扫过项目结构，依次停留在几个关键文件上。

**旁白 + 画面对应：**

> 代码分几个模块，每个负责一件事。

**① 打开 `cc_hardware_buddy.ino`，定位到 `loop()` 函数**
> 主循环每帧做这几步：从蓝牙收数据、推导当前状态、处理按键、最后渲染画面。整个程序就是一个简单的状态机——sleep、idle、busy、attention、celebrate、dizzy、heart，七种状态。

**② 切到 `src/ble_bridge.h`，展示 UUID 注释**
> 蓝牙通信用的是 Nordic UART Service 协议，本质上就是把串口搬到了 BLE 上。Claude 桌面端和开发板之间发的是 JSON 字符串，比如权限审批的回复长这样。

**画面：** 在 `cc_hardware_buddy.ino` 中定位 `sendCmd` 的调用处，展示 `{"cmd":"permission","id":"...","decision":"once"}` 这行 JSON。

**③ 切到 `src/character.h`，展示 `characterTick()` 和 `characterSetState()`**
> GIF 角色这块用了两个库：LovyanGFX 负责驱动 ST7789 屏幕，AnimatedGIF 负责逐帧解码动画，文件存在开发板的 LittleFS 文件系统里。

**④ 切到 `cc_hardware_buddy.ino`，定位 `checkShake()` 函数**
> 摇一摇功能靠 LIS2DW12 加速度计库，每 50ms 读一次三轴加速度，算出合力变化超过阈值就触发 dizzy 状态。

**⑤ 切到 `statusLed()` 函数**
> 状态灯就三行代码——Adafruit NeoPixel 库控制 WS2812B，attention 状态下每 400ms 翻转一次，实现闪烁。

**旁白（总结）：**
> 整个移植的核心改动是把 M5StickC Plus 的专有库换成 Ryzobee BSP，显示用 LovyanGFX，加速度计用 LIS2DW12，LED 用 NeoPixel，逻辑层几乎没动。

---

## 【结尾 / 3:40–3:50】收尾

**画面：** 开发板与电脑同框，角色处于 idle 状态，悠闲待机。

**字幕：**
> 移植版：github.com/Ryzobee/Ryzobee_arduino_esp32
> 原项目：github.com/anthropics/claude-desktop-buddy

---

## 拍摄建议

- **机位**：固定俯拍（45°）同时收入开发板和电脑屏幕；关键状态切换时推近拍开发板特写
- **重点镜头**：idle→busy→attention→heart 这条完整链路，以及摇一摇 dizzy 效果
- **灯光**：确保 WS2812B 灯光变化在画面中清晰可见
- **剪辑**：状态切换瞬间可加 0.5 秒慢动作强调
