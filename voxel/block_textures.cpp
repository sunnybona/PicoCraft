#include "block_textures.h"
#include "voxel_world.h"

// 纹理 SRAM 缓冲：TEX_COUNT 张 TEX_SIZE x TEX_SIZE
static color_t s_tex[TEX_COUNT][TEX_SIZE * TEX_SIZE];

// 纹理表（image 指向 SRAM 缓冲），render_rasterize 通过 chunk_data.h 的 extern 访问
// 定义处加 extern：C++ 命名空间作用域 const 默认内部链接，需显式外部链接
extern const struct texture chunk_texture_list[TEX_COUNT] = {
#define TENTRY(I) { TEX_SIZE, 0, 0, s_tex[I] }
    TENTRY(TEX_GRASS_TOP), TENTRY(TEX_GRASS_TOP_PLAIN), TENTRY(TEX_GRASS_SIDE), TENTRY(TEX_DIRT),
    TENTRY(TEX_STONE), TENTRY(TEX_LOG), TENTRY(TEX_LEAVES), TENTRY(TEX_SAND),
    TENTRY(TEX_PLANK), TENTRY(TEX_WATER), TENTRY(TEX_COBBLE), TENTRY(TEX_BRICK),
    TENTRY(TEX_GLASS), TENTRY(TEX_BEDROCK), TENTRY(TEX_SNOW), TENTRY(TEX_GOLD),
#undef TENTRY
};

// ---------------- 基础工具 ----------------
static inline color_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (color_t)(((g & 15) << 12) | ((b & 15) << 8) | (r & 15));
}

struct Rng {
    uint32_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    int range(int n) { return (int)(next() % (uint32_t)n); }
};

static inline void putpx(color_t *img, int x, int y, color_t c) {
    img[y * TEX_SIZE + x] = c;
}

static inline int clamp15(int v) {
    if (v < 0) return 0; if (v > 15) return 15; return v;
}

// 基础色 + 每像素随机抖动（jitter 为 ±幅度）
static void noisy_fill(color_t *img, uint8_t r, uint8_t g, uint8_t b,
                       int jitter, Rng &rng) {
    for (int y = 0; y < (int)TEX_SIZE; y++) {
        for (int x = 0; x < (int)TEX_SIZE; x++) {
            int d = rng.range(2 * jitter + 1) - jitter;
            putpx(img, x, y, rgb((uint8_t)clamp15(r + d),
                                 (uint8_t)clamp15(g + d),
                                 (uint8_t)clamp15(b + d)));
        }
    }
}

// 在 (cx,cy) 画一朵小花（十字花瓣 + 亮花心）
static void draw_flower(color_t *img, int cx, int cy, color_t petal) {
    color_t center = rgb(15, 13, 4);
    putpx(img, cx, cy, center);
    if (cx > 0)          putpx(img, cx - 1, cy, petal);
    if (cx < TEX_SIZE-1) putpx(img, cx + 1, cy, petal);
    if (cy > 0)          putpx(img, cx, cy - 1, petal);
    if (cy < TEX_SIZE-1) putpx(img, cx, cy + 1, petal);
}

// ---------------- 各纹理生成 ----------------
static void gen_grass_top(color_t *img, Rng &rng) {
    noisy_fill(img, 4, 12, 4, 2, rng);
    // 花朵数量随纹理尺寸：16x16 画 4 朵，8x8 画 1 朵
    const color_t petals[3] = { rgb(15, 4, 5), rgb(15, 13, 4), rgb(14, 14, 14) };
    int nflowers = (TEX_SIZE >= 16) ? 4 : 1;
    int span = (int)TEX_SIZE - 4;
    for (int f = 0; f < nflowers; f++) {
        int fx = 2 + rng.range(span);
        int fy = 2 + rng.range(span);
        draw_flower(img, fx, fy, petals[f % 3]);
    }
}

static void gen_grass_side(color_t *img, Rng &rng) {
    int grass_h = (TEX_SIZE >= 16) ? 4 : 2;
    for (int y = 0; y < (int)TEX_SIZE; y++) {
        bool grass = y < grass_h;
        int br = grass ? 4 : 9, bg = grass ? 12 : 6, bb = grass ? 4 : 3;
        for (int x = 0; x < (int)TEX_SIZE; x++) {
            int d = rng.range(3) - 1;
            putpx(img, x, y, rgb((uint8_t)clamp15(br + d),
                                 (uint8_t)clamp15(bg + d),
                                 (uint8_t)clamp15(bb + d)));
        }
    }
}

static void gen_log(color_t *img, Rng &rng) {
    // 纵向树皮：每列整体深浅，再加点状抖动
    for (int x = 0; x < (int)TEX_SIZE; x++) {
        int col = rng.range(3) - 1;
        for (int y = 0; y < (int)TEX_SIZE; y++) {
            int d = col + rng.range(2);
            putpx(img, x, y, rgb((uint8_t)clamp15(10 + d),
                                 (uint8_t)clamp15(7 + d),
                                 (uint8_t)clamp15(4 + d)));
        }
    }
}

static void gen_leaves(color_t *img, Rng &rng) {
    for (int y = 0; y < (int)TEX_SIZE; y++) {
        for (int x = 0; x < (int)TEX_SIZE; x++) {
            int d = rng.range(5) - 2;
            if (rng.range(8) == 0) d -= 3;   // 深色"孔隙"
            putpx(img, x, y, rgb((uint8_t)clamp15(3 + d),
                                 (uint8_t)clamp15(9 + d),
                                 (uint8_t)clamp15(3 + d)));
        }
    }
}

static void gen_plank(color_t *img, Rng &rng) {
    int seam = (TEX_SIZE >= 16) ? 4 : 2;
    for (int y = 0; y < (int)TEX_SIZE; y++) {
        for (int x = 0; x < (int)TEX_SIZE; x++) {
            int d = rng.range(2);
            int rr = 12 + d, gg = 9 + d, bb = 5 + d;
            if (y % seam == seam - 1) { rr -= 3; gg -= 3; bb -= 2; }
            putpx(img, x, y, rgb((uint8_t)clamp15(rr),
                                 (uint8_t)clamp15(gg),
                                 (uint8_t)clamp15(bb)));
        }
    }
}

static void gen_cobble(color_t *img, Rng &rng) {
    // 鹅卵石：2x2 块整体深浅
    int half = (int)TEX_SIZE / 2;
    for (int by = 0; by < half; by++) {
        for (int bx = 0; bx < half; bx++) {
            int d = rng.range(5) - 2;
            for (int y = by * 2; y < by * 2 + 2; y++) {
                for (int x = bx * 2; x < bx * 2 + 2; x++) {
                    putpx(img, x, y, rgb((uint8_t)clamp15(7 + d),
                                         (uint8_t)clamp15(7 + d),
                                         (uint8_t)clamp15(8 + d)));
                }
            }
        }
    }
}

static void gen_brick(color_t *img, Rng &rng) {
    int row = (TEX_SIZE >= 16) ? 4 : 2;
    int col = (TEX_SIZE >= 16) ? 8 : 4;
    for (int y = 0; y < (int)TEX_SIZE; y++) {
        int off = ((y / row) & 1) * col;
        for (int x = 0; x < (int)TEX_SIZE; x++) {
            int xx = (x + off) % (int)TEX_SIZE;
            bool mortar = (y % row == row - 1) || (xx % col == col - 1);
            int d = rng.range(2);
            int rr, gg, bb;
            if (mortar) { rr = 8; gg = 7; bb = 7; }
            else { rr = 12 + d; gg = 5 + d; bb = 4 + d; }
            putpx(img, x, y, rgb((uint8_t)clamp15(rr),
                                 (uint8_t)clamp15(gg),
                                 (uint8_t)clamp15(bb)));
        }
    }
}

static void gen_glass(color_t *img, Rng &rng) {
    (void)rng;
    for (int y = 0; y < (int)TEX_SIZE; y++) {
        for (int x = 0; x < (int)TEX_SIZE; x++) {
            bool edge = (x == 0 || y == 0 || x == TEX_SIZE-1 || y == TEX_SIZE-1);
            bool streak = (x == y);
            uint8_t v = edge ? 15 : (streak ? 14 : 12);
            putpx(img, x, y, rgb(v, (uint8_t)(v + 2 > 15 ? 15 : v + 2), 15));
        }
    }
}

void init_block_textures() {
    Rng rng;
    rng.s = 0x12345678u;

    gen_grass_top(s_tex[TEX_GRASS_TOP], rng);
    // 无花草顶：同样的草色噪点底，不画花朵
    noisy_fill(s_tex[TEX_GRASS_TOP_PLAIN], 4, 12, 4, 2, rng);
    gen_grass_side(s_tex[TEX_GRASS_SIDE], rng);

    noisy_fill(s_tex[TEX_DIRT], 9, 6, 3, 2, rng);
    noisy_fill(s_tex[TEX_STONE], 9, 9, 10, 2, rng);
    gen_log(s_tex[TEX_LOG], rng);
    gen_leaves(s_tex[TEX_LEAVES], rng);
    noisy_fill(s_tex[TEX_SAND], 13, 13, 9, 1, rng);
    gen_plank(s_tex[TEX_PLANK], rng);
    noisy_fill(s_tex[TEX_WATER], 3, 6, 13, 1, rng);
    gen_cobble(s_tex[TEX_COBBLE], rng);
    gen_brick(s_tex[TEX_BRICK], rng);
    gen_glass(s_tex[TEX_GLASS], rng);
    noisy_fill(s_tex[TEX_BEDROCK], 3, 3, 4, 3, rng);
    noisy_fill(s_tex[TEX_SNOW], 14, 14, 15, 1, rng);
    noisy_fill(s_tex[TEX_GOLD], 15, 12, 3, 2, rng);
}

// 花朵成片网格边长（格）：每 FLOWER_GRID x FLOWER_GRID 区域统一带花/无花
#define FLOWER_GRID 4
// 带花区域占比（%）：仅约 40% 区域有花，其余整片无花
#define FLOWER_AREA_PCT 40

// 向下取整除法（b>0），保证负坐标下区域划分一致
static int floor_div1(int a, int b) {
    int q = a / b;
    int r = a % b;
    if (r != 0 && r < 0) q--;
    return q;
}

static uint32_t cell_hash2(int x, int z) {
    uint32_t h = (uint32_t)(x * 374761393) + (uint32_t)(z * 668265263) + 0x9e3779b9u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// 草顶面纹理：按世界坐标成片决定带花/无花
uint8_t grass_top_texture_id(int wx, int wz) {
    int gx = floor_div1(wx, FLOWER_GRID);
    int gz = floor_div1(wz, FLOWER_GRID);
    return (cell_hash2(gx, gz) % 100u < FLOWER_AREA_PCT)
           ? TEX_GRASS_TOP : TEX_GRASS_TOP_PLAIN;
}

// 面索引 0..5 对应 voxel_mesh FACES：top,bottom,+Z,-Z,+X,-X
uint8_t block_texture_id(uint8_t block, int face) {
    switch (block) {
    case BLK_GRASS:  return (face == 0) ? TEX_GRASS_TOP : TEX_GRASS_SIDE;
    case BLK_DIRT:   return TEX_DIRT;
    case BLK_STONE:  return TEX_STONE;
    case BLK_LOG:    return TEX_LOG;
    case BLK_LEAVES: return TEX_LEAVES;
    case BLK_SAND:   return TEX_SAND;
    case BLK_PLANK:  return TEX_PLANK;
    case BLK_WATER:  return TEX_WATER;
    case BLK_COBBLE: return TEX_COBBLE;
    case BLK_BRICK:  return TEX_BRICK;
    case BLK_GLASS:  return TEX_GLASS;
    case BLK_BEDROCK:return TEX_BEDROCK;
    case BLK_SNOW:   return TEX_SNOW;
    case BLK_GOLD:   return TEX_GOLD;
    default:         return TEX_STONE;
    }
}
