#include "audio_i2s.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "i2s.pio.h"

// ---------------- 常量 ----------------
#define SAMPLE_RATE   44100u
#define RING_SAMPLES  2048u          // 单声道样本数（每样本占 2 个 32bit 槽）
#define RING_WORDS    (RING_SAMPLES * 2u)   // 4096 word = 16KB，约 46ms 缓冲
#define RING_MASK     (RING_WORDS - 1u)
#define MARGIN_WORDS  32u            // 离读指针保留的安全距离（word）
// 启动/欠载重同步时的预载量（3/4 环静音提前量），吸收游戏低帧率下的帧抖动，
// 保证 DMA 不会读到上一轮音效的残留。
#define PRELOAD_WORDS ((RING_WORDS * 3u) / 4u)

// I2S 引脚（PicoBoy 专用，无 RGB LED 冲突）
#define PIN_I2S_DATA  13
#define PIN_I2S_BCLK  14
#define PIN_I2S_LRCLK 15

// ring 折回字节数 = RING_WORDS*4 = 8192 → 13 位
static constexpr uint8_t ring_addr_bits(uint32_t bytes) {
    return bytes <= 1u ? 0u : (uint8_t)(1u + ring_addr_bits(bytes >> 1));
}
#define RING_ADDR_BITS ring_addr_bits(RING_WORDS * 4u)

static uint32_t s_ring[RING_WORDS] __attribute__((aligned(RING_WORDS * 4u)));

static PIO    s_pio = pio1;
static uint   s_sm;
static uint   s_prog_off;
static uint   s_dma_ch;

// ---- 合成器状态 ----
static bool     syn_active;
static uint32_t syn_s;               // 已生成样本数
static uint32_t syn_a0, syn_d0, syn_sus0, syn_total;  // 包络区间端点（样本）
static float    syn_freq, syn_sus, syn_vol;
static int32_t  syn_bend;
static uint32_t syn_bend_s;
static uint8_t  syn_noise;
static uint32_t syn_phase;
static uint32_t syn_nstate;

// 256 点正弦表（有符号 16bit）
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
     444,   443,   463,   504,   565,   647,   748,   869,
    1009,  1167,  1343,  1536,  1745,  1969,  2207,  2459,
    2723,  2999,  3286,  3582,  3887,  4200,  4520,  4846,
    5177,  5512,  5849,  6188,  6527,  6865,  7201,  7534,
    7864,  8189,  8508,  8820,  9125,  9421,  9708,  9985,
   10251, 10506, 10749, 10980, 11198, 11402, 11592, 11768,
   11930, 12077, 12209, 12326, 12428, 12513, 12583, 12636,
   12673, 12694, 12698, 12686, 12657, 12612, 12551, 12474,
   12381, 12273, 12149, 12011, 11858, 11691, 11510, 11316,
   11109, 10890, 10659, 10417, 10165,  9903,  9632,  9353,
    9066,  8772,  8472,  8166,  7855,  7540,  7222,  6900,
    6576,  6250,  5923,  5596,  5307,  4979,  4653,  4329,
    4008,  3690,  3376,  3067,  2763,  2465,  2173,  1888,
    1610,  1340,  1078,   824,   580,   345,   120,   -95,
    -308,  -519,  -727,  -932, -1134, -1332, -1526, -1716,
   -1900, -2080, -2254, -2423, -2585, -2741, -2890, -3032,
   -3168, -3296, -3416, -3529, -3633, -3729, -3817, -3895
};

// ---------------- 合成器 ----------------
static uint32_t ms_to_samps(uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * SAMPLE_RATE) / 1000u);
}

void i2s_trigger(const SfxSpec &spec) {
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
}

// 生成一个单声道样本（中心 0）
static int16_t synth_sample() {
    if (!syn_active) return 0;
    if (syn_s >= syn_total) { syn_active = false; return 0; }

    // bend：当前频率
    float cur_f = syn_freq;
    if (syn_bend != 0 && syn_bend_s > 0) {
        float frac = (float)syn_s / (float)syn_bend_s;
        if (frac > 1.0f) frac = 1.0f;
        cur_f = syn_freq + (float)syn_bend * frac;
    }
    syn_phase += (uint32_t)((cur_f / (float)SAMPLE_RATE) * 4294967296.0f);
    int32_t wave = sin_tab[syn_phase >> 24];

    // 包络
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
        int32_t rnd = (int32_t)(syn_nstate >> 16);          // 有符号伪随机
        int32_t namp = (int32_t)syn_noise * 300;            // 100 → 30000
        out += (rnd * namp) / 32768;
    }

    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;

    syn_s++;
    return (int16_t)out;
}

// ---------------- DMA 环内写指针 ----------------
// 不做跨环累计，wp 始终是环内索引（0..RING_WORDS-1），绕环自然处理，
// 即使一帧内 DMA 绕多圈也不会累计失步。
static uint32_t s_wp;

// ---------------- 每帧填充 ----------------
void i2s_pump() {
    // DMA 当前环内读索引（真实位置，0..RING_WORDS-1）
    uint32_t ridx = ((uint32_t)dma_channel_hw_addr(s_dma_ch)->read_addr -
                     (uint32_t)(uintptr_t)s_ring) >> 2;

    // 写到读指针后方、保留 MARGIN 安全距离的位置（环内前向空间）
    int32_t space = (int32_t)(ridx - MARGIN_WORDS) - (int32_t)s_wp;
    if (space < 0) space += RING_WORDS;

    for (int32_t i = 0; i < space; i++) {
        int16_t v = synth_sample();
        uint32_t w = (uint32_t)(uint16_t)v << 16;   // 左对齐高 16 位
        s_ring[s_wp] = w; s_wp = (s_wp + 1u) & RING_MASK;
        s_ring[s_wp] = w; s_wp = (s_wp + 1u) & RING_MASK;
    }
}

// ---------------- 初始化 ----------------
void i2s_audio_init() {
    for (uint32_t i = 0; i < RING_WORDS; i++) s_ring[i] = 0;

    s_sm = pio_claim_unused_sm(s_pio, true);
    s_prog_off = pio_add_program(s_pio, &i2s_program);

    pio_gpio_init(s_pio, PIN_I2S_DATA);
    pio_gpio_init(s_pio, PIN_I2S_BCLK);
    pio_gpio_init(s_pio, PIN_I2S_LRCLK);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm, PIN_I2S_BCLK, 2, true);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm, PIN_I2S_DATA, 1, true);

    pio_sm_config c = i2s_program_get_default_config(s_prog_off);
    sm_config_set_out_pins(&c, PIN_I2S_DATA, 1);
    sm_config_set_sideset_pins(&c, PIN_I2S_BCLK);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

    // 128 PIO 周期/帧：div256 = sysclk*2/sample_rate
    uint32_t div256 = clock_get_hz(clk_sys) * 2u / SAMPLE_RATE;
    sm_config_set_clkdiv_int_frac(&c, div256 >> 8, div256 & 0xffu);

    pio_sm_init(s_pio, s_sm, s_prog_off, &c);
    pio_sm_set_pins(s_pio, s_sm, 0);
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_prog_off + i2s_offset_entry_point));

    // 屏幕固定占用 DMA 通道 0，但它是直接配置、没有走 claim 注册。
    // 先把通道 0 标记为已占用，避免 dma_claim_unused_channel 误拿到通道 0
    // 并重新配置，破坏屏幕 DMA（那会导致黑屏）。
    if (!dma_channel_is_claimed(0)) dma_channel_claim(0);
    s_dma_ch = dma_claim_unused_channel(true);   // 现在从通道 1 开始分配
    dma_channel_config dc = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(s_pio, s_sm, true));
    channel_config_set_ring(&dc, false, RING_ADDR_BITS);
    dma_channel_configure(s_dma_ch, &dc,
                          &s_pio->txf[s_sm],
                          s_ring, 0, false);

    s_wp = 0;
    syn_active = false;

    // 启动 DMA（极大计数，读 ring 永不停止），再启动状态机，然后填充环内静音
    dma_channel_transfer_from_buffer_now(s_dma_ch, s_ring, 0xFFFFFFFFu);
    pio_sm_set_enabled(s_pio, s_sm, true);
    i2s_pump();
}
