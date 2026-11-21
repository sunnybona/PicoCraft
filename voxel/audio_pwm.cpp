// galagino-max 音频纪律的 PWM 后端（RP2040/RP2350，GPIO11）
//
// 严格对应 galagino 的 hal::AudioOut 块模型（hal::AudioConfig{rate, chunk,
// buffers}，默认 chunk=64、buffers=4）：
//   - 物理外设异步循环播放 buffers 个定长 chunk；每次 write 全有或全无，
//     拿到空闲 chunk 才整块写入，否则立即返回（绝不半块写）。
//   - 播完的 chunk 回收为空闲（对应各后端 onConvertDone -> freeBuffers）。
//   - flushSilence：启动/停止时把全部 chunk 填静音，旧内容不会循环（防 hum）。
//   - 欠载只播静音块，不重复旧音效；有符号 16bit 样本加中点偏置（对应
//     esp SILENCE=0x8000 的 unsigned 中点），空闲彻底停 PWM/DMA。
#include "audio_pwm.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"

// ---------------- galagino AudioConfig 等价参数 ----------------
#define PIN_AUDIO    11
#define CHUNK        64u    // galagino AUDIO_CHUNK / cfg.chunk
#define BUF_COUNT    4u     // galagino DMA_BUF_COUNT / cfg.buffers
#define TOTAL_SLOTS  (CHUNK * BUF_COUNT)      // 256 样本

// 12bit 固定高载波：250MHz/4096 ≈ 61035Hz（不可闻），载波率即采样率
#define PWM_BITS     12
#define PWM_WRAP     ((1u << PWM_BITS) - 1u)  // 4095
#define PWM_MID      (PWM_WRAP / 2u)          // 2048 = 静音中点（对应 SILENCE）
#define SAMPLE_RATE  61035u

// 整个块缓冲 = 256 样本 * 2 = 512 字节，DMA ring 覆盖全部 chunk
static constexpr uint8_t ring_bits(uint32_t bytes) {
    return bytes <= 1u ? 0u : (uint8_t)(1u + ring_bits(bytes >> 1));
}
#define RING_BITS ring_bits(TOTAL_SLOTS * 2u)   // 9

static uint16_t s_buf[TOTAL_SLOTS] __attribute__((aligned(TOTAL_SLOTS * 2u)));

static uint s_slice;
static uint s_dma_ch;
static bool s_stream_on;

// galagino 块计数：filled=已交给外设的 chunk 总数，completed=外设已播完的 chunk 总数
static volatile uint32_t s_filled_blocks;
static volatile uint32_t s_completed_blocks;
static uint32_t s_prev_local;
static uint32_t s_wrap_count;

static struct repeating_timer s_timer;
static volatile bool s_pending_stop;   // 中断请求主循环停硬件（避免在 ISR 内 cancel 自己）

// ---------------- 合成器（对应 galagino machineBase::renderAudio） ----------------
static bool     syn_active;
static uint32_t syn_s;
static uint32_t syn_a0, syn_d0, syn_sus0, syn_total;
static float    syn_freq, syn_sus, syn_vol;
static int32_t  syn_bend;
static uint32_t syn_bend_s;
static uint8_t  syn_noise;
static uint32_t syn_phase;
static uint32_t syn_nstate;

static const int16_t sin_tab[256] = {
       0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
    6393,  7179,  7960,  8736,  9506, 10269, 11024, 11771,
   12509, 13237, 13955, 14662, 15356, 16038, 16706, 17360,
   17998, 18620, 19225, 19812, 20381, 20930, 21459, 21967,
   22453, 22916, 23356, 23772, 24163, 24529, 24870, 25184,
   25472, 25732, 25965, 26170, 26347, 26495, 26615, 26706,
   26768, 26801, 26805, 26780, 26727, 26645, 26535, 26397,
   26232, 26039, 25820, 25575, 25305, 25010, 24692, 24351,
   23988, 23604, 23200, 22777, 22336, 21878, 21403, 20914,
   20411, 19895, 19366, 18826, 18276, 17717, 17150, 16576,
   15998, 15415, 14828, 14239, 13649, 13059, 12471, 11885,
   11303, 10726, 10156,  9594,  9041,  8498,  7966,  7446,
    6939,  6446,  5968,  5506,  5060,  4632,  4222,  3830,
    3458,  3106,  2774,  2463,  2174,  1907,  1662,  1440,
    1240,  1063,   908,   775,   665,   577,   511,   467,
     444,   443,   463,   504,   647,   748,   869,  1009,
    1167,  1343,  1536,  1745,  1969,  2207,  2459,  2723,
    2999,  3286,  3582,  3887,  4200,  4520,  4846,  5177,
    5512,  5849,  6188,  6527,  6865,  7201,  7534,  7864,
    8189,  8508,  8820,  9125,  9421,  9708,  9985, 10251,
   10506, 10749, 10980, 11198, 11402, 11592, 11768, 11930,
   12077, 12209, 12326, 12428, 12513, 12583, 12636, 12673,
   12694, 12686, 12657, 12612, 12551, 12381, 12273, 12149,
   11858, 11691, 11510, 11316, 11109, 10890, 10659, 10417,
    9632,  9353,  9066,  8772,  8472,  8166,  7855,  7540,
    7222,  6900,  6576,  6250,  5923,  5596,  5307,  4979,
    4653,  4329,  4008,  3690,  3376,  3067,  2763,  2465,
    2173,  1888,  1610,  1340,  1078,   824,   580,   345,
     120,   -95,  -308,  -519,  -727,  -932, -1134, -1332,
   -1900, -2080, -2254, -2423, -2585, -2741, -2890, -3032,
   -3168, -3296, -3416, -3529, -3633, -3729, -3817, -3895
};

static uint32_t ms_to_samps(uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * SAMPLE_RATE) / 1000u);
}

// 有符号 16bit -> 12bit PWM 比较值：加中点偏置（对应 galagino SILENCE 中点）
static uint16_t to_level(int16_t v) {
    int32_t s = (int32_t)v >> (16 - PWM_BITS);
    s += PWM_MID;
    if (s < 0) s = 0;
    if (s > (int32_t)PWM_WRAP) s = PWM_WRAP;
    return (uint16_t)s;
}

static int16_t synth_sample() {
    if (!syn_active) return 0;
    if (syn_s >= syn_total) { syn_active = false; return 0; }

    float cur_f = syn_freq;
    if (syn_bend != 0 && syn_bend_s > 0) {
        float frac = (float)syn_s / (float)syn_bend_s;
        if (frac > 1.0f) frac = 1.0f;
        cur_f = syn_freq + (float)syn_bend * frac;
    }
    syn_phase += (uint32_t)((cur_f / (float)SAMPLE_RATE) * 4294967296.0f);
    int32_t wave = sin_tab[syn_phase >> 24];

    float env;
    if (syn_s < syn_a0) {
        env = (float)syn_s / (float)syn_a0;
    } else if (syn_s < syn_d0) {
        float f = (float)(syn_s - syn_a0) / (float)(syn_d0 - syn_a0);
        env = 1.0f - f * (1.0f - syn_sus);
    } else if (syn_s < syn_sus0) {
        env = syn_sus;
    } else {
        uint32_t span = syn_total - syn_sus0;
        float f = (float)(syn_s - syn_sus0) / (float)span;
        env = syn_sus * (1.0f - f);
    }

    int32_t out = (int32_t)((float)wave * env * syn_vol);

    if (syn_noise > 0) {
        syn_nstate = syn_nstate * 1103515245u + 12345u;
        int32_t rnd = (int32_t)(syn_nstate >> 16);
        int32_t namp = (int32_t)syn_noise * 300;
        out += (rnd * namp) / 32768;
    }

    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;

    syn_s++;
    return (int16_t)out;
}

// ---------------- galagino 块回收（onConvertDone 等价） ----------------
// 读 DMA 当前读地址，把外设已完整播完的 chunk 计为空闲。
static void reclaim() {
    uint32_t addr = dma_channel_hw_addr(s_dma_ch)->read_addr;
    uint32_t local = (addr - (uint32_t)(uintptr_t)s_buf) >> 1;  // 0..TOTAL_SLOTS-1
    if (local < s_prev_local) s_wrap_count++;
    s_prev_local = local;
    uint32_t global = s_wrap_count * TOTAL_SLOTS + local;
    s_completed_blocks = global / CHUNK;
}

static uint32_t free_chunks() {
    int32_t queued = (int32_t)(s_filled_blocks - s_completed_blocks);
    if (queued < 0) queued = 0;
    if (queued >= (int32_t)BUF_COUNT) return 0;
    return BUF_COUNT - (uint32_t)queued;
}

// ---------------- 流启停 ----------------
static void stream_stop() {
    if (!s_stream_on) return;
    s_pending_stop = false;
    cancel_repeating_timer(&s_timer);
    pwm_set_enabled(s_slice, false);
    dma_channel_abort(s_dma_ch);
    while (dma_channel_is_busy(s_dma_ch)) tight_loop_contents();
    pwm_set_gpio_level(PIN_AUDIO, 0);
    s_stream_on = false;
}

static bool timer_cb(struct repeating_timer *t);

// galagino Audio::init/flushSilence：全部 chunk 预填静音后再启动外设
static void stream_start() {
    for (uint32_t i = 0; i < TOTAL_SLOTS; i++) s_buf[i] = PWM_MID;
    s_filled_blocks = BUF_COUNT;      // 初始静音块视为已全部在播
    s_completed_blocks = 0;
    s_prev_local = 0;
    s_wrap_count = 0;

    pwm_set_clkdiv(s_slice, 1.0f);
    pwm_set_wrap(s_slice, PWM_WRAP);
    pwm_set_gpio_level(PIN_AUDIO, PWM_MID);
    pwm_set_enabled(s_slice, true);
    s_stream_on = true;

    dma_channel_transfer_from_buffer_now(s_dma_ch, s_buf, 0xFFFFFFFFu);
    add_repeating_timer_ms(-1, timer_cb, NULL, &s_timer);
}

// 纯软件定时回收（不碰音频引脚，无可闻活动）；空闲尾音播完即停硬件。
static bool timer_cb(struct repeating_timer *t) {
    (void)t;
    reclaim();
    if (!syn_active && free_chunks() >= BUF_COUNT) {
        s_pending_stop = true;   // 由主循环 pump 执行 stream_stop，避免在 ISR 内 cancel
    }
    return true;
}

// ---------------- 公共 API ----------------
void pwm_audio_init() {
    s_slice = pwm_gpio_to_slice_num(PIN_AUDIO);

    gpio_set_function(PIN_AUDIO, GPIO_FUNC_PWM);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 1.0f);
    pwm_config_set_wrap(&cfg, PWM_WRAP);
    pwm_init(s_slice, &cfg, false);
    pwm_set_gpio_level(PIN_AUDIO, 0);

    // 屏幕固定占 DMA 通道 0（未走 claim），先标记占用避免误拿
    if (!dma_channel_is_claimed(0)) dma_channel_claim(0);
    s_dma_ch = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_16);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pwm_get_dreq(s_slice));
    channel_config_set_ring(&dc, false, RING_BITS);
    dma_channel_configure(s_dma_ch, &dc,
                          &pwm_hw->slice[s_slice].cc,
                          s_buf, 0, false);

    syn_active = false;
    s_stream_on = false;   // 开机保持关断，完全安静
}

void pwm_audio_trigger(const SfxSpec &spec) {
    syn_phase = 0;
    syn_s = 0;
    syn_freq = (float)spec.freq;
    syn_sus = (float)spec.sustain / 100.0f;
    syn_vol = (float)spec.volume / 100.0f;
    syn_bend = spec.bend;
    syn_bend_s = ms_to_samps(spec.bend_ms);
    syn_noise = spec.noise;
    syn_nstate = 1;

    uint32_t a = ms_to_samps(spec.attack_ms);
    uint32_t d = ms_to_samps(spec.decay_ms);
    uint32_t dur = ms_to_samps(spec.duration_ms);
    uint32_t rel = ms_to_samps(spec.release_ms);
    if (a == 0) a = 1;
    if (d == 0) d = 1;
    if (rel == 0) rel = 1;

    syn_a0 = a;
    syn_d0 = a + d;
    syn_sus0 = dur;
    syn_total = dur + rel;
    syn_active = true;

    if (!s_stream_on) stream_start();
    pwm_audio_pump();
}

// galagino Audio::transmit：先回收/送块（全有或全无），再渲染下一块。
void pwm_audio_pump() {
    if (!s_stream_on) return;
    reclaim();
    if (s_pending_stop) { stream_stop(); return; }

    if (!syn_active) {
        if (free_chunks() >= BUF_COUNT) stream_stop();
        return;
    }

    // 有空闲 chunk 就整块写入；合成器结束后 synth_sample 返回 0 -> 静音中点
    uint32_t nfree = free_chunks();
    for (uint32_t b = 0; b < nfree; b++) {
        uint32_t slot = (s_filled_blocks % BUF_COUNT) * CHUNK;
        for (uint32_t i = 0; i < CHUNK; i++) {
            s_buf[slot + i] = to_level(synth_sample());
        }
        s_filled_blocks++;
    }
}
