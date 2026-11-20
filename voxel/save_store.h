// flash 持久化：把稀疏编辑写入片上 flash 末尾的保留区，重启后恢复。
// 本模块只做 flash 读写；双核的暂停/重启由调用方（main）负责。
#ifndef SAVE_STORE_H
#define SAVE_STORE_H

#include <cstdint>
#include <cstddef>
#include "voxel_world.h"

#define SAVE_MAGIC   0x5043424Cu   // "PCBL"
#define SAVE_VERSION 1u

// 保留 2 个扇区（8192）
#define SAVE_SECTORS 2u
#define SAVE_SIZE    (SAVE_SECTORS * FLASH_SECTOR_SIZE)

struct SaveHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t seed;
    uint32_t count;
};

struct SaveRecord {
    int32_t x, y, z;
    uint8_t block;
    uint8_t pad[3];
};

class SaveStore {
public:
    // 从 flash 读取编辑到 world（成功返回 true）；应在 chunk 生成前调用
    static bool load(VoxelWorld &world);

    // 把 world 的编辑写入 flash（调用前必须已暂停 Core1 且无 flash 中断）
    static bool save(const VoxelWorld &world);

private:
    static uint32_t save_offset();   // flash 保留区起始偏移
};

#endif
