/*
 * speaker.cpp — CI1302 音频芯片 Arduino 驱动
 * CI1302 通过 UART(TX=37, RX=36, 921600bps) 接受 PCM 流播放。
 * 所有 UART 通信在独立 FreeRTOS task 中执行，不阻塞主循环。
 */

#include "speaker.h"
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

// ── 协议常量 ──────────────────────────────────────────────────────────────
#define CI1302_BAUDRATE     921600
#define CI1302_FRAME_MIN    16

static const uint8_t CI1302_MAGIC[4] = { 0xa5, 0xa5, 0x5a, 0x5a };
static const uint8_t CI1302_FILL[4]  = { 0x78, 0x56, 0x34, 0x12 };

#define CMD_SET_AUDIO_VOLUME     0x0117
#define CMD_NET_PLAY_START       0x0201
#define CMD_NET_PLAY_STOP        0x0204
#define CMD_PLAY_DATA_RECV       0x020b
#define CMD_PLAY_DATA_END        0x020c
#define CMD_PLAY_DATA_GET        0x020a
#define CMD_CIAS_AUDIO_RST       0x0603
#define CMD_CIAS_AUDIO_SYS_READY 0x0601

#define PCM_SAMPLE_RATE  16000
#define PCM_AMPLITUDE    28000   // 接近最大音量

// ── 内部状态 ──────────────────────────────────────────────────────────────
struct BeepReq { uint16_t freq; uint16_t dur_ms; };

static HardwareSerial      s_serial(1);
static QueueHandle_t       s_queue       = nullptr;
static volatile bool       s_cancel      = false;
static bool                s_initialized = false;

// ── 帧发送 ────────────────────────────────────────────────────────────────
static void send_frame(uint16_t cmd, const uint8_t* payload, uint16_t len) {
    uint16_t csum = 0;
    for (uint16_t i = 0; i < len; i++) csum += payload[i];

    uint8_t hdr[CI1302_FRAME_MIN];
    memcpy(hdr, CI1302_MAGIC, 4);
    hdr[4] = csum & 0xFF;  hdr[5] = (csum >> 8) & 0xFF;
    hdr[6] = cmd  & 0xFF;  hdr[7] = (cmd  >> 8) & 0xFF;
    hdr[8] = len  & 0xFF;  hdr[9] = (len  >> 8) & 0xFF;
    hdr[10] = 0; hdr[11] = 0;
    memcpy(hdr + 12, CI1302_FILL, 4);

    s_serial.write(hdr, CI1302_FRAME_MIN);
    if (len > 0 && payload) s_serial.write(payload, len);
    s_serial.flush();
}

// ── 帧接收（滑动窗口，16字节头）──────────────────────────────────────────
static int32_t read_one_frame(uint32_t timeout_ms) {
    uint32_t start = millis();
    uint8_t  win[CI1302_FRAME_MIN];
    memset(win, 0, sizeof(win));

    while (millis() - start < timeout_ms) {
        while (s_serial.available()) {
            memmove(win, win + 1, CI1302_FRAME_MIN - 1);
            win[CI1302_FRAME_MIN - 1] = (uint8_t)s_serial.read();

            if (win[0] != 0xa5 || win[1] != 0xa5 ||
                win[2] != 0x5a || win[3] != 0x5a) continue;

            uint16_t cmd = win[6] | ((uint16_t)win[7] << 8);
            uint16_t len = win[8] | ((uint16_t)win[9] << 8);

            // 丢弃 payload
            uint32_t ps = millis();
            uint16_t got = 0;
            while (got < len && millis() - ps < 100) {
                if (s_serial.available()) { s_serial.read(); got++; }
                else vTaskDelay(1);
            }
            return (int32_t)cmd;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return -1;
}

static bool wait_cmd(uint16_t expected, uint32_t timeout_ms) {
    uint32_t start = millis();
    while (millis() - start < timeout_ms) {
        uint32_t left = timeout_ms - (millis() - start);
        int32_t  cmd  = read_one_frame(left < 50 ? left : 50);
        if (cmd == (int32_t)expected) return true;
    }
    return false;
}

// ── 猫叫声 PCM 生成 ───────────────────────────────────────────────────────
// 频率包络：400→1300→600 Hz，带振动和泛音，约 300ms
static void generate_meow(uint8_t* pcm, uint32_t num_samples) {
    float phase1 = 0.0f;  // 基频
    float phase2 = 0.0f;  // 二次谐波

    for (uint32_t i = 0; i < num_samples; i++) {
        float progress = (float)i / num_samples;

        // 频率包络
        float freq;
        if (progress < 0.25f) {
            freq = 400.0f + (1300.0f - 400.0f) * (progress / 0.25f);
        } else if (progress < 0.55f) {
            freq = 1300.0f - (1300.0f - 900.0f) * ((progress - 0.25f) / 0.30f);
        } else {
            freq = 900.0f - (900.0f - 400.0f) * ((progress - 0.55f) / 0.45f);
        }

        // 轻微颤音（8Hz 调制）
        float t = (float)i / PCM_SAMPLE_RATE;
        freq *= (1.0f + 0.025f * sinf(2.0f * (float)M_PI * 8.0f * t));

        // 振幅包络：淡入 + 平稳 + 淡出
        float env;
        if (progress < 0.08f)      env = progress / 0.08f;
        else if (progress > 0.80f) env = (1.0f - progress) / 0.20f;
        else                       env = 1.0f;

        // 更新相位
        float step = 2.0f * (float)M_PI * freq / PCM_SAMPLE_RATE;
        phase1 += step;
        phase2 += step * 2.1f;  // 轻微失谐的二次谐波
        if (phase1 > 2.0f * (float)M_PI) phase1 -= 2.0f * (float)M_PI;
        if (phase2 > 2.0f * (float)M_PI) phase2 -= 2.0f * (float)M_PI;

        // 混合基频 + 谐波（7:3）
        float sample = env * PCM_AMPLITUDE * (0.70f * sinf(phase1) + 0.30f * sinf(phase2));
        int16_t s = (int16_t)sample;
        pcm[i*2]   = (uint8_t)(s & 0xFF);
        pcm[i*2+1] = (uint8_t)((s >> 8) & 0xFF);
    }
}

// ── 实际播放 ──────────────────────────────────────────────────────────────
static void do_beep(uint16_t dur_ms) {
    if (dur_ms < 50)  dur_ms = 300;   // 默认猫叫时长
    if (dur_ms > 600) dur_ms = 600;

    uint32_t num_samples = (uint32_t)PCM_SAMPLE_RATE * dur_ms / 1000;
    uint32_t pcm_bytes   = num_samples * 2;

    uint8_t* pcm = (uint8_t*)malloc(pcm_bytes);
    if (!pcm) return;

    generate_meow(pcm, num_samples);

    send_frame(CMD_NET_PLAY_START, nullptr, 0);

    const uint32_t CHUNK    = 1024;
    uint32_t       sent     = 0;
    uint32_t       deadline = millis() + 4000;
    bool           ended    = false;

    while (millis() < deadline && !s_cancel) {
        int32_t cmd = read_one_frame(200);

        if (cmd == CMD_PLAY_DATA_GET) {
            if (sent >= pcm_bytes) {
                send_frame(CMD_PLAY_DATA_END, nullptr, 0);
                ended = true;
                break;
            }
            uint32_t chunk = pcm_bytes - sent;
            if (chunk > CHUNK) chunk = CHUNK;
            send_frame(CMD_PLAY_DATA_RECV, pcm + sent, (uint16_t)chunk);
            sent += chunk;
        } else if (cmd == 0x020d) {
            ended = true;
            break;
        }
    }

    if (!ended) send_frame(CMD_PLAY_DATA_END, nullptr, 0);
    free(pcm);
}

// ── Speaker 任务 ──────────────────────────────────────────────────────────
static void speaker_task(void* arg) {
    send_frame(CMD_CIAS_AUDIO_RST, nullptr, 0);
    wait_cmd(CMD_CIAS_AUDIO_SYS_READY, 5000);
    vTaskDelay(pdMS_TO_TICKS(1000));

    uint8_t vol[1] = { 7 };
    send_frame(CMD_SET_AUDIO_VOLUME, vol, 1);

    s_initialized = true;

    // 开机猫叫
    do_beep(300);

    BeepReq req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE) {
            s_cancel = false;
            do_beep(req.dur_ms);
        }
    }
}

// ── 公开接口 ──────────────────────────────────────────────────────────────

void speakerInit() {
    s_serial.begin(CI1302_BAUDRATE, SERIAL_8N1, SPEAKER_UART_RX, SPEAKER_UART_TX);
    s_queue = xQueueCreate(1, sizeof(BeepReq));  // 队列长度 1：只保留最新请求
    xTaskCreatePinnedToCore(speaker_task, "spk", 4096, nullptr, 5, nullptr, 0);
}

void speakerSetVolume(uint8_t level) {
    if (level < 1) level = 1;
    if (level > 7) level = 7;
    uint8_t payload[1] = { level };
    send_frame(CMD_SET_AUDIO_VOLUME, payload, 1);
}

void speakerBeep(uint16_t freq, uint16_t dur_ms) {
    if (!s_queue) return;

    // 取消当前播放，清空队列，只保留最新一次
    s_cancel = true;
    BeepReq old;
    while (xQueueReceive(s_queue, &old, 0) == pdTRUE) {}

    BeepReq req = { freq, dur_ms };
    xQueueSend(s_queue, &req, 0);
}

void speakerStop() {
    s_cancel = true;
    BeepReq old;
    while (s_queue && xQueueReceive(s_queue, &old, 0) == pdTRUE) {}
    if (s_initialized) send_frame(CMD_NET_PLAY_STOP, nullptr, 0);
}
