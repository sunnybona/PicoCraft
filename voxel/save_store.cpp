#include "save_store.h"
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"              // save_and_disable_interrupts
#include "hardware/regs/addressmap.h"   // XIP_BASE

// 单次编程缓冲：header(16) + 最多 256 条记录(4096) = 4112，向上取整到 256 页 = 4352
#define SAVE_PROGRAM_BYTES 4352u
static uint8_t program_buf[SAVE_PROGRAM_BYTES] __attribute__((aligned(256)));

uint32_t SaveStore::save_offset() {
    return (uint32_t)(PICO_FLASH_SIZE_BYTES - SAVE_SIZE);
}

bool SaveStore::load(VoxelWorld &world) {
    const uint8_t *base = (const uint8_t *)(XIP_BASE + save_offset());
    SaveHeader hdr;
    memcpy(&hdr, base, sizeof(hdr));

    if (hdr.magic != SAVE_MAGIC || hdr.version != SAVE_VERSION) {
        return false;
    }

    uint32_t count = hdr.count;
    if (count > MAX_EDITS) count = MAX_EDITS;

    world.edit_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        SaveRecord rec;
        memcpy(&rec, base + sizeof(SaveHeader) + i * sizeof(SaveRecord), sizeof(rec));
        if (rec.y < 0 || rec.y >= CHUNK_H) continue;
        world.edits[world.edit_count].x = rec.x;
        world.edits[world.edit_count].y = rec.y;
        world.edits[world.edit_count].z = rec.z;
        world.edits[world.edit_count].block = rec.block;
        world.edit_count++;
    }
    return true;
}

bool SaveStore::save(const VoxelWorld &world) {
    memset(program_buf, 0xFF, sizeof(program_buf));

    SaveHeader hdr;
    hdr.magic = SAVE_MAGIC;
    hdr.version = SAVE_VERSION;
    hdr.seed = world.seed;
    hdr.count = (uint32_t)world.edit_count;
    memcpy(program_buf, &hdr, sizeof(hdr));

    for (int i = 0; i < world.edit_count; i++) {
        SaveRecord rec;
        rec.x = world.edits[i].x;
        rec.y = world.edits[i].y;
        rec.z = world.edits[i].z;
        rec.block = world.edits[i].block;
        rec.pad[0] = rec.pad[1] = rec.pad[2] = 0xFF;
        memcpy(program_buf + sizeof(hdr) + i * sizeof(rec), &rec, sizeof(rec));
    }

    uint32_t off = save_offset();

    // flash_range_erase/program 内部会退出 XIP（此时 flash 不可取指），
    // 但它们不关闭中断。音频 1ms 定时器、DMA 等中断处理都在 flash，
    // 若在擦除窗口内触发会取指失败而 hardfault，故全程禁用中断。
    uint32_t saved_irq = save_and_disable_interrupts();
    flash_range_erase(off, SAVE_SIZE);
    flash_range_program(off, program_buf, SAVE_PROGRAM_BYTES);
    restore_interrupts(saved_irq);
    return true;
}
