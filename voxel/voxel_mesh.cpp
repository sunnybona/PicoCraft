#include "voxel_mesh.h"
#include "../engine/render_globals.h"
#include "../engine/render_math.h"
#include "block_textures.h"

// 6 个面：带符号法线 + 4 个局部角（0/1）。
// 角点绕序已验证：从外侧看为 CCW，法线指向外侧，能正确通过背面剔除。
namespace {

struct FaceDef {
    int normal[3];
    int corner[4][3];
};

const FaceDef FACES[6] = {
    // TOP (+Y)
    { {0, 1, 0}, { {0,1,0},{0,1,1},{1,1,1},{1,1,0} } },
    // BOTTOM (-Y)
    { {0,-1, 0}, { {0,0,0},{1,0,0},{1,0,1},{0,0,1} } },
    // +Z
    { {0, 0, 1}, { {0,0,1},{1,0,1},{1,1,1},{0,1,1} } },
    // -Z
    { {0, 0,-1}, { {0,0,0},{0,1,0},{1,1,0},{1,0,0} } },
    // +X
    { {1, 0, 0}, { {1,0,1},{1,0,0},{1,1,0},{1,1,1} } },
    // -X
    { {-1, 0, 0}, { {0,0,1},{0,1,1},{0,1,0},{0,0,0} } },
};

// 方块基础 RGB（0..15），HUD 用
struct BaseRGB { uint8_t r, g, b; };
const BaseRGB BLOCK_COLOR[BLOCK_TYPE_COUNT] = {
    {0, 0, 0},      // AIR
    {4, 12, 4},     // GRASS
    {9, 6, 3},      // DIRT
    {9, 9, 10},     // STONE
    {10, 7, 4},     // LOG
    {3, 9, 3},      // LEAVES
    {13, 13, 9},    // SAND
    {12, 9, 5},     // PLANK
    {3, 6, 13},     // WATER
    {7, 7, 8},     // COBBLE
    {12, 5, 4},     // BRICK
    {12, 14, 15},   // GLASS
    {3, 3, 4},      // BEDROCK
    {14, 14, 15},   // SNOW
    {15, 12, 3},    // GOLD
};

} // namespace

void block_rgb(uint8_t type, uint8_t &r, uint8_t &g, uint8_t &b) {
    if (type >= BLOCK_TYPE_COUNT) type = 0;
    const BaseRGB &c = BLOCK_COLOR[type];
    r = c.r; g = c.g; b = c.b;
}

// 纹理面：shader 100 + 该面的 UV 映射（文件内函数）
static void emit_textured_face(int bx, int by, int bz,
                               const FaceDef &fd, int face_idx,
                               uint8_t block) {
    // 法线轴与两个切向轴
    int naxis = 0;
    for (int a = 0; a < 3; a++) if (fd.normal[a] != 0) naxis = a;
    int t0 = (naxis + 1) % 3, t1 = (naxis + 2) % 3;

    int32_t p[4][3];
    uint8_t uv[4][2];
    for (int i = 0; i < 4; i++) {
        p[i][0] = float_to_int((float)(bx + fd.corner[i][0]));
        p[i][1] = float_to_int((float)(by + fd.corner[i][1]));
        p[i][2] = float_to_int((float)(bz + fd.corner[i][2]));
        uv[i][0] = (uint8_t)(fd.corner[i][t0] * TEX_SIZE);
        uv[i][1] = (uint8_t)(fd.corner[i][t1] * TEX_SIZE);
    }

    triangle_32 t;
    t.shader_id = 100;
    // 草顶面（face_idx==0）按世界坐标成片决定带花/无花，其余面用常规映射
    t.texture_id = (block == BLK_GRASS && face_idx == 0)
                   ? grass_top_texture_id(bx, bz)
                   : block_texture_id(block, face_idx);

    const int idx[2][3] = { {0,1,2}, {0,2,3} };
    for (int ti = 0; ti < 2; ti++) {
        vertex_32 *v[3] = {&t.vertex1,&t.vertex2,&t.vertex3};
        for (int k = 0; k < 3; k++) {
            int ci = idx[ti][k];
            v[k]->x = p[ci][0];
            v[k]->y = p[ci][1];
            v[k]->z = p[ci][2];
        }
        t.vertex_parameter1.uv[0] = uv[idx[ti][0]][0];
        t.vertex_parameter1.uv[1] = uv[idx[ti][0]][1];
        t.vertex_parameter2.uv[0] = uv[idx[ti][1]][0];
        t.vertex_parameter2.uv[1] = uv[idx[ti][1]][1];
        t.vertex_parameter3.uv[0] = uv[idx[ti][2]][0];
        t.vertex_parameter3.uv[1] = uv[idx[ti][2]][1];
        render_triangle(t);
    }
}

void VoxelMesh::build(const VoxelWorld &world, float cam_x, float cam_z, int radius) {
    int cx = (int)cam_x;
    int cz = (int)cam_z;

    for (int dx = -radius; dx <= radius; dx++) {
        for (int dz = -radius; dz <= radius; dz++) {
            int bx = cx + dx;
            int bz = cz + dz;
            if (dx * dx + dz * dz > radius * radius) continue;   // 圆形视距

            for (int by = 0; by < CHUNK_H; by++) {
                int b = world.get_block(bx, by, bz);
                if (b == BLK_AIR) continue;

                for (int f = 0; f < 6; f++) {
                    const FaceDef &fd = FACES[f];
                    int neighbor = world.get_block(bx + fd.normal[0],
                                                   by + fd.normal[1],
                                                   bz + fd.normal[2]);
                    if (neighbor != BLK_AIR) continue;

                    emit_textured_face(bx, by, bz, fd, f, (uint8_t)b);

                    if (number_triangles >= MAX_RENDER_TRIANGLES - 2) return;
                }
            }
        }
    }
}
