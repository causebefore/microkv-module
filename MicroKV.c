/**
 * @file MicroKV.c
 * @brief 轻量级键值存储实现
 * @author liu
 * @date 2025-12-20
 * @version 2.0
 *
 * @details 本文件实现了 MicroKV 的核心功能：
 *
 * ## 核心算法
 *
 * ### 写入策略
 * - 追加写入：新数据直接追加到活跃扇区末尾
 * - 更新键：不删除旧值，直接追加新值（读取时优先返回最新值）
 * - 删除键：写入一个 val_len=0 的标记条目
 *
 * ### 垃圾回收 (GC)
 * 1. **全量GC** (MKV_Compact)
 *    - 触发：活跃扇区空间不足时
 *    - 过程：迁移所有有效条目到新扇区
 *    - 优化：使用位图 + 精确验证避免哈希冲突
 *
 * 2. **增量GC** (MKV_INCREMENTAL_GC)
 *    - 触发：活跃扇区使用率 > MKV_GC_THRESHOLD_PERCENT
 *    - 过程：每次写入后迁移 N 个条目
 *    - 优势：分摊延迟，避免长时间阻塞
 *
 * ### 缓存策略 (MKV_CACHE_ENABLE)
 * - 算法：LFU (Least Frequently Used)
 * - 命中：直接返回缓存数据，增加访问计数
 * - 未命中：从 Flash 读取并更新缓存
 * - 替换：选择访问次数最少的条目
 *
 * ### 默认值机制
 * - 存储：静态常量表，不占用 Flash
 * - 查找：先查 Flash，未找到再查默认值表
 * - 重置：支持单个键或所有键重置
 *
 * @note 移植方法：
 *       1. 修改 MicroKV_cfg.h 配置 Flash 地址和大小
 *       2. 修改 MicroKV_port.c 实现 Flash 操作函数
 *       3. 调用 mkv_init() 初始化
 */

#include "MicroKV.h"
#define LOG_TAG "MicroKV"
#include "mlog.h"

/* ==================== 内部静态实例 ==================== */
static MKV_Instance_t g_mkv = {0};

/* ==================== 辅助宏 ==================== */
#define MKV_SECTOR_ADDR(idx) (g_mkv.flash_ops.flash_base + (idx) * g_mkv.flash_ops.sector_size)
#define MKV_ALIGN(x)         (((x) + (g_mkv.flash_ops.align_size - 1)) & ~(g_mkv.flash_ops.align_size - 1))

/* ==================== CRC16 ==================== */
/**
 * @brief 计算CRC16校验值
 * @param data 数据指针
 * @param len 数据长度
 * @return uint16_t CRC16校验值
 * @note 使用MODBUS CRC16算法（多项式0xA001）
 */
static uint16_t MKV_CRC16(const uint8_t* data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
        }
    }
    return crc;
}

#if MKV_CACHE_ENABLE
/* ==================== 缓存辅助函数 ==================== */
/**
 * @brief 在缓存中查找键
 * @param key 键名
 * @return MKV_CacheEntry_t* 缓存条目指针，NULL表示未命中
 * @note 命中时会自动增加访问计数（LFU算法）
 */
static MKV_CacheEntry_t* MKV_CacheFind(const char* key)
{
    uint8_t key_len = strlen(key);

    for (uint8_t i = 0; i < MKV_CACHE_SIZE; i++)
    {
        MKV_CacheEntry_t* entry = &g_mkv.cache.entries[i];  // 获取缓存条目

        if (entry->valid && entry->key_len == key_len && memcmp(entry->key, key, key_len) == 0)  // 比较键名
        {
            // 命中，更新访问计数
            entry->access_count++;    // 增加访问次数
            g_mkv.cache.hit_count++;  // 增加命中计数
            return entry;
        }
    }
    // 未命中
    g_mkv.cache.miss_count++;  // 增加未命中计数
    return NULL;
}
/**
 * @brief 查找LFU缓存条目索引
 * @return uint8_t 缓存条目索引（优先返回空闲条目，否则返回访问次数最少的条目）
 * @note LFU = Least Frequently Used，最少使用频率算法
 */
static uint8_t MKV_CacheFindLFU(void)
{
    uint8_t  lfu_idx   = 0;
    uint32_t min_count = 0xFFFFFFFF;

    for (uint8_t i = 0; i < MKV_CACHE_SIZE; i++)
    {
        if (!g_mkv.cache.entries[i].valid)
        {
            return i;
        }

        if (g_mkv.cache.entries[i].access_count < min_count)
        {
            min_count = g_mkv.cache.entries[i].access_count;
            lfu_idx   = i;
        }
    }

    return lfu_idx;
}
/**
 * @brief 更新缓存
 * @param key 键名
 * @param value 值指针
 * @param val_len 值长度
 * @note 如果键已在缓存中则更新，否则使用LFU算法替换
 */
static void MKV_CacheUpdate(const char* key, const void* value, uint8_t val_len)
{
    MKV_CacheEntry_t* entry   = NULL;
    uint8_t           key_len = strlen(key);

    for (uint8_t i = 0; i < MKV_CACHE_SIZE; i++)
    {
        if (g_mkv.cache.entries[i].valid && g_mkv.cache.entries[i].key_len == key_len &&
            memcmp(g_mkv.cache.entries[i].key, key, key_len) == 0)
        {
            entry = &g_mkv.cache.entries[i];
            break;
        }
    }

    if (!entry)
    {
        uint8_t idx    = MKV_CacheFindLFU();
        entry          = &g_mkv.cache.entries[idx];
        entry->key_len = key_len;
        memcpy(entry->key, key, key_len);
        entry->access_count = 1;
    }

    entry->val_len = val_len;
    memcpy(entry->value, value, val_len);
    entry->valid = 1;
}

/**
 * @brief 从缓存中移除键
 * @param key 键名
 */
static void MKV_CacheRemove(const char* key)
{
    uint8_t key_len = strlen(key);

    for (uint8_t i = 0; i < MKV_CACHE_SIZE; i++)
    {
        if (g_mkv.cache.entries[i].valid && g_mkv.cache.entries[i].key_len == key_len &&
            memcmp(g_mkv.cache.entries[i].key, key, key_len) == 0)
        {
            g_mkv.cache.entries[i].valid = 0;
            break;
        }
    }
}
#endif

/* ==================== 位图辅助函数 ==================== */
#define MKV_BITMAP_SIZE 32

/**
 * @brief 计算键的哈希值
 * @param key 键名
 * @param len 键长度
 * @return uint8_t 8位哈希值（0-255）
 * @note 使用简单的31倍乘法哈希，用于GC位图加速
 */
static uint8_t MKV_HashKey(const char* key, uint8_t len)
{
    uint16_t hash = 0;
    for (uint8_t i = 0; i < len; i++)
    {
        hash = hash * 31 + key[i];
    }
    return (uint8_t) (hash & 0xFF);
}

/**
 * @brief 设置位图中的指定位
 * @param bitmap 位图数组
 * @param idx 位索引（0-255）
 */
static inline void MKV_BitmapSet(uint8_t* bitmap, uint8_t idx)
{
    bitmap[idx >> 3] |= (1 << (idx & 7));
}

/**
 * @brief 测试位图中的指定位
 * @param bitmap 位图数组
 * @param idx 位索引（0-255）
 * @return uint8_t 1=已设置，0=未设置
 */
static inline uint8_t MKV_BitmapTest(const uint8_t* bitmap, uint8_t idx)
{
    return (bitmap[idx >> 3] >> (idx & 7)) & 1;
}

/* ==================== 内部函数 ==================== */

/**
 * @brief 读取扇区头
 * @param idx 扇区索引
 * @param hdr 输出：扇区头结构体
 * @return int 0=成功，-1=失败
 */
static int MKV_ReadSectorHeader(uint8_t idx, MKV_SectorHeader_t* hdr)
{
    return g_mkv.flash_ops.read_func(MKV_SECTOR_ADDR(idx), (uint8_t*) hdr, sizeof(MKV_SectorHeader_t));
}

/**
 * @brief 检查扇区是否有效
 * @param idx 扇区索引
 * @return uint8_t 1=有效，0=无效
 * @note 通过检查魔数判断扇区是否已格式化
 */
static uint8_t MKV_IsSectorValid(uint8_t idx)
{
    MKV_SectorHeader_t hdr;
    if (MKV_ReadSectorHeader(idx, &hdr) != 0)
    {
        return 0;
    }
    return (hdr.magic == MKV_MAGIC);
}

/**
 * @brief 扫描扇区中的下一个可写位置
 * @param idx 扇区索引
 * @return uint32_t 相对于扇区起始的偏移量
 * @note 通过查找第一个擦除状态的位置确定写入偏移
 */
static uint32_t MKV_ScanWriteOffset(uint8_t idx)
{
    uint32_t sector = MKV_SECTOR_ADDR(idx);
    uint32_t offset = MKV_SECTOR_HEADER_SIZE;

    while (offset < g_mkv.flash_ops.sector_size - MKV_ENTRY_HEADER_SIZE)
    {
        MKV_Entry_t entry;
        if (g_mkv.flash_ops.read_func(sector + offset, (uint8_t*) &entry, MKV_ENTRY_HEADER_SIZE) != 0)
        {
            break;
        }

        if (entry.state == MKV_STATE_ERASED)
        {
            break;
        }

        uint32_t entry_size = MKV_ALIGN(MKV_ENTRY_HEADER_SIZE + entry.key_len + entry.val_len + MKV_ENTRY_CRC_SIZE);
        offset += entry_size;
    }

    return offset;
}

/**
 * @brief 在指定扇区中查找键
 * @param idx 扇区索引
 * @param key 键名
 * @param out_entry 输出：找到的条目（可为NULL）
 * @return uint32_t 条目的Flash地址，0表示未找到
 * @note 返回最后一次出现的位置（最新的值）
 */
static uint32_t MKV_FindKeyInSector(uint8_t idx, const char* key, MKV_Entry_t* out_entry)
{
    uint32_t sector     = MKV_SECTOR_ADDR(idx);
    uint32_t found_addr = 0;
    uint32_t offset     = MKV_SECTOR_HEADER_SIZE;
    uint8_t  key_len    = strlen(key);

    while (offset < g_mkv.flash_ops.sector_size - MKV_ENTRY_HEADER_SIZE)
    {
        MKV_Entry_t entry;
        if (g_mkv.flash_ops.read_func(sector + offset, (uint8_t*) &entry, MKV_ENTRY_HEADER_SIZE) != 0)
        {
            break;
        }

        if (entry.state == MKV_STATE_ERASED)
        {
            break;
        }

        uint32_t entry_size = MKV_ALIGN(MKV_ENTRY_HEADER_SIZE + entry.key_len + entry.val_len + MKV_ENTRY_CRC_SIZE);

        if (entry.state == MKV_STATE_VALID && entry.key_len == key_len)
        {
            char temp_key[MKV_MAX_KEY_LEN];
            g_mkv.flash_ops.read_func(sector + offset + MKV_ENTRY_HEADER_SIZE, (uint8_t*) temp_key, key_len);

            if (memcmp(temp_key, key, key_len) == 0)
            {
                found_addr = sector + offset;
                if (out_entry)
                {
                    *out_entry = entry;
                }
            }
        }

        offset += entry_size;
    }

    return found_addr;
}

/**
 * @brief 在所有扇区中查找键
 * @param key 键名
 * @param out_entry 输出：找到的条目（可为NULL）
 * @return uint32_t 条目的Flash地址，0表示未找到
 * @note 从活跃扇区开始反向搜索，优先返回最新的值
 */
static uint32_t MKV_FindKey(const char* key, MKV_Entry_t* out_entry)
{
    for (uint8_t i = 0; i < g_mkv.flash_ops.sector_count; i++)
    {
        int8_t idx = (int8_t) g_mkv.active_sector - i;
        if (idx < 0)
        {
            idx += g_mkv.flash_ops.sector_count;
        }

        if (!MKV_IsSectorValid(idx))
        {
            continue;
        }

        uint32_t addr = MKV_FindKeyInSector(idx, key, out_entry);
        if (addr != 0)
        {
            log_d("Found key='%s' in sector %u at addr 0x%08X", key, idx, addr);
            return addr;
        }
    }
    return 0;
}

/**
 * @brief 切换到下一个扇区
 * @return MKV_Error_t 错误码
 * @note 擦除下一个扇区，写入扇区头，更新活跃扇区索引和序号
 */
static MKV_Error_t MKV_SwitchToNextSector(void)
{
    uint8_t  next_idx  = (g_mkv.active_sector + 1) % g_mkv.flash_ops.sector_count;
    uint32_t next_addr = MKV_SECTOR_ADDR(next_idx);

    if (g_mkv.flash_ops.erase_func(next_addr) != 0)
    {
        return MKV_ERR_FLASH;
    }

    MKV_SectorHeader_t hdr = {.magic = MKV_MAGIC, .seq = g_mkv.sector_seq + 1};
    if (g_mkv.flash_ops.write_func(next_addr, (uint8_t*) &hdr, sizeof(hdr)) != 0)
    {
        return MKV_ERR_FLASH;
    }

    g_mkv.active_sector = next_idx;
    g_mkv.sector_seq    = hdr.seq;
    g_mkv.write_offset  = MKV_SECTOR_HEADER_SIZE;

    return MKV_OK;
}

/**
 * @brief 执行全量垃圾回收
 * @return MKV_Error_t 错误码
 * @note 将所有旧扇区的有效条目迁移到新扇区，使用位图和精确验证避免哈希冲突
 * @warning 此操作会阻塞较长时间，不适合实时系统频繁调用
 */
static MKV_Error_t MKV_Compact(void)
{
    MKV_Error_t err = MKV_SwitchToNextSector();
    if (err != MKV_OK)
    {
        return err;
    }

    uint8_t copied_bitmap[MKV_BITMAP_SIZE] = {0};

    for (uint8_t s = 1; s < g_mkv.flash_ops.sector_count; s++)
    {
        int8_t idx = (int8_t) g_mkv.active_sector - s;
        if (idx < 0)
        {
            idx += g_mkv.flash_ops.sector_count;
        }

        if (!MKV_IsSectorValid(idx))
        {
            continue;
        }

        uint32_t sector = MKV_SECTOR_ADDR(idx);
        uint32_t offset = MKV_SECTOR_HEADER_SIZE;

        while (offset < g_mkv.flash_ops.sector_size - MKV_ENTRY_HEADER_SIZE)
        {
            MKV_Entry_t entry;
            if (g_mkv.flash_ops.read_func(sector + offset, (uint8_t*) &entry, MKV_ENTRY_HEADER_SIZE) != 0)
            {
                break;
            }
            if (entry.state == MKV_STATE_ERASED)
            {
                break;
            }

            uint32_t entry_size = MKV_ALIGN(MKV_ENTRY_HEADER_SIZE + entry.key_len + entry.val_len + MKV_ENTRY_CRC_SIZE);

            if (entry.state == MKV_STATE_VALID && entry.val_len > 0)
            {
                char key[MKV_MAX_KEY_LEN] = {0};
                g_mkv.flash_ops.read_func(sector + offset + MKV_ENTRY_HEADER_SIZE, (uint8_t*) key, entry.key_len);

                uint8_t hash      = MKV_HashKey(key, entry.key_len);
                uint8_t need_copy = 0;

                if (!MKV_BitmapTest(copied_bitmap, hash))
                {
                    // 位图未标记，需要复制
                    need_copy = 1;
                }
                else
                {
                    // 位图已标记，可能是哈希冲突，精确验证
                    // 检查新扇区是否真的已有这个 key
                    MKV_Entry_t existing;
                    if (MKV_FindKeyInSector(g_mkv.active_sector, key, &existing) == 0)
                    {
                        // 新扇区没有这个 key，是哈希冲突，需要复制
                        need_copy = 1;
                        log_d("Hash collision detected for key='%s', copying anyway", key);
                    }
                }

                if (need_copy)
                {
                    if (g_mkv.write_offset + entry_size > g_mkv.flash_ops.sector_size)
                    {
                        err = MKV_SwitchToNextSector();
                        if (err != MKV_OK)
                        {
                            return err;
                        }
                        // 切换扇区后清空位图（新扇区为空）
                        memset(copied_bitmap, 0, sizeof(copied_bitmap));
                    }

                    uint8_t buf[MKV_ENTRY_HEADER_SIZE + MKV_MAX_KEY_LEN + MKV_MAX_VALUE_LEN + MKV_ENTRY_CRC_SIZE];
                    g_mkv.flash_ops.read_func(sector + offset, buf, entry_size);

                    uint32_t new_addr = MKV_SECTOR_ADDR(g_mkv.active_sector) + g_mkv.write_offset;
                    if (g_mkv.flash_ops.write_func(new_addr, buf, entry_size) != 0)
                    {
                        return MKV_ERR_FLASH;
                    }

                    g_mkv.write_offset += entry_size;
                    MKV_BitmapSet(copied_bitmap, hash);
                }
            }

            offset += entry_size;
        }
    }

    return MKV_OK;
}

#if MKV_INCREMENTAL_GC
/* ==================== 增量GC ==================== */

/**
 * @brief 检查是否需要启动增量GC
 * @return uint8_t 1=需要启动，0=不需要
 * @note 当活跃扇区使用率超过阈值时触发
 */
static uint8_t MKV_ShouldStartGC(void)
{
    if (g_mkv.gc_active)
    {
        return 0;  // 已经在进行中
    }

    // 计算使用率
    uint32_t used    = g_mkv.write_offset;
    uint32_t total   = g_mkv.flash_ops.sector_size;
    uint32_t percent = (used * 100) / total;

    return (percent >= MKV_GC_THRESHOLD_PERCENT);
}

/**
 * @brief 启动增量GC
 * @note 查找最旧的扇区作为GC源，初始化GC状态
 */
static void MKV_StartIncrementalGC(void)
{
    // 找到最旧的有效扇区作为GC源
    uint8_t  oldest_idx = 0;
    uint16_t oldest_seq = 0xFFFF;

    for (uint8_t i = 0; i < g_mkv.flash_ops.sector_count; i++)
    {
        if (i == g_mkv.active_sector)
        {
            continue;
        }

        MKV_SectorHeader_t hdr;
        if (MKV_ReadSectorHeader(i, &hdr) == 0 && hdr.magic == MKV_MAGIC)
        {
            if ((int16_t) (oldest_seq - hdr.seq) > 0)
            {
                oldest_seq = hdr.seq;
                oldest_idx = i;
            }
        }
    }

    if (oldest_seq == 0xFFFF)
    {
        return;  // 没有旧扇区
    }

    g_mkv.gc_src_sector = oldest_idx;
    g_mkv.gc_src_offset = MKV_SECTOR_HEADER_SIZE;
    g_mkv.gc_active     = 1;
    memset(g_mkv.gc_bitmap, 0, sizeof(g_mkv.gc_bitmap));

    log_i("Incremental GC started, src sector=%u", oldest_idx);
}

/**
 * @brief 执行一步增量GC（迁移一个条目）
 * @return uint8_t 1=还有更多，0=完成
 * @note 使用位图加速已迁移键的查找，避免重复迁移
 */
static uint8_t MKV_IncrementalGCStep(void)
{
    if (!g_mkv.gc_active)
    {
        return 0;
    }

    uint32_t sector = MKV_SECTOR_ADDR(g_mkv.gc_src_sector);

    // 查找下一个有效条目
    while (g_mkv.gc_src_offset < g_mkv.flash_ops.sector_size - MKV_ENTRY_HEADER_SIZE)
    {
        MKV_Entry_t entry;
        if (g_mkv.flash_ops.read_func(sector + g_mkv.gc_src_offset, (uint8_t*) &entry, MKV_ENTRY_HEADER_SIZE) != 0)
        {
            break;
        }

        // 遇到擦除状态，扫描完成
        if (entry.state == MKV_STATE_ERASED)
        {
            break;
        }

        uint32_t entry_size = MKV_ALIGN(MKV_ENTRY_HEADER_SIZE + entry.key_len + entry.val_len + MKV_ENTRY_CRC_SIZE);

        // 跳过无效/已删除的条目
        if (entry.state != MKV_STATE_VALID || entry.val_len == 0)
        {
            g_mkv.gc_src_offset += entry_size;
            continue;
        }

        // 读取键名
        char key[MKV_MAX_KEY_LEN] = {0};
        g_mkv.flash_ops.read_func(sector + g_mkv.gc_src_offset + MKV_ENTRY_HEADER_SIZE, (uint8_t*) key, entry.key_len);

        uint8_t hash = MKV_HashKey(key, entry.key_len);

        // 检查是否已迁移
        if (!MKV_BitmapTest(g_mkv.gc_bitmap, hash))
        {
            // 检查这个key是否在活跃扇区已有更新的版本
            MKV_Entry_t new_entry;
            uint32_t    new_addr = MKV_FindKeyInSector(g_mkv.active_sector, key, &new_entry);

            if (new_addr == 0)
            {
                // 活跃扇区没有这个key，需要迁移
                // 检查空间
                if (g_mkv.write_offset + entry_size <= g_mkv.flash_ops.sector_size)
                {
                    // 复制条目
                    uint8_t buf[MKV_ENTRY_HEADER_SIZE + MKV_MAX_KEY_LEN + MKV_MAX_VALUE_LEN + MKV_ENTRY_CRC_SIZE];
                    g_mkv.flash_ops.read_func(sector + g_mkv.gc_src_offset, buf, entry_size);

                    uint32_t new_write_addr = MKV_SECTOR_ADDR(g_mkv.active_sector) + g_mkv.write_offset;
                    if (g_mkv.flash_ops.write_func(new_write_addr, buf, entry_size) == 0)
                    {
                        g_mkv.write_offset += entry_size;
                        log_d("GC migrated key='%s'", key);
                    }
                }
            }

            MKV_BitmapSet(g_mkv.gc_bitmap, hash);
        }

        g_mkv.gc_src_offset += entry_size;
        return 1;  // 处理了一个条目，下次继续
    }

    // 扫描完成，可以擦除源扇区
    log_i("Incremental GC complete, erasing sector %u", g_mkv.gc_src_sector);
    g_mkv.flash_ops.erase_func(MKV_SECTOR_ADDR(g_mkv.gc_src_sector));
    g_mkv.gc_active = 0;

    return 0;
}

/**
 * @brief 执行增量GC（每次写入后调用）
 * @note 自动检查是否需要启动GC，如果GC进行中则执行配置的步数
 */
static void MKV_DoIncrementalGC(void)
{
    // 检查是否需要启动GC
    if (!g_mkv.gc_active && MKV_ShouldStartGC())
    {
        MKV_StartIncrementalGC();
    }

    // 如果GC进行中，执行几步
    if (g_mkv.gc_active)
    {
        for (uint8_t i = 0; i < MKV_GC_ENTRIES_PER_WRITE; i++)
        {
            if (!MKV_IncrementalGCStep())
            {
                break;
            }
        }
    }
}
#endif /* MKV_INCREMENTAL_GC */

/* ==================== 公共 API ==================== */

MKV_Error_t mkv_internal_init(const MKV_FlashOps_t* ops)
{
    if (!ops)
    {
        return MKV_ERR_INVALID;
    }

    if (!ops->read_func || !ops->write_func || !ops->erase_func)
    {
        return MKV_ERR_INVALID;
    }

    if (ops->sector_count < 2)
    {
        return MKV_ERR_INVALID;
    }

    if (ops->align_size != 2 && ops->align_size != 4)
    {
        return MKV_ERR_INVALID;
    }

    memset(&g_mkv, 0, sizeof(MKV_Instance_t));
    g_mkv.flash_ops = *ops;

    return MKV_OK;
}

MKV_Error_t mkv_scan(void)
{
    if (g_mkv.initialized)
    {
        return MKV_OK;
    }

    uint8_t  found      = 0;
    uint16_t max_seq    = 0;
    uint8_t  active_idx = 0;

    for (uint8_t i = 0; i < g_mkv.flash_ops.sector_count; i++)
    {
        MKV_SectorHeader_t hdr;
        if (MKV_ReadSectorHeader(i, &hdr) != 0)
        {
            continue;
        }

        if (hdr.magic == MKV_MAGIC)
        {
            if (!found || (int16_t) (hdr.seq - max_seq) > 0)
            {
                max_seq    = hdr.seq;
                active_idx = i;
                found      = 1;
            }
        }
    }

    if (!found)
    {
        return mkv_format();
    }

    g_mkv.active_sector = active_idx;
    g_mkv.sector_seq    = max_seq;
    g_mkv.write_offset  = MKV_ScanWriteOffset(active_idx);
    g_mkv.initialized   = 1;

    return MKV_OK;
}

MKV_Error_t mkv_format(void)
{
    for (uint8_t i = 0; i < g_mkv.flash_ops.sector_count; i++)
    {
        if (g_mkv.flash_ops.erase_func(MKV_SECTOR_ADDR(i)) != 0)
        {
            return MKV_ERR_FLASH;
        }
    }

    MKV_SectorHeader_t hdr = {.magic = MKV_MAGIC, .seq = 1};
    if (g_mkv.flash_ops.write_func(MKV_SECTOR_ADDR(0), (uint8_t*) &hdr, sizeof(hdr)) != 0)
    {
        return MKV_ERR_FLASH;
    }

    g_mkv.active_sector = 0;
    g_mkv.sector_seq    = 1;
    g_mkv.write_offset  = MKV_SECTOR_HEADER_SIZE;
    g_mkv.initialized   = 1;

    return MKV_OK;
}

MKV_Error_t mkv_set(const char* key, const void* value, uint8_t len)
{
    if (!g_mkv.initialized)
    {
        return MKV_ERR_INVALID;
    }

    if (!key || len > MKV_MAX_VALUE_LEN)
    {
        return MKV_ERR_INVALID;
    }

    if (len > 0 && !value)
    {
        return MKV_ERR_INVALID;
    }

    uint8_t key_len = strlen(key);
    if (key_len == 0 || key_len >= MKV_MAX_KEY_LEN)
    {
        return MKV_ERR_INVALID;
    }

    uint32_t entry_size = MKV_ALIGN(MKV_ENTRY_HEADER_SIZE + key_len + len + MKV_ENTRY_CRC_SIZE);
    log_d("Set key='%s', entry_size=%u", key, entry_size);

    if (g_mkv.write_offset + entry_size > g_mkv.flash_ops.sector_size)
    {
        MKV_SectorHeader_t next_hdr;
        uint8_t            next_idx = (g_mkv.active_sector + 1) % g_mkv.flash_ops.sector_count;

        if (MKV_ReadSectorHeader(next_idx, &next_hdr) == 0 && next_hdr.magic == MKV_MAGIC)
        {
            log_i("Next sector %u is valid, compacting...", next_idx);
            MKV_Error_t err = MKV_Compact();
            if (err != MKV_OK)
            {
                return err;
            }
        }
        else
        {
            MKV_Error_t err = MKV_SwitchToNextSector();
            if (err != MKV_OK)
            {
                return err;
            }
        }

        if (g_mkv.write_offset + entry_size > g_mkv.flash_ops.sector_size)
        {
            return MKV_ERR_NO_SPACE;
        }
    }

    uint8_t      buf[MKV_ENTRY_HEADER_SIZE + MKV_MAX_KEY_LEN + MKV_MAX_VALUE_LEN + MKV_ENTRY_CRC_SIZE];
    MKV_Entry_t* entry = (MKV_Entry_t*) buf;
    log_d("Writing entry at offset %u in sector %u", g_mkv.write_offset, g_mkv.active_sector);
    entry->state   = MKV_STATE_WRITING;
    entry->key_len = key_len;
    entry->val_len = len;
    log_i("Preparing to write entry: key='%s', key_len=%u, val_len=%u", key, key_len, len);
    memcpy(buf + MKV_ENTRY_HEADER_SIZE, key, key_len);
    memcpy(buf + MKV_ENTRY_HEADER_SIZE + key_len, value, len);

    uint16_t crc = MKV_CRC16(buf + MKV_ENTRY_HEADER_SIZE, key_len + len);
    memcpy(buf + MKV_ENTRY_HEADER_SIZE + key_len + len, &crc, 2);

    uint32_t write_addr = MKV_SECTOR_ADDR(g_mkv.active_sector) + g_mkv.write_offset;
    if (g_mkv.flash_ops.write_func(write_addr, buf, entry_size) != 0)
    {
        return MKV_ERR_FLASH;
    }
    log_i("Wrote entry key='%s' at addr 0x%08X", key, write_addr);

    uint16_t valid = MKV_STATE_VALID;
    if (g_mkv.flash_ops.write_func(write_addr, (uint8_t*) &valid, 2) != 0)
    {
        return MKV_ERR_FLASH;
    }

    g_mkv.write_offset += entry_size;

#if MKV_CACHE_ENABLE
    if (len > 0)
    {
        MKV_CacheUpdate(key, value, len);
    }
#endif

#if MKV_INCREMENTAL_GC
    // 写入后执行增量GC
    MKV_DoIncrementalGC();
#endif

    return MKV_OK;
}

MKV_Error_t mkv_get(const char* key, void* buffer, uint8_t buf_size, uint8_t* out_len)
{
    if (!g_mkv.initialized || !key || !buffer)
    {
        return MKV_ERR_INVALID;
    }

#if MKV_CACHE_ENABLE
    MKV_CacheEntry_t* cached = MKV_CacheFind(key);
    if (cached)
    {
        uint8_t copy_len = (cached->val_len < buf_size) ? cached->val_len : buf_size;
        memcpy(buffer, cached->value, copy_len);
        if (out_len)
        {
            *out_len = copy_len;
        }
        log_d("Cache hit for key='%s'", key);
        return MKV_OK;
    }
    log_d("Cache miss for key='%s'", key);
#endif

    MKV_Entry_t entry;
    uint32_t    addr = MKV_FindKey(key, &entry);
    log_d("Get key='%s', found at addr 0x%08X", key, addr);
    if (addr == 0)
    {
        return MKV_ERR_NOT_FOUND;
    }

    if (entry.val_len == 0)
    {
        return MKV_ERR_NOT_FOUND;
    }

    uint8_t read_len = (entry.val_len < buf_size) ? entry.val_len : buf_size;
    if (g_mkv.flash_ops.read_func(addr + MKV_ENTRY_HEADER_SIZE + entry.key_len, buffer, read_len) != 0)
    {
        return MKV_ERR_FLASH;
    }

#if MKV_CACHE_ENABLE
    MKV_CacheUpdate(key, buffer, read_len);
#endif

    if (out_len)
    {
        *out_len = read_len;
    }

    return MKV_OK;
}

MKV_Error_t mkv_del(const char* key)
{
#if MKV_CACHE_ENABLE
    MKV_CacheRemove(key);
#endif
    uint8_t dummy = 0;
    return mkv_set(key, &dummy, 0);
}

uint8_t mkv_exists(const char* key)
{
    if (!g_mkv.initialized || !key)
    {
        return 0;
    }

    MKV_Entry_t entry;
    uint32_t    addr = MKV_FindKey(key, &entry);

    return (addr != 0 && entry.val_len > 0);
}

void mkv_get_usage(uint32_t* used, uint32_t* total)
{
    if (used)
    {
        *used = g_mkv.write_offset;
    }
    if (total)
    {
        *total = g_mkv.flash_ops.sector_size * g_mkv.flash_ops.sector_count;
    }
}

#if MKV_INCREMENTAL_GC
uint8_t mkv_gc_step(uint8_t steps)
{
    if (!g_mkv.initialized)
    {
        return 0;
    }

    // 检查是否需要启动GC
    if (!g_mkv.gc_active && MKV_ShouldStartGC())
    {
        MKV_StartIncrementalGC();
    }

    if (!g_mkv.gc_active)
    {
        return 0;
    }

    // 执行指定步数
    for (uint8_t i = 0; i < steps; i++)
    {
        if (!MKV_IncrementalGCStep())
        {
            return 0;  // GC完成
        }
    }

    return 1;  // 还在进行中
}

uint8_t mkv_gc_is_active(void)
{
    return g_mkv.gc_active;
}
#endif

#if MKV_CACHE_ENABLE
void mkv_get_cache_stats(MKV_CacheStats_t* stats)
{
    if (!stats)
    {
        return;
    }

    stats->hit_count  = g_mkv.cache.hit_count;
    stats->miss_count = g_mkv.cache.miss_count;

    uint32_t total = stats->hit_count + stats->miss_count;
    if (total > 0)
    {
        stats->hit_rate = (float) stats->hit_count / total * 100.0f;
    }
    else
    {
        stats->hit_rate = 0.0f;
    }
}

void mkv_cache_clear(void)
{
    memset(&g_mkv.cache, 0, sizeof(g_mkv.cache));
}
#endif

/* ==================== 默认值 API 实现 ==================== */

void mkv_set_defaults(const MKV_Default_t* defaults, uint16_t count)
{
    g_mkv.defaults      = defaults;
    g_mkv.default_count = count;
    log_i("Set default table: %u entries", count);
}

const MKV_Default_t* mkv_find_default(const char* key)
{
    if (!key || !g_mkv.defaults)
    {
        return NULL;
    }

    uint8_t key_len = strlen(key);

    for (uint16_t i = 0; i < g_mkv.default_count; i++)
    {
        const MKV_Default_t* def = &g_mkv.defaults[i];
        if (def->key && strlen(def->key) == key_len && memcmp(def->key, key, key_len) == 0)
        {
            return def;
        }
    }

    return NULL;
}

MKV_Error_t mkv_get_default(const char* key, void* buffer, uint8_t buf_size, uint8_t* out_len)
{
    if (!key || !buffer)
    {
        return MKV_ERR_INVALID;
    }

    MKV_Error_t err = mkv_get(key, buffer, buf_size, out_len);
    if (err == MKV_OK)
    {
        return MKV_OK;
    }

    const MKV_Default_t* def = mkv_find_default(key);
    if (def)
    {
        uint8_t copy_len = (def->len < buf_size) ? def->len : buf_size;
        memcpy(buffer, def->value, copy_len);
        if (out_len)
        {
            *out_len = copy_len;
        }
        log_d("Return default value for key='%s'", key);
        return MKV_OK;
    }

    return MKV_ERR_NOT_FOUND;
}

MKV_Error_t mkv_reset_key(const char* key)
{
    if (!key)
    {
        return MKV_ERR_INVALID;
    }

    const MKV_Default_t* def = mkv_find_default(key);
    if (!def)
    {
        log_w("No default value for key='%s'", key);
        return MKV_ERR_NOT_FOUND;
    }

    MKV_Error_t err = mkv_set(key, def->value, def->len);
    if (err == MKV_OK)
    {
        log_i("Reset key='%s' to default", key);
    }

    return err;
}

MKV_Error_t mkv_reset_all(void)
{
    if (!g_mkv.defaults)
    {
        return MKV_ERR_INVALID;
    }

    log_i("Reset all %u keys to defaults...", g_mkv.default_count);

    for (uint16_t i = 0; i < g_mkv.default_count; i++)
    {
        const MKV_Default_t* def = &g_mkv.defaults[i];
        if (def->key && def->value && def->len > 0)
        {
            MKV_Error_t err = mkv_set(def->key, def->value, def->len);
            if (err != MKV_OK)
            {
                log_e("Failed to reset key='%s', err=%d", def->key, err);
                return err;
            }
        }
    }

    log_i("All defaults restored");
    return MKV_OK;
}
