#include "entities.h"
#include "voxel_world.h"
#include "../engine/render_globals.h"
#include "../engine/render_math.h"
#include <math.h>

static int floor_div(int a, int b) {
    int q = a / b;
    int r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}

// 确定性整数哈希（实体锚点用）
static uint32_t ent_hash(int x, int z, uint32_t seed) {
    uint32_t h = seed ^ 0x454e544du;   // "ENTM"
    h = h * 374761393u + (uint32_t)(x * 668265263);
    h = h * 374761393u + (uint32_t)(z * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// 出生点强制布局：玩家 (0,0) 周围一圈，四种动物 + 一个 NPC，初始即可见。
// 玩家初始看向 -Z，NPC 放在正前方；其余分散在不同方向。
static const struct { int8_t dx, dz; uint8_t type; } spawn_layout[SPAWN_ENTITY_N] = {
    {  0, -6, ENT_NPC},
    { -9, -7, ENT_COW},
    {  9, -9, ENT_SHEEP},
    { -8,  8, ENT_CHICKEN},
    {  9,  7, ENT_DUCK},
};

// ---------------- 生命周期 ----------------
EntitySystem::EntitySystem() {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        entities[i].type = ENT_NONE;
        entities[i].home_cx = entities[i].home_cz = 0;
    }
    // 预占出生实体槽（位置在首次 update 贴地时确定）
    for (int i = 0; i < SPAWN_ENTITY_N; i++) {
        Entity &e = entities[i];
        e.type = spawn_layout[i].type;
        e.home_cx = 100;          // 特殊标记：出生实体，不参与 chunk 匹配
        e.home_cz = 100;
        e.seq = (uint8_t)i;
        e.x = (float)spawn_layout[i].dx;
        e.z = (float)spawn_layout[i].dz;
        e.yaw = 0.0f;
        e.rng = (uint8_t)(0x30 + i * 17);
        e.timer = (uint8_t)(10 + i * 5);
        e.moving = 0;
        e.y = 1.0f;
    }
}

static uint8_t pick_type(uint32_t h) {
    switch ((h >> 10) % 10u) {
        case 0: case 1: case 9: return ENT_COW;
        case 2: case 3:         return ENT_SHEEP;
        case 4: case 5:         return ENT_CHICKEN;
        case 6: case 7:         return ENT_DUCK;
        default:                return ENT_NPC;
    }
}

void EntitySystem::update(const VoxelWorld &world, uint32_t seed,
                         float cam_x, float cam_z, uint32_t tick) {
    int pcx = floor_div((int)floorf(cam_x), CHUNK_S);
    int pcz = floor_div((int)floorf(cam_z), CHUNK_S);

    bool seen[MAX_ENTITIES] = {};
    // 出生实体（前 N 槽）始终保留，不参与随机 chunk 匹配与释放
    for (int i = 0; i < SPAWN_ENTITY_N; i++) seen[i] = true;

    // 扫描与 chunk 加载一致的 3x3 chunk（实体所在 chunk 一定已加载）
    for (int ddz = -1; ddz <= 1; ddz++) {
        for (int ddx = -1; ddx <= 1; ddx++) {
            int cx = pcx + ddx, cz = pcz + ddz;
            uint32_t h = ent_hash(cx, cz, seed);
            if ((h % 100u) >= 38u) continue;          // 约 38% chunk 有实体

            uint8_t type = pick_type(h);
            int lx = 4 + (int)((h >> 8)  % 9u);       // 4..12
            int lz = 4 + (int)((h >> 20) % 9u);
            int wx = cx * CHUNK_S + lx;
            int wz = cz * CHUNK_S + lz;

            // 找现有槽，否则分配空槽（出生实体前 N 槽保留，只在其后查找）
            int idx = -1;
            for (int i = SPAWN_ENTITY_N; i < MAX_ENTITIES; i++) {
                if (entities[i].type != ENT_NONE &&
                    entities[i].home_cx == cx &&
                    entities[i].home_cz == cz) { idx = i; break; }
            }
            if (idx < 0) {
                for (int i = SPAWN_ENTITY_N; i < MAX_ENTITIES; i++) {
                    if (entities[i].type == ENT_NONE) { idx = i; break; }
                }
            }
            if (idx < 0) continue;

            Entity &e = entities[idx];
            if (e.type == ENT_NONE) {
                e.type = type;
                e.home_cx = (int16_t)cx;
                e.home_cz = (int16_t)cz;
                e.seq = 0;
                e.x = (float)wx;
                e.z = (float)wz;
                e.yaw = 0.0f;
                e.rng = (uint8_t)(h | 1u);
                e.timer = 10u;
                e.moving = 0;
                int gy = world.surface_y(wx, wz);
                e.y = (gy >= 0 ? (float)(gy + 1) : 1.0f);
            }
            seen[idx] = true;
        }
    }

    // 离开范围的实体释放
    for (int i = 0; i < MAX_ENTITIES; i++) {
        if (entities[i].type != ENT_NONE && !seen[i]) entities[i].type = ENT_NONE;
    }

    // AI 推进 + 贴地
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity &e = entities[i];
        if (e.type == ENT_NONE) continue;

        e.timer--;
        if (e.timer == 0) {
            e.rng = (uint8_t)(e.rng * 73u + 97u);
            uint8_t r = e.rng;
            if (r % 3u == 0u) {
                e.moving = 0;
                e.timer = 20u + (r % 30u);
            } else {
                e.moving = 1;
                e.yaw = (float)(r % 8u) * (PI / 4.0f);
                e.timer = 8u + (r % 16u);
            }
        }

        if (e.moving) {
            float sp = (e.type == ENT_COW || e.type == ENT_SHEEP) ? 0.018f : 0.024f;
            e.x += -sinf(e.yaw) * sp;
            e.z += -cosf(e.yaw) * sp;
        }

        int gy = world.surface_y((int)e.x, (int)e.z);
        if (gy >= 0) e.y = (float)(gy + 1);
    }
}

// ---------------- 渲染 ----------------
static inline color_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (color_t)(((g & 15) << 12) | ((b & 15) << 8) | (r & 15));
}

// 对 GBAR 颜色按系数缩放，制造面方向明暗
static color_t shade(color_t c, float f) {
    int r = (int)((c & 15) * f);
    int b = (int)(((c >> 8) & 15) * f);
    int g = (int)(((c >> 12) & 15) * f);
    if (r > 15) r = 15; if (g > 15) g = 15; if (b > 15) b = 15;
    return rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// 8 角点 + 6 面（绕序与 voxel_mesh 一致，外视 CCW）
static void emit_box(float x0, float y0, float z0,
                     float x1, float y1, float z1, color_t col) {
    const float p[8][3] = {
        {x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1},
        {x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1},
    };
    // 面：角索引 + 亮度
    static const struct { uint8_t c[4]; float br; } faces[6] = {
        {{4,7,6,5}, 1.00f},   // top
        {{0,1,2,3}, 0.50f},   // bottom
        {{3,2,6,7}, 0.80f},   // +Z
        {{0,4,5,1}, 0.80f},   // -Z
        {{2,1,5,6}, 0.65f},   // +X
        {{3,7,4,0}, 0.65f},   // -X
    };

    triangle_32 t;
    t.shader_id = 1;
    t.texture_id = 0;

    for (int f = 0; f < 6; f++) {
        if (number_triangles >= MAX_RENDER_TRIANGLES - 2) return;
        color_t fc = shade(col, faces[f].br);
        const int tri[2][3] = {{0,1,2},{0,2,3}};
        for (int ti = 0; ti < 2; ti++) {
            vertex_32 *v[3] = {&t.vertex1,&t.vertex2,&t.vertex3};
            for (int k = 0; k < 3; k++) {
                const float *q = p[faces[f].c[tri[ti][k]]];
                v[k]->x = float_to_int(q[0]);
                v[k]->y = float_to_int(q[1]);
                v[k]->z = float_to_int(q[2]);
            }
            t.vertex_parameter1.color = fc;
            t.vertex_parameter2.color = fc;
            t.vertex_parameter3.color = fc;
            render_triangle(t);
        }
    }
}

void EntitySystem::render(float cam_x, float cam_z) {
    for (int i = 0; i < MAX_ENTITIES; i++) {
        const Entity &e = entities[i];
        if (e.type == ENT_NONE) continue;

        float dx = e.x - cam_x, dz = e.z - cam_z;
        if (dx * dx + dz * dz > (float)(ENTITY_DRAW_RADIUS * ENTITY_DRAW_RADIUS))
            continue;

        const float x = e.x, y = e.y, z = e.z;

        switch (e.type) {
        case ENT_COW: {
            color_t body = rgb(14,14,13), dark = rgb(8,6,5);
            const float lx[2]={-0.28f,0.22f}, lz[2]={-0.15f,0.15f};
            for (int a=0;a<2;a++) for (int b=0;b<2;b++)
                emit_box(x+lx[a], y+0.05f, z+lz[b],
                         x+lx[a]+0.10f, y+0.50f, z+lz[b]+0.10f, dark);
            emit_box(x-0.35f,y+0.50f,z-0.20f, x+0.35f,y+0.82f,z+0.20f, body);
            emit_box(x+0.32f,y+0.62f,z-0.14f, x+0.52f,y+0.90f,z+0.14f, dark);
        } break;

        case ENT_SHEEP: {
            color_t wool = rgb(14,14,14), blk = rgb(7,7,7);
            const float lx[2]={-0.24f,0.20f}, lz[2]={-0.13f,0.13f};
            for (int a=0;a<2;a++) for (int b=0;b<2;b++)
                emit_box(x+lx[a], y+0.05f, z+lz[b],
                         x+lx[a]+0.07f, y+0.45f, z+lz[b]+0.07f, blk);
            emit_box(x-0.30f,y+0.45f,z-0.18f, x+0.30f,y+0.75f,z+0.18f, wool);
            emit_box(x+0.28f,y+0.55f,z-0.12f, x+0.44f,y+0.75f,z+0.12f, blk);
        } break;

        case ENT_CHICKEN: {
            color_t wh = rgb(14,14,13), org = rgb(15,12,3);
            emit_box(x-0.05f,y+0.02f,z-0.02f, x-0.01f,y+0.20f,z+0.02f, org);
            emit_box(x+0.01f,y+0.02f,z-0.02f, x+0.05f,y+0.20f,z+0.02f, org);
            emit_box(x-0.14f,y+0.20f,z-0.12f, x+0.14f,y+0.40f,z+0.12f, wh);
            emit_box(x+0.08f,y+0.40f,z-0.07f, x+0.20f,y+0.52f,z+0.07f, wh);
            emit_box(x+0.20f,y+0.43f,z-0.03f, x+0.26f,y+0.47f,z+0.03f, org);
        } break;

        case ENT_DUCK: {
            color_t gry = rgb(12,13,13), grn = rgb(4,10,4), org = rgb(15,12,3);
            emit_box(x-0.05f,y+0.02f,z-0.02f, x-0.01f,y+0.15f,z+0.02f, org);
            emit_box(x+0.01f,y+0.02f,z-0.02f, x+0.05f,y+0.15f,z+0.02f, org);
            emit_box(x-0.16f,y+0.15f,z-0.13f, x+0.16f,y+0.36f,z+0.13f, gry);
            emit_box(x+0.08f,y+0.36f,z-0.08f, x+0.20f,y+0.50f,z+0.08f, grn);
            emit_box(x+0.20f,y+0.40f,z-0.04f, x+0.27f,y+0.44f,z+0.04f, org);
        } break;

        case ENT_NPC: {
            color_t robe = rgb(11,7,4), pants = rgb(4,4,9), skin = rgb(14,11,9);
            emit_box(x-0.12f,y+0.05f,z-0.10f, x-0.02f,y+0.65f,z+0.10f, pants);
            emit_box(x+0.02f,y+0.05f,z-0.10f, x+0.12f,y+0.65f,z+0.10f, pants);
            emit_box(x-0.22f,y+0.65f,z-0.14f, x+0.22f,y+1.20f,z+0.14f, robe);
            emit_box(x-0.30f,y+0.70f,z-0.12f, x-0.22f,y+1.12f,z+0.12f, robe);
            emit_box(x+0.22f,y+0.70f,z-0.12f, x+0.30f,y+1.12f,z+0.12f, robe);
            emit_box(x-0.14f,y+1.20f,z-0.14f, x+0.14f,y+1.45f,z+0.14f, skin);
            emit_box(x+0.14f,y+1.28f,z-0.03f, x+0.18f,y+1.36f,z+0.03f, rgb(12,9,7));
        } break;

        default: break;
        }
    }
}
