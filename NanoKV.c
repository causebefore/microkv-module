/**
 * @file NanoKV.c
 * @brief NanoKV - 轻量级KV/TLV存储实现
 * @version 3.0
 * @date 2026-01-18
 *
 * @details 核心功能：
 * - KV存储：键值对存储，支持默认值回退
 * - TLV存储：类型-长度-值存储，支持历史记录和保留策略
 * - LFU缓存：加速热点数据读取，可配置缓存大小
 * - 增量GC：分摊垃圾回收开销，避免长时间阻塞
 * - 掉电安全：WRITING→VALID状态机保护数据完整性
 * - 多扇区环形：充分利用Flash空间，自动磨损均衡
 */

#include "NanoKV.h"

#include <string.h>

/* ==================== 内部实例与辅助宏 ==================== */
static nkv_instance_t g_nkv = {0};
static void           nkv_sync_version(void);
static nkv_err_t      update_entry_state(uint32_t addr, uint16_t state);

#define NKV_VER_KEY "__nkv_ver__"

#define SECTOR_ADDR(i)    (g_nkv.flash.base + (i) * g_nkv.flash.sector_size)            // 扇区地址
#define ALIGN(x)          (((x) + (g_nkv.flash.align - 1)) & ~(g_nkv.flash.align - 1))  // 对齐
#define ALIGNED_HDR_SIZE  ALIGN(NKV_SECTOR_HDR_SIZE)
#define PREV_SECTOR(c, o) (((c) + g_nkv.flash.sector_count - (o)) % g_nkv.flash.sector_count)
#define ENTRY_SIZE(e)     ALIGN(NKV_HEADER_SIZE + (e).key_len + (e).val_len + NKV_CRC_SIZE)
#define MAX_ENTRY_SIZE    (NKV_HEADER_SIZE + NKV_MAX_KEY_LEN + NKV_MAX_VALUE_LEN + NKV_CRC_SIZE + 32)

/* ==================== TLV保留策略 ==================== */
#if NKV_TLV_RETENTION_ENABLE
typedef struct
{
    uint8_t  type;
    uint16_t keep_count;
} tlv_retention_t;

typedef struct
{
    uint8_t  type;
    uint32_t threshold;
} tlv_keep_info_t;

static tlv_retention_t g_tlv_retention[NKV_TLV_RETENTION_MAX];
static uint8_t         g_tlv_retention_count = 0;
static tlv_keep_info_t g_tlv_keep_info[NKV_TLV_RETENTION_MAX];
static uint8_t         g_tlv_keep_info_count = 0;
#endif

/* TLV默认值表 */
static const nkv_tlv_default_t* g_tlv_defaults      = NULL;
static uint16_t                 g_tlv_default_count = 0;

/* ==================== CRC16计算 ==================== */
/* 计算MODBUS CRC16校验值 */
static uint16_t calc_crc16(const uint8_t* data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

/* ==================== 位图操作 ==================== */
/* 计算键哈希值(用于GC加速) */
static uint8_t hash_key(const char* key, uint8_t len)
{
    uint16_t hash = 0;
    for (uint8_t i = 0; i < len; i++)
        hash = hash * 31 + key[i];
    return (uint8_t) (hash & 0xFF);
}

/**
 * @brief 快速探测 Flash 范围是否全为 0xFF
 * @param addr 起始地址
 * @param size 大小
 * @return 1=全为 0xFF, 0=存在非 0xFF 字节
 */
static uint8_t nkv_is_erased(uint32_t addr, uint32_t size)
{
    uint32_t buf[16]; /* 每次读取 64 字节 */
    uint32_t len;
    while (size > 0)
    {
        len = (size > sizeof(buf)) ? sizeof(buf) : size;
        if (g_nkv.flash.read(addr, (uint8_t*) buf, len) != 0)
            return 0;

        uint32_t words = len / 4;
        for (uint32_t i = 0; i < words; i++)
        {
            if (buf[i] != 0xFFFFFFFF)
                return 0;
        }
        for (uint32_t i = words * 4; i < len; i++)
        {
            if (((uint8_t*) buf)[i] != 0xFF)
                return 0;
        }
        addr += len;
        size -= len;
    }
    return 1;
}

static inline void bitmap_set(uint8_t* bmp, uint8_t idx)
{
    bmp[idx >> 3] |= (1 << (idx & 7));
}

static inline uint8_t bitmap_test(const uint8_t* bmp, uint8_t idx)
{
    return (bmp[idx >> 3] >> (idx & 7)) & 1;
}

/* ==================== 缓存实现 ==================== */
#if NKV_CACHE_ENABLE
/* 查找缓存中的键 */
static nkv_cache_entry_t* cache_find(const char* key)
{
    uint8_t klen = strlen(key);
    for (uint8_t i = 0; i < NKV_CACHE_SIZE; i++)
    {
        nkv_cache_entry_t* e = &g_nkv.cache.entries[i];
        if (e->valid && e->key_len == klen && memcmp(e->key, key, klen) == 0)
        {
            e->access_count++;
            g_nkv.cache.hit_count++;
            return e;
        }
    }
    g_nkv.cache.miss_count++;
    return NULL;
}

/* 查找LFU替换候选 */
static uint8_t cache_find_lfu(void)
{
    uint8_t  idx = 0;
    uint32_t min = 0xFFFFFFFF;
    for (uint8_t i = 0; i < NKV_CACHE_SIZE; i++)
    {
        if (!g_nkv.cache.entries[i].valid)
            return i;
        if (g_nkv.cache.entries[i].access_count < min)
        {
            min = g_nkv.cache.entries[i].access_count;
            idx = i;
        }
    }
    return idx;
}

/* 更新缓存 */
static void cache_update(const char* key, const void* val, uint8_t len)
{
    uint8_t            klen = strlen(key);
    nkv_cache_entry_t* e    = NULL;

    /* 查找现有条目 */
    for (uint8_t i = 0; i < NKV_CACHE_SIZE; i++)
    {
        if (g_nkv.cache.entries[i].valid && g_nkv.cache.entries[i].key_len == klen &&
            memcmp(g_nkv.cache.entries[i].key, key, klen) == 0)
        {
            e = &g_nkv.cache.entries[i];
            break;
        }
    }

    /* 未找到则使用LFU替换 */
    if (!e)
    {
        e          = &g_nkv.cache.entries[cache_find_lfu()];
        e->key_len = klen;
        memcpy(e->key, key, klen);
        e->access_count = 1;
    }

    e->val_len = len;
    memcpy(e->value, val, len);
    e->valid = 1;
}

/* 移除缓存项 */
static void cache_remove(const char* key)
{
    uint8_t klen = strlen(key);
    for (uint8_t i = 0; i < NKV_CACHE_SIZE; i++)
    {
        if (g_nkv.cache.entries[i].valid && g_nkv.cache.entries[i].key_len == klen &&
            memcmp(g_nkv.cache.entries[i].key, key, klen) == 0)
        {
            g_nkv.cache.entries[i].valid = 0;
            break;
        }
    }
}
#endif

/* ==================== 条目匹配器 ==================== */
typedef struct
{
    const char* key;
    uint8_t     key_len;
} kv_match_ctx_t;

typedef struct
{
    uint8_t type;
} tlv_match_ctx_t;

/* KV键匹配器 */
static uint8_t kv_matcher(const nkv_entry_t* entry, uint32_t addr, void* ctx)
{
    /* 仅匹配 VALID 或 PRE_DEL 状态 (PRE_DEL 在掉电恢复期间被视为有效) */
    if (entry->state != NKV_STATE_VALID && entry->state != NKV_STATE_PRE_DEL)
        return 0;
    kv_match_ctx_t* c = (kv_match_ctx_t*) ctx;
    if (entry->key_len != c->key_len)
        return 0;

    char tmp[NKV_MAX_KEY_LEN];
    g_nkv.flash.read(addr + NKV_HEADER_SIZE, (uint8_t*) tmp, c->key_len);
    return (memcmp(tmp, c->key, c->key_len) == 0);
}

/* TLV类型匹配器 */
static uint8_t tlv_matcher(const nkv_entry_t* entry, uint32_t addr, void* ctx)
{
    if ((entry->state != NKV_STATE_VALID && entry->state != NKV_STATE_PRE_DEL) || entry->key_len != 0 ||
        entry->val_len == 0)
        return 0;

    tlv_match_ctx_t* c = (tlv_match_ctx_t*) ctx;
    uint8_t          type;
    if (g_nkv.flash.read(addr + NKV_HEADER_SIZE, &type, 1) != 0)
        return 0;
    return (type == c->type);
}

/* ==================== 扇区操作 ==================== */
/* 读取扇区头 */
static int read_sector_hdr(uint8_t idx, nkv_sector_hdr_t* hdr)
{
    return g_nkv.flash.read(SECTOR_ADDR(idx), (uint8_t*) hdr, sizeof(nkv_sector_hdr_t));
}

/* 检查扇区是否有效 */
uint8_t nkv_is_sector_valid(uint8_t idx)
{
    nkv_sector_hdr_t hdr;
    if (read_sector_hdr(idx, &hdr) != 0)
        return 0;
    return (hdr.magic == NKV_MAGIC);
}

/* 扫描扇区写入偏移，并清理异常状态 */
static uint32_t scan_write_offset(uint8_t idx)
{
    uint32_t sector      = SECTOR_ADDR(idx);
    uint32_t sector_size = g_nkv.flash.sector_size;
    uint32_t low         = ALIGNED_HDR_SIZE;
    uint32_t high        = sector_size;

    /*
     * 快速探测：二分查找第一个连续 0xFF 的区域 (约 256 字节)
     * 这可以帮助我们快速跳过大部分已写区域，定位到空闲区边界。
     */
    const uint32_t probe_size = 256;
    while (high - low > probe_size)
    {
        uint32_t mid = low + ((high - low) / 2);
        mid          = ALIGN(mid);
        if (nkv_is_erased(sector + mid, probe_size))
        {
            high = mid;
        }
        else
        {
            low = mid + probe_size;
        }
    }

    /*
     * 线性扫描以精确确定写偏移。
     * 由于 KV 是变长链接且没有 Sync Word，必须从头扫描以保证正确性。
     */
    uint32_t offset = ALIGNED_HDR_SIZE;
    while (offset <= sector_size - ALIGN(NKV_HEADER_SIZE))
    {
        nkv_entry_t entry;
        if (g_nkv.flash.read(sector + offset, (uint8_t*) &entry, NKV_HEADER_SIZE) != 0)
            break;

        if (entry.state == NKV_STATE_ERASED)
        {
            /* 发现 0xFFFF，进一步确认后面是否全是 0xFF */
            if (nkv_is_erased(sector + offset, (sector_size - offset > 32) ? 32 : (sector_size - offset)))
            {
                break;
            }
        }

#if NKV_CLEAN_DIRTY_ON_BOOT
        /* 掉电恢复：清理 WRITING 状态的脏数据（写入中掉电的不完整条目） */
        if (entry.state == NKV_STATE_WRITING)
        {
            update_entry_state(sector + offset, NKV_STATE_DELETED);
        }
#endif

        uint32_t entry_sz = ENTRY_SIZE(entry);
        if (entry_sz < ALIGN(NKV_HEADER_SIZE + NKV_CRC_SIZE)) /* 异常条目大小 */
            break;

        offset += entry_sz;
    }

    return offset;
}

/* 通用扇区查找 */
static uint32_t find_in_sector(uint8_t idx, uint8_t (*matcher)(const nkv_entry_t*, uint32_t, void*), void* ctx,
                               nkv_entry_t* out)
{
    uint32_t sector = SECTOR_ADDR(idx);
    uint32_t found = 0, offset = ALIGNED_HDR_SIZE;

    while (offset <= g_nkv.flash.sector_size - ALIGN(NKV_HEADER_SIZE))
    {
        nkv_entry_t entry;
        if (g_nkv.flash.read(sector + offset, (uint8_t*) &entry, NKV_HEADER_SIZE) != 0)
            break;
        if (entry.state == NKV_STATE_ERASED)
            break;

        uint32_t addr = sector + offset;
        if (matcher(&entry, addr, ctx))
        {
            found = addr;
            if (out)
                *out = entry;
        }
        offset += ENTRY_SIZE(entry);
    }
    return found;
}

/* 在扇区中查找键 */
static uint32_t find_key_in_sector(uint8_t idx, const char* key, nkv_entry_t* out)
{
    kv_match_ctx_t ctx = {.key = key, .key_len = strlen(key)};
    return find_in_sector(idx, kv_matcher, &ctx, out);
}

/* 在所有扇区中查找键 */
static uint32_t find_key(const char* key, nkv_entry_t* out)
{
    for (uint8_t i = 0; i < g_nkv.flash.sector_count; i++)
    {
        uint8_t idx = PREV_SECTOR(g_nkv.active_sector, i);
        if (!nkv_is_sector_valid(idx))
            continue;
        uint32_t addr = find_key_in_sector(idx, key, out);
        if (addr != 0)
            return addr;
    }
    return 0;
}

/* 切换到指定扇区 */
static nkv_err_t switch_to_sector(uint8_t idx)
{
    uint32_t addr = SECTOR_ADDR(idx);

    /* 快速探测：如果扇区已经是干净的，则跳过擦除 */
    if (!nkv_is_erased(addr, g_nkv.flash.sector_size))
    {
        if (g_nkv.flash.erase(addr) != 0)
            return NKV_ERR_FLASH;
    }

    nkv_sector_hdr_t hdr     = {.magic = NKV_MAGIC, .seq = g_nkv.sector_seq + 1};
    uint32_t         hdr_len = ALIGN(sizeof(nkv_sector_hdr_t));
    uint8_t          buf[32]; /* 足够容纳对齐后的头 */

    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &hdr, sizeof(hdr));

    if (g_nkv.flash.write(addr, buf, hdr_len) != 0)
        return NKV_ERR_FLASH;

    g_nkv.active_sector = idx;
    g_nkv.sector_seq    = hdr.seq;
    g_nkv.write_offset  = ALIGNED_HDR_SIZE;
    return NKV_OK;
}

/* 查找空闲扇区 */
static int8_t find_free_sector(void)
{
    for (uint8_t i = 1; i < g_nkv.flash.sector_count; i++)
    {
        uint8_t idx = (g_nkv.active_sector + i) % g_nkv.flash.sector_count;
        if (!nkv_is_sector_valid(idx))
            return idx;
    }
    return -1;
}

/* 切换到下一扇区 */
static nkv_err_t switch_to_next_sector(void)
{
    return switch_to_sector((g_nkv.active_sector + 1) % g_nkv.flash.sector_count);
}

/* ==================== 条目迁移 ==================== */
#if NKV_TLV_RETENTION_ENABLE
static void clear_tlv_keep_info(void)
{
    g_tlv_keep_info_count = 0;
}

/* 计算TLV保留阈值 */
static uint32_t find_tlv_keep_threshold(uint8_t type, uint16_t keep)
{
    nkv_tlv_history_t hist[32];
    uint8_t           count = 0;

    nkv_tlv_get_history(type, hist, 32, &count);
    if (count == 0 || count <= keep)
        return 0;

    return hist[keep].flash_addr - NKV_HEADER_SIZE - 1;
}

/* 准备所有TLV保留阈值 */
static void prepare_tlv_keep_info(void)
{
    clear_tlv_keep_info();
    for (uint8_t i = 0; i < g_tlv_retention_count && g_tlv_keep_info_count < NKV_TLV_RETENTION_MAX; i++)
    {
        if (g_tlv_retention[i].keep_count == 0)
            continue;
        g_tlv_keep_info[g_tlv_keep_info_count].type = g_tlv_retention[i].type;
        g_tlv_keep_info[g_tlv_keep_info_count].threshold =
            find_tlv_keep_threshold(g_tlv_retention[i].type, g_tlv_retention[i].keep_count);
        g_tlv_keep_info_count++;
    }
}

/* 检查TLV是否应迁移 */
static uint8_t should_migrate_tlv(uint8_t type, uint32_t addr)
{
    for (uint8_t i = 0; i < g_tlv_keep_info_count; i++)
    {
        if (g_tlv_keep_info[i].type == type)
        {
            if (g_tlv_keep_info[i].threshold == 0)
                return 1;
            return (addr >= g_tlv_keep_info[i].threshold);
        }
    }
    return 1;
}
#endif

/* 迁移条目 */
static nkv_err_t migrate_entry(uint32_t src, const nkv_entry_t* entry)
{
    static uint8_t buf[MAX_ENTRY_SIZE];
    uint32_t       size = ENTRY_SIZE(*entry);

    if (g_nkv.write_offset + size > g_nkv.flash.sector_size)
        return NKV_ERR_NO_SPACE;
    if (g_nkv.flash.read(src, buf, size) != 0)
        return NKV_ERR_FLASH;

    uint32_t dest = SECTOR_ADDR(g_nkv.active_sector) + g_nkv.write_offset;
    if (g_nkv.flash.write(dest, buf, size) != 0)
        return NKV_ERR_FLASH;

    g_nkv.write_offset += size;
    return NKV_OK;
}

/* ==================== 全量GC ==================== */
static nkv_err_t do_compact(void)
{
#if NKV_TLV_RETENTION_ENABLE
    prepare_tlv_keep_info();
#endif

    nkv_err_t err = switch_to_next_sector();
    if (err != NKV_OK)
        return err;

    uint8_t bitmap[32] = {0};

    for (uint8_t s = 1; s < g_nkv.flash.sector_count; s++)
    {
        uint8_t idx = PREV_SECTOR(g_nkv.active_sector, s);
        if (!nkv_is_sector_valid(idx))
            continue;

        uint32_t sector = SECTOR_ADDR(idx);
        uint32_t offset = ALIGNED_HDR_SIZE;

        while (offset <= g_nkv.flash.sector_size - ALIGN(NKV_HEADER_SIZE))
        {
            nkv_entry_t entry;
            if (g_nkv.flash.read(sector + offset, (uint8_t*) &entry, NKV_HEADER_SIZE) != 0)
                break;
            if (entry.state == NKV_STATE_ERASED)
                break;

            uint32_t entry_size = ENTRY_SIZE(entry);

            /* 仅迁移 VALID 状态的数据。
             * PRE_DEL 状态的数据在 GC 时被视为旧数据，不予迁移（因为新键一定已存在或系统处于异常态）。
             * DELETED 和 WRITING 状态的数据直接跳过。
             */
            if (entry.state == NKV_STATE_VALID && entry.val_len > 0)
            {
#if NKV_TLV_RETENTION_ENABLE
                /* TLV保留检查 */
                if (entry.key_len == 0)
                {
                    uint8_t type;
                    g_nkv.flash.read(sector + offset + NKV_HEADER_SIZE, &type, 1);
                    if (!should_migrate_tlv(type, sector + offset))
                    {
                        offset += entry_size;
                        continue;
                    }
                }
#endif
                /* 读取键并检查是否需要迁移 */
                char key[NKV_MAX_KEY_LEN] = {0};
                g_nkv.flash.read(sector + offset + NKV_HEADER_SIZE, (uint8_t*) key, entry.key_len);

                uint8_t hash      = hash_key(key, entry.key_len);
                uint8_t need_copy = 0;

                if (!bitmap_test(bitmap, hash))
                {
                    need_copy = 1;
                }
                else
                {
                    nkv_entry_t existing;
                    if (find_key_in_sector(g_nkv.active_sector, key, &existing) == 0)
                        need_copy = 1;
                }

                if (need_copy)
                {
                    nkv_err_t ret = migrate_entry(sector + offset, &entry);
                    if (ret == NKV_ERR_NO_SPACE)
                    {
                        err = switch_to_next_sector();
                        if (err != NKV_OK)
                            return err;
                        memset(bitmap, 0, sizeof(bitmap));
                        ret = migrate_entry(sector + offset, &entry);
                        if (ret != NKV_OK)
                            return ret;
                    }
                    else if (ret != NKV_OK)
                    {
                        return ret;
                    }
                    bitmap_set(bitmap, hash);
                }
            }
            offset += entry_size;
        }
    }
    return NKV_OK;
}

/* ==================== 增量GC ==================== */
#if NKV_INCREMENTAL_GC
/* 统计空闲扇区数 */
static uint8_t count_free_sectors(void)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < g_nkv.flash.sector_count; i++)
        if (!nkv_is_sector_valid(i))
            count++;
    return count;
}

/* 检查是否需要启动GC */
static uint8_t should_start_gc(void)
{
    if (g_nkv.gc_active)
        return 0;
    return (count_free_sectors() < 1);
}

/* 启动增量GC */
static uint8_t start_incremental_gc(void)
{
    #if NKV_TLV_RETENTION_ENABLE
    prepare_tlv_keep_info();
    #endif

    uint8_t  oldest_idx = 0;
    uint16_t oldest_seq = 0xFFFF;

    for (uint8_t i = 0; i < g_nkv.flash.sector_count; i++)
    {
        if (i == g_nkv.active_sector)
            continue;
        nkv_sector_hdr_t hdr;
        if (read_sector_hdr(i, &hdr) == 0 && hdr.magic == NKV_MAGIC)
        {
            /*
             * 序号回绕处理：使用带符号差值比较
             * 当 seq 从 0xFFFF 溢出到 0x0000 时，普通比较会失败。
             * 例如：oldest_seq=0xFFFE, hdr.seq=0x0001
             *   普通比较: 0xFFFE > 0x0001 → 错误地认为旧扇区更新
             *   带符号差值: (int16_t)(0xFFFE - 0x0001) = -3 < 0 → 正确识别新扇区
             */
            if ((int16_t) (oldest_seq - hdr.seq) > 0)
            {
                oldest_seq = hdr.seq;
                oldest_idx = i;
            }
        }
    }

    if (oldest_seq == 0xFFFF)
        return 0;

    g_nkv.gc_src_sector = oldest_idx;
    g_nkv.gc_src_offset = ALIGNED_HDR_SIZE;
    g_nkv.gc_active     = 1;
    memset(g_nkv.gc_bitmap, 0, sizeof(g_nkv.gc_bitmap));
    return 1;
}

/* 执行一步增量GC */
static uint8_t incremental_gc_step(void)
{
    if (!g_nkv.gc_active)
        return 0;

    uint32_t sector = SECTOR_ADDR(g_nkv.gc_src_sector);

    while (g_nkv.gc_src_offset <= g_nkv.flash.sector_size - ALIGN(NKV_HEADER_SIZE))
    {
        nkv_entry_t entry;
        if (g_nkv.flash.read(sector + g_nkv.gc_src_offset, (uint8_t*) &entry, NKV_HEADER_SIZE) != 0)
            break;
        if (entry.state == NKV_STATE_ERASED)
            break;

        uint32_t entry_size = ENTRY_SIZE(entry);

        /* 增量 GC 同样只迁移 VALID 状态的数据 */
        if (entry.state != NKV_STATE_VALID || entry.val_len == 0)
        {
            g_nkv.gc_src_offset += entry_size;
            continue;
        }

    #if NKV_TLV_RETENTION_ENABLE
        if (entry.key_len == 0)
        {
            uint8_t type;
            g_nkv.flash.read(sector + g_nkv.gc_src_offset + NKV_HEADER_SIZE, &type, 1);
            if (!should_migrate_tlv(type, sector + g_nkv.gc_src_offset))
            {
                g_nkv.gc_src_offset += entry_size;
                continue;
            }
        }
    #endif

        char key[NKV_MAX_KEY_LEN] = {0};
        g_nkv.flash.read(sector + g_nkv.gc_src_offset + NKV_HEADER_SIZE, (uint8_t*) key, entry.key_len);

        uint8_t hash = hash_key(key, entry.key_len);

        if (!bitmap_test(g_nkv.gc_bitmap, hash))
        {
            nkv_entry_t new_entry;
            if (find_key_in_sector(g_nkv.active_sector, key, &new_entry) == 0)
            {
                migrate_entry(sector + g_nkv.gc_src_offset, &entry);
            }
            bitmap_set(g_nkv.gc_bitmap, hash);
        }

        g_nkv.gc_src_offset += entry_size;
        return 1;
    }

    /* 扫描完成，擦除源扇区 */
    g_nkv.flash.erase(SECTOR_ADDR(g_nkv.gc_src_sector));
    g_nkv.gc_active = 0;

    if (count_free_sectors() < 1)
        start_incremental_gc();
    return 0;
}

/* 执行增量GC */
static void do_incremental_gc(void)
{
    if (!g_nkv.gc_active && should_start_gc())
    {
        if (!start_incremental_gc())
            return;
    }
    if (g_nkv.gc_active)
    {
        for (uint8_t i = 0; i < NKV_GC_ENTRIES_PER_WRITE; i++)
            if (!incremental_gc_step())
                break;
    }
}
#endif

/* ==================== 公共API ==================== */

nkv_err_t nkv_internal_init(const nkv_flash_ops_t* ops)
{
    if (!ops || !ops->read || !ops->write || !ops->erase)
        return NKV_ERR_INVALID;
    if (ops->sector_count < 2)
        return NKV_ERR_INVALID;
    if (ops->align == 0 || (ops->align & (ops->align - 1)) != 0)
        return NKV_ERR_INVALID;

    /* 写入原子性断言：对齐值必须 >= 2 字节（状态字段大小），以保证状态更新的原子性 */
    NKV_ASSERT(ops->align >= sizeof(uint16_t) && "align < sizeof(state), atomic write not guaranteed");

    /* 断言：最大条目大小不能超过扇区大小的一半（确保 GC 迁移空间） */
    uint32_t max_entry = NKV_HEADER_SIZE + NKV_MAX_KEY_LEN + NKV_MAX_VALUE_LEN + NKV_CRC_SIZE + ops->align;
    NKV_ASSERT(max_entry <= ops->sector_size / 2 && "max_entry > sector_size/2, config invalid");

    memset(&g_nkv, 0, sizeof(nkv_instance_t));
    g_nkv.flash = *ops;
    return NKV_OK;
}

nkv_err_t nkv_scan(void)
{
    if (g_nkv.initialized)
        return NKV_OK;

    uint8_t  found = 0, active_idx = 0;
    uint16_t max_seq = 0;

    for (uint8_t i = 0; i < g_nkv.flash.sector_count; i++)
    {
        nkv_sector_hdr_t hdr;
        if (read_sector_hdr(i, &hdr) != 0)
            continue;
        if (hdr.magic == NKV_MAGIC)
        {
            /*
             * 序号回绕处理：使用带符号差值比较
             * 当 seq 从 0xFFFF 溢出到 0x0000 时，差值的最高位会正确反映新旧关系。
             * 例如：max_seq=0xFFFE, hdr.seq=0x0001
             *   (int16_t)(0x0001 - 0xFFFE) = 3 > 0 → 正确识别新扇区
             */
            if (!found || (int16_t) (hdr.seq - max_seq) > 0)
            {
                max_seq    = hdr.seq;
                active_idx = i;
                found      = 1;
            }
        }
    }

    if (!found)
        return nkv_format();

    g_nkv.active_sector = active_idx;
    g_nkv.sector_seq    = max_seq;
    g_nkv.write_offset  = scan_write_offset(active_idx);
    g_nkv.initialized   = 1;

    /* 扫描完成后检查并同步默认值 */
    nkv_sync_version();

    return NKV_OK;
}

nkv_err_t nkv_format(void)
{
    for (uint8_t i = 0; i < g_nkv.flash.sector_count; i++)
    {
        uint32_t addr = SECTOR_ADDR(i);
        if (!nkv_is_erased(addr, g_nkv.flash.sector_size))
        {
            if (g_nkv.flash.erase(addr) != 0)
                return NKV_ERR_FLASH;
        }
    }

    nkv_sector_hdr_t hdr     = {.magic = NKV_MAGIC, .seq = 1};
    uint32_t         hdr_len = ALIGN(sizeof(hdr));
    uint8_t          buf[32];

    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &hdr, sizeof(hdr));

    if (g_nkv.flash.write(SECTOR_ADDR(0), buf, hdr_len) != 0)
        return NKV_ERR_FLASH;

    g_nkv.active_sector = 0;
    g_nkv.sector_seq    = 1;
    g_nkv.write_offset  = ALIGNED_HDR_SIZE;
    g_nkv.initialized   = 1;
    return NKV_OK;
}

/* 更新条目状态 */
static nkv_err_t update_entry_state(uint32_t addr, uint16_t state)
{
    static uint8_t buf[32];
    memset(buf, 0xFF, sizeof(buf));
    uint16_t* s = (uint16_t*) buf;
    *s          = state;
    return (g_nkv.flash.write(addr, buf, g_nkv.flash.align) == 0) ? NKV_OK : NKV_ERR_FLASH;
}

nkv_err_t nkv_set(const char* key, const void* value, uint8_t len)
{
    if (!g_nkv.initialized || !key || len > NKV_MAX_VALUE_LEN)
        return NKV_ERR_INVALID;
    if (len > 0 && !value)
        return NKV_ERR_INVALID;

    uint8_t key_len = strlen(key);
    if (key_len >= NKV_MAX_KEY_LEN)
        return NKV_ERR_INVALID;

    /* 1. 查找旧条目 */
    nkv_entry_t old_entry;
    uint32_t    old_addr  = find_key(key, &old_entry);
    uint8_t     is_update = (old_addr != 0 && old_entry.val_len > 0);

    /* 2. 计算并检查条目大小 */
    uint32_t entry_size = ALIGN(NKV_HEADER_SIZE + key_len + len + NKV_CRC_SIZE);

    /* 断言：单个条目不能超过扇区有效空间 */
    NKV_ASSERT(entry_size <= g_nkv.flash.sector_size - ALIGNED_HDR_SIZE && "entry_size exceeds sector capacity");

    if (g_nkv.write_offset + entry_size > g_nkv.flash.sector_size)
    {
        int8_t free_idx = find_free_sector();
        if (free_idx >= 0)
        {
            nkv_err_t err = switch_to_sector((uint8_t) free_idx);
            if (err != NKV_OK)
                return err;
        }
        else
        {
            nkv_err_t err = do_compact();
            if (err != NKV_OK)
                return err;
        }
        if (g_nkv.write_offset + entry_size > g_nkv.flash.sector_size)
            return NKV_ERR_NO_SPACE;
    }

    /* 3. 二阶段提交：如果是更新，先标记旧键为 PRE_DEL */
    if (is_update)
    {
        update_entry_state(old_addr, NKV_STATE_PRE_DEL);
    }

    /* 4. 构建并写入新条目 (WRITING 状态) */
    static uint8_t buf[MAX_ENTRY_SIZE];
    nkv_entry_t*   entry = (nkv_entry_t*) buf;

    memset(buf, 0xFF, entry_size);
    entry->state   = NKV_STATE_WRITING;
    entry->key_len = key_len;
    entry->val_len = len;

    memcpy(buf + NKV_HEADER_SIZE, key, key_len);
    memcpy(buf + NKV_HEADER_SIZE + key_len, value, len);

    uint16_t crc = calc_crc16(buf + NKV_HEADER_SIZE, key_len + len);
    memcpy(buf + NKV_HEADER_SIZE + key_len + len, &crc, 2);

    uint32_t new_addr = SECTOR_ADDR(g_nkv.active_sector) + g_nkv.write_offset;
    if (g_nkv.flash.write(new_addr, buf, entry_size) != 0)
        return NKV_ERR_FLASH;

    /* 5. 标记新键为 VALID */
    update_entry_state(new_addr, NKV_STATE_VALID);

    /* 6. 标记旧键为 DELETED */
    if (is_update)
    {
        update_entry_state(old_addr, NKV_STATE_DELETED);
    }

    g_nkv.write_offset += entry_size;

#if NKV_CACHE_ENABLE
    if (len > 0)
        cache_update(key, value, len);
    else
        cache_remove(key);
#endif

#if NKV_INCREMENTAL_GC
    do_incremental_gc();
#endif

    return NKV_OK;
}

nkv_err_t nkv_get(const char* key, void* buf, uint8_t size, uint8_t* out_len)
{
    if (!g_nkv.initialized || !key || !buf)
        return NKV_ERR_INVALID;

#if NKV_CACHE_ENABLE
    nkv_cache_entry_t* cached = cache_find(key);
    if (cached)
    {
        uint8_t len = (cached->val_len < size) ? cached->val_len : size;
        memcpy(buf, cached->value, len);
        if (out_len)
            *out_len = len;
        return NKV_OK;
    }
#endif

    nkv_entry_t entry;
    uint32_t    addr = find_key(key, &entry);
    if (addr == 0 || entry.val_len == 0)
        return NKV_ERR_NOT_FOUND;

#if NKV_VERIFY_ON_READ
    /* CRC 校验：读取完整的 Key+Value 并验证 */
    {
        static uint8_t verify_buf[NKV_MAX_KEY_LEN + NKV_MAX_VALUE_LEN];
        uint16_t       data_len = entry.key_len + entry.val_len;
        uint16_t       stored_crc, calc_crc;

        if (g_nkv.flash.read(addr + NKV_HEADER_SIZE, verify_buf, data_len) != 0)
            return NKV_ERR_FLASH;
        if (g_nkv.flash.read(addr + NKV_HEADER_SIZE + data_len, (uint8_t*) &stored_crc, NKV_CRC_SIZE) != 0)
            return NKV_ERR_FLASH;

        calc_crc = calc_crc16(verify_buf, data_len);
        if (calc_crc != stored_crc)
            return NKV_ERR_CRC;

        /* CRC 通过，从 verify_buf 中复制值 */
        uint8_t len = (entry.val_len < size) ? entry.val_len : size;
        memcpy(buf, verify_buf + entry.key_len, len);
        if (out_len)
            *out_len = len;
    }
#else
    uint8_t len = (entry.val_len < size) ? entry.val_len : size;
    if (g_nkv.flash.read(addr + NKV_HEADER_SIZE + entry.key_len, buf, len) != 0)
        return NKV_ERR_FLASH;
    if (out_len)
        *out_len = len;
#endif

#if NKV_CACHE_ENABLE
    cache_update(key, buf, (entry.val_len < size) ? entry.val_len : size);
#endif

    return NKV_OK;
}

nkv_err_t nkv_del(const char* key)
{
#if NKV_CACHE_ENABLE
    cache_remove(key);
#endif
    return nkv_set(key, NULL, 0);
}

uint8_t nkv_exists(const char* key)
{
    if (!g_nkv.initialized || !key)
        return 0;
    nkv_entry_t entry;
    uint32_t    addr = find_key(key, &entry);
    return (addr != 0 && entry.val_len > 0);
}

void nkv_get_usage(uint32_t* used, uint32_t* total)
{
    if (used)
        *used = g_nkv.write_offset;
    if (total)
        *total = g_nkv.flash.sector_size * g_nkv.flash.sector_count;
}

#if NKV_INCREMENTAL_GC
uint8_t nkv_gc_step(uint8_t steps)
{
    if (!g_nkv.initialized)
        return 0;
    if (!g_nkv.gc_active && should_start_gc())
        start_incremental_gc();
    if (!g_nkv.gc_active)
        return 0;

    for (uint8_t i = 0; i < steps; i++)
        if (!incremental_gc_step())
            return 0;
    return 1;
}

uint8_t nkv_gc_active(void)
{
    return g_nkv.gc_active;
}
#endif

#if NKV_CACHE_ENABLE
void nkv_cache_stats(nkv_cache_stats_t* stats)
{
    if (!stats)
        return;
    stats->hit_count  = g_nkv.cache.hit_count;
    stats->miss_count = g_nkv.cache.miss_count;
    uint32_t total    = stats->hit_count + stats->miss_count;
    stats->hit_rate   = (total > 0) ? ((float) stats->hit_count / total * 100.0f) : 0.0f;
}

void nkv_cache_clear(void)
{
    memset(&g_nkv.cache, 0, sizeof(g_nkv.cache));
}
#endif

/* ==================== 默认值同步 ==================== */

static void nkv_sync_version(void)
{
    if (!g_nkv.initialized)
        return;

    uint32_t  saved_ver = 0;
    nkv_err_t err       = nkv_get(NKV_VER_KEY, &saved_ver, sizeof(saved_ver), NULL);

    if (err != NKV_OK || saved_ver != NKV_SETTING_VER)
    {
        NKV_LOG_I("Config version changed: %d -> %d, syncing defaults...", (int) saved_ver, NKV_SETTING_VER);

        /* 同步 KV 默认值 */
        if (g_nkv.defaults)
        {
            for (uint16_t i = 0; i < g_nkv.default_count; i++)
            {
                if (g_nkv.defaults[i].key && !nkv_exists(g_nkv.defaults[i].key))
                {
                    nkv_set(g_nkv.defaults[i].key, g_nkv.defaults[i].value, g_nkv.defaults[i].len);
                }
            }
        }

        /* 同步 TLV 默认值 */
        if (g_tlv_defaults)
        {
            for (uint16_t i = 0; i < g_tlv_default_count; i++)
            {
                if (g_tlv_defaults[i].type != 0 && !nkv_tlv_exists(g_tlv_defaults[i].type))
                {
                    nkv_tlv_set(g_tlv_defaults[i].type, g_tlv_defaults[i].value, g_tlv_defaults[i].len);
                }
            }
        }

        uint32_t new_ver = NKV_SETTING_VER;
        nkv_set(NKV_VER_KEY, &new_ver, sizeof(new_ver));
    }
}

/* ==================== 默认值API ==================== */

void nkv_set_defaults(const nkv_default_t* defs, uint16_t count)
{
    g_nkv.defaults      = defs;
    g_nkv.default_count = count;

    nkv_sync_version();
}

const nkv_default_t* nkv_find_default(const char* key)
{
    if (!key || !g_nkv.defaults)
        return NULL;
    uint8_t klen = strlen(key);

    for (uint16_t i = 0; i < g_nkv.default_count; i++)
    {
        const nkv_default_t* d = &g_nkv.defaults[i];
        if (d->key && strlen(d->key) == klen && memcmp(d->key, key, klen) == 0)
            return d;
    }
    return NULL;
}

nkv_err_t nkv_get_default(const char* key, void* buf, uint8_t size, uint8_t* out_len)
{
    if (!key || !buf)
        return NKV_ERR_INVALID;

    if (nkv_get(key, buf, size, out_len) == NKV_OK)
        return NKV_OK;

    const nkv_default_t* def = nkv_find_default(key);
    if (def)
    {
        uint8_t len = (def->len < size) ? def->len : size;
        memcpy(buf, def->value, len);
        if (out_len)
            *out_len = len;
        return NKV_OK;
    }
    return NKV_ERR_NOT_FOUND;
}

nkv_err_t nkv_reset_key(const char* key)
{
    if (!key)
        return NKV_ERR_INVALID;
    const nkv_default_t* def = nkv_find_default(key);
    if (!def)
        return NKV_ERR_NOT_FOUND;
    return nkv_set(key, def->value, def->len);
}

nkv_err_t nkv_reset_all(void)
{
    if (!g_nkv.defaults)
        return NKV_ERR_INVALID;

    for (uint16_t i = 0; i < g_nkv.default_count; i++)
    {
        const nkv_default_t* d = &g_nkv.defaults[i];
        if (d->key && d->value && d->len > 0)
        {
            nkv_err_t err = nkv_set(d->key, d->value, d->len);
            if (err != NKV_OK)
                return err;
        }
    }
    return NKV_OK;
}

nkv_instance_t* nkv_get_instance(void)
{
    return &g_nkv;
}

/* ==================== TLV实现 ==================== */

/* 在扇区中查找TLV类型 */
static uint32_t find_tlv_in_sector(uint8_t idx, uint8_t type, nkv_entry_t* out)
{
    tlv_match_ctx_t ctx = {.type = type};
    return find_in_sector(idx, tlv_matcher, &ctx, out);
}

/* 在所有扇区中查找TLV类型 */
static uint32_t find_tlv(uint8_t type, nkv_entry_t* out)
{
    for (uint8_t i = 0; i < g_nkv.flash.sector_count; i++)
    {
        uint8_t idx = PREV_SECTOR(g_nkv.active_sector, i);
        if (!nkv_is_sector_valid(idx))
            continue;
        uint32_t addr = find_tlv_in_sector(idx, type, out);
        if (addr != 0)
            return addr;
    }
    return 0;
}

nkv_err_t nkv_tlv_set(uint8_t type, const void* value, uint8_t len)
{
    if (type == 0 || !value || len == 0 || len > 254)
        return NKV_ERR_INVALID;

    uint8_t data[256];
    data[0] = type;
    memcpy(data + 1, value, len);
    return nkv_set("", data, len + 1);
}

nkv_err_t nkv_tlv_get(uint8_t type, void* buf, uint8_t size, uint8_t* out_len)
{
    if (type == 0 || !buf || size == 0)
        return NKV_ERR_INVALID;

    nkv_entry_t entry;
    uint32_t    addr = find_tlv(type, &entry);
    if (addr == 0 || entry.val_len <= 1)
        return NKV_ERR_NOT_FOUND;

    uint8_t len      = entry.val_len - 1;
    uint8_t read_len = (len < size) ? len : size;

    if (g_nkv.flash.read(addr + NKV_HEADER_SIZE + 1, buf, read_len) != 0)
        return NKV_ERR_FLASH;

    if (out_len)
        *out_len = read_len;
    return NKV_OK;
}

nkv_err_t nkv_tlv_del(uint8_t type)
{
    if (type == 0)
        return NKV_ERR_INVALID;
    return nkv_set("", &type, 1);
}

uint8_t nkv_tlv_exists(uint8_t type)
{
    if (type == 0)
        return 0;
    nkv_entry_t entry;
    uint32_t    addr = find_tlv(type, &entry);
    return (addr != 0 && entry.val_len > 1);
}

/* TLV默认值 */
void nkv_tlv_set_defaults(const nkv_tlv_default_t* defs, uint16_t count)
{
    g_tlv_defaults      = defs;
    g_tlv_default_count = count;

    nkv_sync_version();
}

static const nkv_tlv_default_t* find_tlv_default(uint8_t type)
{
    if (!g_tlv_defaults)
        return NULL;
    for (uint16_t i = 0; i < g_tlv_default_count; i++)
        if (g_tlv_defaults[i].type == type)
            return &g_tlv_defaults[i];
    return NULL;
}

nkv_err_t nkv_tlv_get_default(uint8_t type, void* buf, uint8_t size, uint8_t* out_len)
{
    if (nkv_tlv_get(type, buf, size, out_len) == NKV_OK)
        return NKV_OK;

    const nkv_tlv_default_t* def = find_tlv_default(type);
    if (!def)
        return NKV_ERR_NOT_FOUND;

    uint8_t len = (def->len < size) ? def->len : size;
    memcpy(buf, def->value, len);
    if (out_len)
        *out_len = len;
    return NKV_OK;
}

nkv_err_t nkv_tlv_reset_type(uint8_t type)
{
    const nkv_tlv_default_t* def = find_tlv_default(type);
    if (!def)
        return NKV_ERR_NOT_FOUND;
    return nkv_tlv_set(type, def->value, def->len);
}

nkv_err_t nkv_tlv_reset_all(void)
{
    if (!g_tlv_defaults)
        return NKV_OK;
    for (uint16_t i = 0; i < g_tlv_default_count; i++)
    {
        nkv_err_t err = nkv_tlv_set(g_tlv_defaults[i].type, g_tlv_defaults[i].value, g_tlv_defaults[i].len);
        if (err != NKV_OK)
            return err;
    }
    return NKV_OK;
}

/* TLV迭代器 */
void nkv_tlv_iter_init(nkv_tlv_iter_t* iter)
{
    if (!iter)
        return;
    iter->sector_idx    = 0;
    iter->sector_offset = ALIGNED_HDR_SIZE;
    iter->finished      = 0;
}

uint8_t nkv_tlv_iter_next(nkv_tlv_iter_t* iter, nkv_tlv_entry_t* info)
{
    if (!iter || iter->finished || !info)
        return 0;

    while (iter->sector_idx < g_nkv.flash.sector_count)
    {
        if (!nkv_is_sector_valid(iter->sector_idx))
        {
            iter->sector_idx++;
            iter->sector_offset = ALIGNED_HDR_SIZE;
            continue;
        }

        uint32_t sector = g_nkv.flash.base + iter->sector_idx * g_nkv.flash.sector_size;

        while (iter->sector_offset <= g_nkv.flash.sector_size - ALIGN(NKV_HEADER_SIZE))
        {
            uint32_t    addr = sector + iter->sector_offset;
            nkv_entry_t entry;

            if (g_nkv.flash.read(addr, (uint8_t*) &entry, sizeof(nkv_entry_t)) != 0)
                break;
            if (entry.state == NKV_STATE_ERASED)
                break;

            iter->sector_offset += ENTRY_SIZE(entry);

            if ((entry.state == NKV_STATE_VALID || entry.state == NKV_STATE_PRE_DEL) && entry.key_len == 0 &&
                entry.val_len > 1)
            {
                uint8_t type;
                if (g_nkv.flash.read(addr + NKV_HEADER_SIZE, &type, 1) == 0)
                {
                    info->type       = type;
                    info->len        = entry.val_len - 1;
                    info->flash_addr = addr + NKV_HEADER_SIZE + 1;
                    return 1;
                }
            }
        }
        iter->sector_idx++;
        iter->sector_offset = ALIGNED_HDR_SIZE;
    }

    iter->finished = 1;
    return 0;
}

nkv_err_t nkv_tlv_iter_read(const nkv_tlv_entry_t* info, void* buf, uint8_t size)
{
    if (!info || !buf || size == 0)
        return NKV_ERR_INVALID;
    uint8_t len = (info->len < size) ? info->len : size;
    if (g_nkv.flash.read(info->flash_addr, buf, len) != 0)
        return NKV_ERR_FLASH;
    return NKV_OK;
}

/* TLV统计 */
void nkv_tlv_stats(uint16_t* count, uint32_t* used)
{
    uint16_t c = 0;
    uint32_t u = 0;

    nkv_tlv_iter_t  iter;
    nkv_tlv_entry_t info;
    nkv_tlv_iter_init(&iter);

    while (nkv_tlv_iter_next(&iter, &info))
    {
        c++;
        u += 7 + info.len; /* 头4B + 类型1B + 值NB + CRC2B */
    }

    if (count)
        *count = c;
    if (used)
        *used = u;
}

uint8_t nkv_tlv_has_data(void)
{
    nkv_tlv_iter_t  iter;
    nkv_tlv_entry_t info;
    nkv_tlv_iter_init(&iter);
    return nkv_tlv_iter_next(&iter, &info);
}

/* TLV历史记录 */
nkv_err_t nkv_tlv_get_history(uint8_t type, nkv_tlv_history_t* history, uint8_t max, uint8_t* count)
{
    if (type == 0 || !history || max == 0)
        return NKV_ERR_INVALID;

    nkv_tlv_history_t tmp[32];
    uint8_t           n = 0;

    nkv_tlv_iter_t  iter;
    nkv_tlv_entry_t info;
    nkv_tlv_iter_init(&iter);

    while (nkv_tlv_iter_next(&iter, &info) && n < 32)
    {
        if (info.type == type)
        {
            tmp[n].type        = type;
            tmp[n].len         = info.len;
            tmp[n].flash_addr  = info.flash_addr;
            tmp[n].write_order = info.flash_addr;
            n++;
        }
    }

    /* 按写入顺序排序(最新在前) */
    for (uint8_t i = 1; i < n; i++)
    {
        nkv_tlv_history_t t = tmp[i];
        int8_t            j = i - 1;
        while (j >= 0 && tmp[j].write_order < t.write_order)
        {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = t;
    }

    uint8_t copy = (n < max) ? n : max;
    for (uint8_t i = 0; i < copy; i++)
        history[i] = tmp[i];
    if (count)
        *count = copy;

    return NKV_OK;
}

nkv_err_t nkv_tlv_read_history(const nkv_tlv_history_t* entry, void* buf, uint8_t size)
{
    if (!entry || !buf || size == 0)
        return NKV_ERR_INVALID;
    uint8_t len = (entry->len < size) ? entry->len : size;
    if (g_nkv.flash.read(entry->flash_addr, buf, len) != 0)
        return NKV_ERR_FLASH;
    return NKV_OK;
}

/* TLV保留策略 */
#if NKV_TLV_RETENTION_ENABLE
nkv_err_t nkv_tlv_set_retention(uint8_t type, uint16_t keep)
{
    if (type == 0)
        return NKV_ERR_INVALID;

    /* 查找并更新现有策略 */
    for (uint8_t i = 0; i < g_tlv_retention_count; i++)
    {
        if (g_tlv_retention[i].type == type)
        {
            g_tlv_retention[i].keep_count = keep;
            return NKV_OK;
        }
    }

    /* 新增策略 */
    if (g_tlv_retention_count >= NKV_TLV_RETENTION_MAX)
        return NKV_ERR_INVALID;
    g_tlv_retention[g_tlv_retention_count].type       = type;
    g_tlv_retention[g_tlv_retention_count].keep_count = keep;
    g_tlv_retention_count++;
    return NKV_OK;
}

void nkv_tlv_clear_retention(uint8_t type)
{
    for (uint8_t i = 0; i < g_tlv_retention_count; i++)
    {
        if (g_tlv_retention[i].type == type)
        {
            for (uint8_t j = i; j < g_tlv_retention_count - 1; j++)
                g_tlv_retention[j] = g_tlv_retention[j + 1];
            g_tlv_retention_count--;
            return;
        }
    }
}
#endif
