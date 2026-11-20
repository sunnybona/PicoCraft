// I2S 数字音频：PIO + 外部 I2S DAC（如 MAX98357A），DMA 环形缓冲，44.1kHz。
// 与 picosystem 的 PWM 音频独立，本模块自带实时波形合成器（正弦+包络+噪声+bend）。
#ifndef AUDIO_I2S_H
#define AUDIO_I2S_H

#include <stdint.h>
#include <stddef.h>

// 一个音效的合成参数（与 picosystem voice 的语义对应）
struct SfxSpec {
    uint32_t freq;         // 起始频率 Hz
    uint32_t duration_ms;  // 持续（sustain 段）时长
    uint32_t attack_ms;
    uint32_t decay_ms;
    uint8_t  sustain;      // 0..100
    uint32_t release_ms;
    int32_t  bend;         // 频率滑变 Hz（0=不滑）
    uint32_t bend_ms;
    uint8_t  noise;        // 0..100，叠加噪声强度
    uint8_t  volume;       // 0..100
};

void i2s_audio_init();
void i2s_trigger(const SfxSpec &spec);   // 触发一个音效（非阻塞，立即返回）
void i2s_pump();                         // 每帧调用，按合成器填充环形缓冲

#endif
