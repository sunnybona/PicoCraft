// 实体系统：动物（牛/羊/鸡/鸭）与 NPC。
// 无 PSRAM：固定大小实体池、严格限数量，按玩家位置确定性生成并流式激活/失活，
// 不写入 flash。实体用轴对齐的小立方体组合渲染，复用 Pico3D 三角形管线。
#ifndef ENTITIES_H
#define ENTITIES_H

#include <cstdint>

class VoxelWorld;

enum EntityType : uint8_t {
    ENT_NONE = 0,
    ENT_COW,
    ENT_SHEEP,
    ENT_CHICKEN,
    ENT_DUCK,
    ENT_NPC,
};

#define MAX_ENTITIES 12          // 同时驻留的实体上限
#define ENTITY_ACTIVE_RADIUS 24  // 距玩家（方块）内激活/更新
#define ENTITY_DRAW_RADIUS  14   // 距玩家（方块）内渲染
#define SPAWN_ENTITY_N 5         // 出生点强制保留的实体数量（占前 N 槽）

struct Entity {
    uint8_t  type;          // ENT_NONE = 空槽
    int16_t  home_cx;       // 生成所在 chunk 坐标
    int16_t  home_cz;
    uint8_t  seq;           // 同 chunk 内序号
    float    x, y, z;       // 脚部基准位置（米）
    float    yaw;
    uint8_t  rng;           // 内部确定性随机状态
    uint8_t  timer;         // AI 决策计时
    uint8_t  moving;        // 当前是否在游走
};

class EntitySystem {
public:
    Entity entities[MAX_ENTITIES];

    EntitySystem();

    // 每帧：确定性流式激活/失活 + 简单 AI 推进
    void update(const VoxelWorld &world, uint32_t seed,
                float cam_x, float cam_z, uint32_t tick);

    // 渲染距离内的实体（小立方体组合），写入三角形列表
    void render(float cam_x, float cam_z);
};

#endif
