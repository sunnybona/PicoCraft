// galagino 风格 PWM 音频：固定高载波（约 61kHz，人耳不可闻）+ DMA 连续环形
// PCM，全有或全无块写入、显式静音、空闲彻底停硬件。替代 picosystem 蜂鸣器模型。
#ifndef AUDIO_PWM_H
#define AUDIO_PWM_H

#include <stdint.h>
#include "audio_i2s.h"   // 复用 SfxSpec 定义（PWM 构建不编译 audio_i2s.cpp）

void pwm_audio_init();
void pwm_audio_trigger(const SfxSpec &spec);   // 非阻塞触发音效
void pwm_audio_pump();                         // 每帧调用，块写入/显式静音/按需停硬件

#endif
