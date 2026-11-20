// 方块纹理：启动时程序化生成到 SRAM 的小纹理（16x16），供 shader_id=100 采样。
// 纹理在运行时生成、只占少量 SRAM，不占 flash；草地顶面纹理含花朵图案。
#ifndef BLOCK_TEXTURES_H
#define BLOCK_TEXTURES_H

#include <cstdint>
#include "../engine/render_globals.h"

// 纹理边长（像素）：RP2040 主 RAM 紧张用 8x8；RP2350 用 16x16 全细节
#if defined(PICO_RP2040) && PICO_RP2040
#define TEX_SIZE 8u
#else
#define TEX_SIZE 16u
#endif

// 纹理槽位（对应不同方块/面）
enum BlockTex : uint8_t {
    TEX_GRASS_TOP = 0,          // 草地顶面（带花朵）
    TEX_GRASS_SIDE,             // 草地侧面（草边+泥土）
    TEX_DIRT,
    TEX_STONE,
    TEX_LOG,
    TEX_LEAVES,
    TEX_SAND,
    TEX_PLANK,
    TEX_WATER,
    TEX_COBBLE,
    TEX_BRICK,
    TEX_GLASS,
    TEX_BEDROCK,
    TEX_SNOW,
    TEX_GOLD,
    TEX_COUNT,
};

// 启动时生成全部纹理，并把 chunk_texture_list 指向它们
void init_block_textures();

// 某方块、某面（法线 0..5 对应 FACES 顺序）的纹理槽位
uint8_t block_texture_id(uint8_t block, int face);

#endif
