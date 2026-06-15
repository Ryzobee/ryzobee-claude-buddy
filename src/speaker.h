#pragma once
#include <stdint.h>

/*
 * speaker.h — CI1302 音频芯片 Arduino 驱动接口
 *
 * 引脚来自 D:\espcode\rootmaker1d54-robot\main\board_pins.h:
 *   BOARD_PIN_THERMAL_UART_TX  GPIO_NUM_4  → CI1302 RXD
 *   BOARD_PIN_THERMAL_UART_RX  GPIO_NUM_6  → CI1302 TXD
 *
 * 波特率：921600（与 ESP-IDF 工程 ci1302_init() 一致）
 *
 * CI1302 内置音频 ID（对应 tone_res/cmd_1.mp3 ~ cmd_4.mp3）：
 *   1 → cmd_1（低音提示）
 *   2 → cmd_2（中低音）
 *   3 → cmd_3（中高音）
 *   4 → cmd_4（高音提示）
 * 如需调整音效映射，修改 speaker.cpp 中的 speakerBeep() 即可。
 */

// ── 引脚定义（与 board_pins.h 保持一致）─────────────────────────────────────
#define SPEAKER_UART_TX   37  // CI1302 UART TX
#define SPEAKER_UART_RX   36  // CI1302 UART RX

// ── 公开接口 ──────────────────────────────────────────────────────────────────

/** 初始化 CI1302 UART，在 setup() 中调用一次。 */
void speakerInit();

/**
 * 播放提示音。
 * @param freq    频率（Hz），用于映射到 CI1302 内置音频 ID（0 = 静音）
 * @param dur_ms  时长（ms，当前版本由 CI1302 自行控制播放时长）
 */
void speakerBeep(uint16_t freq, uint16_t dur_ms);

/** 设置 CI1302 音量（1–7，默认 5）。 */
void speakerSetVolume(uint8_t level);

/** 停止当前播放。 */
void speakerStop();
