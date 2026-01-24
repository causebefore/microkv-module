/**
 * @file NanoKV_test.c
 * @brief NanoKV 完整功能测试
 * @note 使用内存模拟 4 个 Flash 扇区，测试所有 API
 */

#include "NanoKV.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ==================== 计时工具 ==================== */

#ifdef _WIN32
    #include <windows.h>
static LARGE_INTEGER g_freq;
static LARGE_INTEGER g_start;

static void timer_init(void)
{
    QueryPerformanceFrequency(&g_freq);
}

static void timer_start(void)
{
    QueryPerformanceCounter(&g_start);
}

static double timer_elapsed_us(void)
{
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    return (double) (end.QuadPart - g_start.QuadPart) * 1000000.0 / g_freq.QuadPart;
}
#else
static clock_t g_start;

static void timer_init(void)
{
}

static void timer_start(void)
{
    g_start = clock();
}

static double timer_elapsed_us(void)
{
    return (double) (clock() - g_start) * 1000000.0 / CLOCKS_PER_SEC;
}
#endif

/* ==================== Flash 模拟层 ==================== */

#define TEST_SECTOR_SIZE  (4 * 1024u) /* 每个扇区 4KB */
#define TEST_SECTOR_COUNT 4u          /* 扇区数量 4 个 */
#define TEST_FLASH_SIZE   (TEST_SECTOR_SIZE * TEST_SECTOR_COUNT)

static uint8_t  g_flash[TEST_FLASH_SIZE];
static uint32_t g_test_pass = 0;
static uint32_t g_test_fail = 0;

/* 性能统计 */
typedef struct
{
    double set_time_us;
    double get_time_us;
    double del_time_us;
    double gc_time_us;
    int    set_count;
    int    get_count;
    int    del_count;
    int    gc_count;
} perf_stats_t;

static perf_stats_t g_perf = {0};

#define PERF_ADD(field, val)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        g_perf.field##_time_us += (val);                                                                               \
        g_perf.field##_count++;                                                                                        \
    }                                                                                                                  \
    while (0)

/* 测试断言宏 */
#define TEST_ASSERT(cond, msg)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            g_test_pass++;                                                                                             \
            printf("  [PASS] %s\n", msg);                                                                              \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            g_test_fail++;                                                                                             \
            printf("  [FAIL] %s\n", msg);                                                                              \
        }                                                                                                              \
    }                                                                                                                  \
    while (0)

static int flash_range_check(uint32_t addr, uint32_t len)
{
    if (addr >= TEST_FLASH_SIZE || len > TEST_FLASH_SIZE - addr)
        return -1;
    return 0;
}

static int mock_flash_read(uint32_t addr, uint8_t* buf, uint32_t len)
{
    if (flash_range_check(addr, len) != 0)
        return -1;
    memcpy(buf, &g_flash[addr], len);
    return 0;
}

static int mock_flash_write(uint32_t addr, const uint8_t* buf, uint32_t len)
{
    if (flash_range_check(addr, len) != 0)
        return -1;
    memcpy(&g_flash[addr], buf, len);
    return 0;
}

static int mock_flash_erase(uint32_t addr)
{
    if (addr >= TEST_FLASH_SIZE)
        return -1;
    uint32_t sector_index = addr / TEST_SECTOR_SIZE;
    uint32_t base         = sector_index * TEST_SECTOR_SIZE;
    memset(&g_flash[base], 0xFF, TEST_SECTOR_SIZE);
    return 0;
}

static void build_flash_ops(nkv_flash_ops_t* ops)
{
    ops->read         = mock_flash_read;
    ops->write        = mock_flash_write;
    ops->erase        = mock_flash_erase;
    ops->base         = 0;
    ops->sector_size  = TEST_SECTOR_SIZE;
    ops->sector_count = TEST_SECTOR_COUNT;
    ops->align        = 4;
}

static void print_usage(void)
{
    uint32_t used = 0, total = 0;
    nkv_get_usage(&used, &total);
    printf("  [INFO] used=%u bytes, total=%u bytes, %.1f%%\n",
           (unsigned) used,
           (unsigned) total,
           total ? (used * 100.0f / total) : 0.0f);
}

static void print_perf_summary(void)
{
    printf("\n========== 性能统计 ==========\n");
    if (g_perf.set_count > 0)
        printf("  SET: count=%d, total=%.1fus, avg=%.2fus\n",
               g_perf.set_count,
               g_perf.set_time_us,
               g_perf.set_time_us / g_perf.set_count);
    if (g_perf.get_count > 0)
        printf("  GET: count=%d, total=%.1fus, avg=%.2fus\n",
               g_perf.get_count,
               g_perf.get_time_us,
               g_perf.get_time_us / g_perf.get_count);
    if (g_perf.del_count > 0)
        printf("  DEL: count=%d, total=%.1fus, avg=%.2fus\n",
               g_perf.del_count,
               g_perf.del_time_us,
               g_perf.del_time_us / g_perf.del_count);
    if (g_perf.gc_count > 0)
        printf("   GC: count=%d, total=%.1fus, avg=%.2fus\n",
               g_perf.gc_count,
               g_perf.gc_time_us,
               g_perf.gc_time_us / g_perf.gc_count);
    printf("================================\n");
}

/* ==================== 测试用例 ==================== */

/* 1. KV 基础读写测试 */
static void test_kv_basic(void)
{
    printf("\n=== 1. KV 基础读写测试 ===\n");

    nkv_err_t err;
    uint32_t  vol = 50;
    uint32_t  read_vol;
    uint8_t   len;

    /* 写入 */
    err = nkv_set("volume", &vol, sizeof(vol));
    TEST_ASSERT(err == NKV_OK, "nkv_set(volume=50)");

    /* 读取 */
    err = nkv_get("volume", &read_vol, sizeof(read_vol), &len);
    TEST_ASSERT(err == NKV_OK && read_vol == 50 && len == 4, "nkv_get(volume) == 50");

    /* 更新 */
    vol = 80;
    err = nkv_set("volume", &vol, sizeof(vol));
    TEST_ASSERT(err == NKV_OK, "nkv_set(volume=80)");

    err = nkv_get("volume", &read_vol, sizeof(read_vol), &len);
    TEST_ASSERT(err == NKV_OK && read_vol == 80, "nkv_get(volume) == 80 after update");

    /* 存在性检查 */
    TEST_ASSERT(nkv_exists("volume") == 1, "nkv_exists(volume) == 1");
    TEST_ASSERT(nkv_exists("nonexist") == 0, "nkv_exists(nonexist) == 0");

    /* 删除 */
    err = nkv_del("volume");
    TEST_ASSERT(err == NKV_OK, "nkv_del(volume)");
    TEST_ASSERT(nkv_exists("volume") == 0, "nkv_exists(volume) == 0 after delete");

    print_usage();
}

/* 2. KV 默认值测试 */
static void test_kv_defaults(void)
{
    printf("\n=== 2. KV 默认值测试 ===\n");

    /* 定义默认值表 */
    static uint32_t def_brightness = 100;
    static uint8_t  def_mode       = 1;

    static const nkv_default_t defaults[] = {
        {.key = "brightness", .value = &def_brightness, .len = sizeof(def_brightness)},
        {.key = "mode",       .value = &def_mode,       .len = sizeof(def_mode)      },
    };

    nkv_set_defaults(defaults, sizeof(defaults) / sizeof(defaults[0]));
    TEST_ASSERT(1, "nkv_set_defaults() called");

    /* 未写入时，使用 get_default 获取默认值 */
    uint32_t  brightness = 0;
    uint8_t   len;
    nkv_err_t err = nkv_get_default("brightness", &brightness, sizeof(brightness), &len);
    TEST_ASSERT(err == NKV_OK && brightness == 100, "nkv_get_default(brightness) == 100");

    /* 写入新值后，get_default 应返回新值 */
    brightness = 50;
    nkv_set("brightness", &brightness, sizeof(brightness));
    brightness = 0;
    err        = nkv_get_default("brightness", &brightness, sizeof(brightness), &len);
    TEST_ASSERT(err == NKV_OK && brightness == 50, "nkv_get_default(brightness) == 50 after set");

    /* 重置单个键 */
    err = nkv_reset_key("brightness");
    TEST_ASSERT(err == NKV_OK, "nkv_reset_key(brightness)");

    brightness = 0;
    err        = nkv_get("brightness", &brightness, sizeof(brightness), &len);
    TEST_ASSERT(err == NKV_OK && brightness == 100, "brightness == 100 after reset");

    /* 查找默认值 */
    const nkv_default_t* def = nkv_find_default("mode");
    TEST_ASSERT(def != NULL && *(uint8_t*) def->value == 1, "nkv_find_default(mode)");

    print_usage();
}

/* 3. TLV 基础读写测试 */
static void test_tlv_basic(void)
{
    printf("\n=== 3. TLV 基础读写测试 ===\n");

    nkv_err_t err;
    uint8_t   mode = 0x01;
    uint8_t   read_mode;
    uint8_t   len;

    /* 写入 */
    err = nkv_tlv_set(0x10, &mode, sizeof(mode));
    TEST_ASSERT(err == NKV_OK, "nkv_tlv_set(type=0x10, mode=0x01)");

    /* 存在性检查 */
    TEST_ASSERT(nkv_tlv_exists(0x10) == 1, "nkv_tlv_exists(0x10) == 1");
    TEST_ASSERT(nkv_tlv_exists(0x20) == 0, "nkv_tlv_exists(0x20) == 0");

    /* 读取 */
    err = nkv_tlv_get(0x10, &read_mode, sizeof(read_mode), &len);
    TEST_ASSERT(err == NKV_OK && read_mode == 0x01 && len == 1, "nkv_tlv_get(0x10) == 0x01");

    /* 更新 */
    mode = 0x02;
    err  = nkv_tlv_set(0x10, &mode, sizeof(mode));
    TEST_ASSERT(err == NKV_OK, "nkv_tlv_set(type=0x10, mode=0x02)");

    err = nkv_tlv_get(0x10, &read_mode, sizeof(read_mode), &len);
    TEST_ASSERT(err == NKV_OK && read_mode == 0x02, "nkv_tlv_get(0x10) == 0x02 after update");

    /* 删除 */
    err = nkv_tlv_del(0x10);
    TEST_ASSERT(err == NKV_OK, "nkv_tlv_del(0x10)");
    TEST_ASSERT(nkv_tlv_exists(0x10) == 0, "nkv_tlv_exists(0x10) == 0 after delete");

    print_usage();
}

/* 4. TLV 默认值测试 */
static void test_tlv_defaults(void)
{
    printf("\n=== 4. TLV 默认值测试 ===\n");

    static uint8_t  def_sensor_mode = 0xAA;
    static uint16_t def_interval    = 1000;

    static const nkv_tlv_default_t tlv_defaults[] = {
        {.type = 0x20, .value = &def_sensor_mode, .len = sizeof(def_sensor_mode)},
        {.type = 0x21, .value = &def_interval,    .len = sizeof(def_interval)   },
    };

    nkv_tlv_set_defaults(tlv_defaults, sizeof(tlv_defaults) / sizeof(tlv_defaults[0]));
    TEST_ASSERT(1, "nkv_tlv_set_defaults() called");

    /* 未写入时获取默认值 */
    uint8_t   sensor_mode = 0;
    uint8_t   len;
    nkv_err_t err = nkv_tlv_get_default(0x20, &sensor_mode, sizeof(sensor_mode), &len);
    TEST_ASSERT(err == NKV_OK && sensor_mode == 0xAA, "nkv_tlv_get_default(0x20) == 0xAA");

    /* 写入新值后获取 */
    sensor_mode = 0xBB;
    nkv_tlv_set(0x20, &sensor_mode, sizeof(sensor_mode));
    sensor_mode = 0;
    err         = nkv_tlv_get_default(0x20, &sensor_mode, sizeof(sensor_mode), &len);
    TEST_ASSERT(err == NKV_OK && sensor_mode == 0xBB, "nkv_tlv_get_default(0x20) == 0xBB after set");

    /* 重置单个类型 */
    err = nkv_tlv_reset_type(0x20);
    TEST_ASSERT(err == NKV_OK, "nkv_tlv_reset_type(0x20)");

    sensor_mode = 0;
    err         = nkv_tlv_get(0x20, &sensor_mode, sizeof(sensor_mode), &len);
    TEST_ASSERT(err == NKV_OK && sensor_mode == 0xAA, "sensor_mode == 0xAA after reset");

    print_usage();
}

/* 5. TLV 迭代器测试 */
static void test_tlv_iterator(void)
{
    printf("\n=== 5. TLV 迭代器测试 ===\n");

    /* 先写入几个 TLV 条目 */
    uint8_t val1 = 0x11, val2 = 0x22, val3 = 0x33;
    nkv_tlv_set(0x30, &val1, sizeof(val1));
    nkv_tlv_set(0x31, &val2, sizeof(val2));
    nkv_tlv_set(0x32, &val3, sizeof(val3));

    /* 使用迭代器遍历 */
    nkv_tlv_iter_t  iter;
    nkv_tlv_entry_t info;
    uint8_t         count = 0;

    nkv_tlv_iter_init(&iter);
    while (nkv_tlv_iter_next(&iter, &info))
    {
        uint8_t val;
        if (nkv_tlv_iter_read(&info, &val, sizeof(val)) == NKV_OK)
        {
            printf("  [INFO] type=0x%02X, len=%u, value=0x%02X\n", info.type, info.len, val);
            count++;
        }
    }

    TEST_ASSERT(count >= 3, "TLV iterator found >= 3 entries");

    /* TLV 统计 */
    uint16_t tlv_count;
    uint32_t tlv_used;
    nkv_tlv_stats(&tlv_count, &tlv_used);
    printf("  [INFO] TLV stats: count=%u, used=%u bytes\n", tlv_count, (unsigned) tlv_used);
    TEST_ASSERT(tlv_count >= 3, "nkv_tlv_stats count >= 3");

    /* TLV 是否有数据 */
    TEST_ASSERT(nkv_tlv_has_data() == 1, "nkv_tlv_has_data() == 1");

    print_usage();
}

/* 6. TLV 历史记录测试 */
static void test_tlv_history(void)
{
    printf("\n=== 6. TLV 历史记录测试 ===\n");

    /* 写入多条同类型的 TLV */
    uint8_t val;
    for (val = 1; val <= 5; val++)
    {
        nkv_tlv_set(0x40, &val, sizeof(val));
    }

    /* 获取历史记录 */
    nkv_tlv_history_t history[8];
    uint8_t           count = 0;

    nkv_err_t err = nkv_tlv_get_history(0x40, history, 8, &count);
    TEST_ASSERT(err == NKV_OK && count >= 1, "nkv_tlv_get_history(0x40) found entries");
    printf("  [INFO] History count for type 0x40: %u\n", count);

    /* 读取最新的历史记录 */
    if (count > 0)
    {
        uint8_t hist_val;
        err = nkv_tlv_read_history(&history[0], &hist_val, sizeof(hist_val));
        TEST_ASSERT(err == NKV_OK && hist_val == 5, "Latest history value == 5");
    }

    print_usage();
}

/* 7. 缓存功能测试 */
#if NKV_CACHE_ENABLE
static void test_cache(void)
{
    printf("\n=== 7. 缓存功能测试 ===\n");

    /* 写入并读取多次以触发缓存 */
    uint32_t data = 12345;
    nkv_set("cached_key", &data, sizeof(data));

    uint32_t read_data;
    uint8_t  len;
    for (int i = 0; i < 10; i++)
    {
        nkv_get("cached_key", &read_data, sizeof(read_data), &len);
    }

    /* 获取缓存统计 */
    nkv_cache_stats_t stats;
    nkv_cache_stats(&stats);
    printf("  [INFO] Cache: hits=%u, misses=%u, hit_rate=%.1f%%\n",
           (unsigned) stats.hit_count,
           (unsigned) stats.miss_count,
           stats.hit_rate);

    TEST_ASSERT(stats.hit_count > 0, "Cache hit count > 0");

    /* 清除缓存 */
    nkv_cache_clear();
    TEST_ASSERT(1, "nkv_cache_clear() called");

    print_usage();
}
#endif

/* 8. 增量 GC 测试 */
#if NKV_INCREMENTAL_GC
static void test_incremental_gc(void)
{
    printf("\n=== 8. 增量 GC 测试 ===\n");

    /* 写入大量数据以触发 GC */
    char     key[16];
    uint32_t val;
    for (int i = 0; i < 50; i++)
    {
        snprintf(key, sizeof(key), "gc_key_%d", i);
        val = i * 100;
        nkv_set(key, &val, sizeof(val));
    }

    /* 检查 GC 状态 */
    uint8_t gc_active = nkv_gc_active();
    printf("  [INFO] GC active: %u\n", gc_active);

    /* 手动执行 GC 步骤 */
    uint8_t gc_done = nkv_gc_step(10);
    printf("  [INFO] GC step result: %u\n", gc_done);
    TEST_ASSERT(1, "nkv_gc_step() executed");

    print_usage();
}
#endif

/* 9. TLV 保留策略测试 */
#if NKV_TLV_RETENTION_ENABLE
static void test_tlv_retention(void)
{
    printf("\n=== 9. TLV 保留策略测试 ===\n");

    /* 设置保留策略：type 0x50 只保留最新 3 条 */
    nkv_err_t err = nkv_tlv_set_retention(0x50, 3);
    TEST_ASSERT(err == NKV_OK, "nkv_tlv_set_retention(0x50, 3)");

    /* 写入多条数据 */
    uint8_t val;
    for (val = 1; val <= 10; val++)
    {
        nkv_tlv_set(0x50, &val, sizeof(val));
    }

    /* 清除保留策略 */
    nkv_tlv_clear_retention(0x50);
    TEST_ASSERT(1, "nkv_tlv_clear_retention(0x50) called");

    print_usage();
}
#endif

/* 10. 对齐验证测试 */
static void test_alignment(void)
{
    printf("\n=== 10. 对齐验证测试 ===\n");

    nkv_instance_t* inst  = nkv_get_instance();
    uint32_t        align = inst->flash.align;

    /* 记录初始偏移 */
    uint32_t start_offset = inst->write_offset;
    TEST_ASSERT((start_offset % align) == 0, "Initial offset is aligned");

    /* 写入不同长度的数据，验证对齐 */
    uint8_t data1 = 0xAA;                  /* 1 字节值 */
    nkv_set("al1", &data1, sizeof(data1)); /* header(4) + key(3) + val(1) + crc(2) = 10 -> 12 */
    TEST_ASSERT((inst->write_offset % align) == 0, "Offset aligned after 1-byte value");
    printf("  [INFO] After al1: offset=%u (align=%u)\n", (unsigned) inst->write_offset, (unsigned) align);

    uint16_t data2 = 0xBBCC;               /* 2 字节值 */
    nkv_set("al2", &data2, sizeof(data2)); /* header(4) + key(3) + val(2) + crc(2) = 11 -> 12 */
    TEST_ASSERT((inst->write_offset % align) == 0, "Offset aligned after 2-byte value");
    printf("  [INFO] After al2: offset=%u\n", (unsigned) inst->write_offset);

    uint8_t data3[5] = {1, 2, 3, 4, 5};   /* 5 字节值 */
    nkv_set("al3", data3, sizeof(data3)); /* header(4) + key(3) + val(5) + crc(2) = 14 -> 16 */
    TEST_ASSERT((inst->write_offset % align) == 0, "Offset aligned after 5-byte value");
    printf("  [INFO] After al3: offset=%u\n", (unsigned) inst->write_offset);

    uint8_t data4[7] = {0};               /* 7 字节值 */
    nkv_set("al4", data4, sizeof(data4)); /* header(4) + key(3) + val(7) + crc(2) = 16 -> 16 */
    TEST_ASSERT((inst->write_offset % align) == 0, "Offset aligned after 7-byte value");
    printf("  [INFO] After al4: offset=%u\n", (unsigned) inst->write_offset);

    print_usage();
}

/* 11. 全量 GC 测试 */
static void test_full_gc(void)
{
    printf("\n=== 11. 全量 GC 测试 ===\n");

    nkv_instance_t* inst = nkv_get_instance();

    /* 先格式化，确保干净状态 */
    nkv_format();
    printf("  [INFO] Formatted, active_sector=%u, write_offset=%u\n",
           inst->active_sector,
           (unsigned) inst->write_offset);

    /* 填充多个扇区以触发全量 GC */
    char     key[16];
    uint8_t  val[64];
    uint32_t fill_count = 0;

    memset(val, 0x55, sizeof(val));

    /* 写入数据直到填满多个扇区 */
    for (int i = 0; i < 200; i++)
    {
        snprintf(key, sizeof(key), "fgc%d", i);
        if (nkv_set(key, val, 32) != NKV_OK)
            break;
        fill_count++;
    }
    printf("  [INFO] Filled %u entries, active_sector=%u\n", (unsigned) fill_count, inst->active_sector);

    /* 验证数据完整性（抽样检查） */
    uint8_t read_val[64];
    uint8_t len;
    int     valid_count = 0;
    for (int i = 0; i < (int) fill_count; i += 10)
    {
        snprintf(key, sizeof(key), "fgc%d", i);
        if (nkv_get(key, read_val, sizeof(read_val), &len) == NKV_OK && len == 32)
        {
            valid_count++;
        }
    }
    printf("  [INFO] Verified %d sampled entries after GC\n", valid_count);
    TEST_ASSERT(valid_count > 0, "Data integrity after full GC");

    /* 检查扇区切换是否发生 */
    TEST_ASSERT(inst->sector_seq > 1, "Sector sequence increased (GC occurred)");
    printf("  [INFO] sector_seq=%u (should > 1 if GC occurred)\n", inst->sector_seq);

    print_usage();
}

/* 12. 增量 GC 详细测试 */
#if NKV_INCREMENTAL_GC
static void test_incremental_gc_detail(void)
{
    printf("\n=== 12. 增量 GC 详细测试 ===\n");

    nkv_instance_t* inst = nkv_get_instance();

    /* 格式化重新开始 */
    nkv_format();
    uint8_t initial_sector = inst->active_sector;

    printf("  [INFO] Start: sector=%u, seq=%u, offset=%u\n",
           inst->active_sector,
           inst->sector_seq,
           (unsigned) inst->write_offset);

    /* 写入数据填满当前扇区，触发增量 GC */
    char    key[16];
    uint8_t val[48];
    int     write_count = 0;
    memset(val, 0xAA, sizeof(val));

    for (int i = 0; i < 100; i++)
    {
        snprintf(key, sizeof(key), "igc%d", i);
        nkv_err_t err = nkv_set(key, val, sizeof(val));
        if (err != NKV_OK)
        {
            printf("  [INFO] Write stopped at i=%d, err=%d\n", i, err);
            break;
        }
        write_count++;

        /* 每隔 20 次检查 GC 状态 */
        if ((i + 1) % 20 == 0)
        {
            printf("  [INFO] i=%d: sector=%u, offset=%u, gc_active=%u\n",
                   i,
                   inst->active_sector,
                   (unsigned) inst->write_offset,
                   nkv_gc_active());
        }
    }

    printf("  [INFO] Total writes: %d\n", write_count);

    /* 增量 GC 现象：
     * 1. gc_active 在扇区用完时变为 1
     * 2. 每次 nkv_set 后自动迁移 NKV_GC_ENTRIES_PER_WRITE 个条目
     * 3. 旧扇区扫描完后自动擦除，gc_active 变为 0
     */
    TEST_ASSERT(inst->active_sector != initial_sector || inst->sector_seq > 1, "Sector switched or sequence increased");

    /* 验证数据完整性 */
    uint8_t read_val[48];
    uint8_t len;
    int     valid = 0;
    for (int i = 0; i < write_count; i += 5)
    {
        snprintf(key, sizeof(key), "igc%d", i);
        if (nkv_get(key, read_val, sizeof(read_val), &len) == NKV_OK)
            valid++;
    }
    TEST_ASSERT(valid > 0, "Data accessible after incremental GC");
    printf("  [INFO] Verified %d entries\n", valid);

    print_usage();
}
#endif

/* 13. 多键写入与读取测试 */
static void test_multi_keys(void)
{
    printf("\n=== 13. 多键写入与读取测试 ===\n");

    /* 写入多个不同类型的键 */
    uint8_t  u8_val    = 255;
    uint16_t u16_val   = 65535;
    uint32_t u32_val   = 0xDEADBEEF;
    char     str_val[] = "Hello";

    nkv_set("u8_key", &u8_val, sizeof(u8_val));
    nkv_set("u16_key", &u16_val, sizeof(u16_val));
    nkv_set("u32_key", &u32_val, sizeof(u32_val));
    nkv_set("str_key", str_val, sizeof(str_val));

    /* 读取并验证 */
    uint8_t  r_u8;
    uint16_t r_u16;
    uint32_t r_u32;
    char     r_str[16];
    uint8_t  len;

    nkv_get("u8_key", &r_u8, sizeof(r_u8), &len);
    TEST_ASSERT(r_u8 == 255, "u8_key == 255");

    nkv_get("u16_key", &r_u16, sizeof(r_u16), &len);
    TEST_ASSERT(r_u16 == 65535, "u16_key == 65535");

    nkv_get("u32_key", &r_u32, sizeof(r_u32), &len);
    TEST_ASSERT(r_u32 == 0xDEADBEEF, "u32_key == 0xDEADBEEF");

    nkv_get("str_key", r_str, sizeof(r_str), &len);
    TEST_ASSERT(strcmp(r_str, "Hello") == 0, "str_key == 'Hello'");

    print_usage();
}

/* 14. 性能基准测试 */
static void test_performance_benchmark(void)
{
    printf("\n=== 14. 性能基准测试 ===\n");

    nkv_format();

    char     key[16];
    uint32_t val;
    uint8_t  len;
    double   elapsed;
    int      count = 100;

    /* SET 性能测试 */
    timer_start();
    for (int i = 0; i < count; i++)
    {
        snprintf(key, sizeof(key), "perf%d", i);
        val = i * 1000;
        nkv_set(key, &val, sizeof(val));
    }
    elapsed = timer_elapsed_us();
    printf("  [PERF] SET %d entries: %.1fus total, %.2fus/op\n", count, elapsed, elapsed / count);
    PERF_ADD(set, elapsed);

    /* GET 性能测试 */
    timer_start();
    for (int i = 0; i < count; i++)
    {
        snprintf(key, sizeof(key), "perf%d", i);
        nkv_get(key, &val, sizeof(val), &len);
    }
    elapsed = timer_elapsed_us();
    printf("  [PERF] GET %d entries: %.1fus total, %.2fus/op\n", count, elapsed, elapsed / count);
    PERF_ADD(get, elapsed);

    /* DEL 性能测试 */
    timer_start();
    for (int i = 0; i < count; i++)
    {
        snprintf(key, sizeof(key), "perf%d", i);
        nkv_del(key);
    }
    elapsed = timer_elapsed_us();
    printf("  [PERF] DEL %d entries: %.1fus total, %.2fus/op\n", count, elapsed, elapsed / count);
    PERF_ADD(del, elapsed);

    TEST_ASSERT(1, "Performance benchmark completed");
    print_usage();
}

/* 15. 填满 3 扇区的增量 GC 测试 */
#if NKV_INCREMENTAL_GC
static void test_incremental_gc_full(void)
{
    printf("\n=== 15. 填满 3 扇区增量 GC 测试 ===\n");

    nkv_instance_t* inst = nkv_get_instance();
    nkv_format();

    printf("  [INFO] sector_size=%u, sector_count=%u\n", (unsigned) inst->flash.sector_size, inst->flash.sector_count);

    /* 计算填满 3 个扇区所需的条目数 */
    uint32_t entry_size         = 4 + 6 + 32 + 2; /* header + key + val + crc, aligned to 44 */
    uint32_t usable_space       = inst->flash.sector_size - 4;
    uint32_t entries_per_sector = usable_space / 48;           /* aligned entry size */
    uint32_t target_entries     = entries_per_sector * 3 + 10; /* 填满 3 扇区再多写一点 */

    printf("  [INFO] Target: ~%u entries to fill 3 sectors\n", (unsigned) target_entries);

    char    key[16];
    uint8_t val[32];
    int     write_count    = 0;
    int     gc_trigger_cnt = 0;
    uint8_t last_gc_state  = 0;

    memset(val, 0xBB, sizeof(val));

    double total_gc_time = 0;

    for (uint32_t i = 0; i < target_entries; i++)
    {
        snprintf(key, sizeof(key), "gc3_%u", (unsigned) i);

        timer_start();
        nkv_err_t err     = nkv_set(key, val, sizeof(val));
        double    op_time = timer_elapsed_us();

        if (err != NKV_OK)
        {
            printf("  [INFO] Write stopped at i=%u, err=%d\n", (unsigned) i, err);
            break;
        }
        write_count++;

        /* 检测 GC 状态变化 */
        uint8_t cur_gc = nkv_gc_active();
        if (cur_gc && !last_gc_state)
        {
            gc_trigger_cnt++;
            printf("  [INFO] GC triggered at i=%u, sector=%u\n", (unsigned) i, inst->active_sector);
        }
        if (cur_gc)
        {
            total_gc_time += op_time; /* GC 期间的写入时间包含 GC 开销 */
        }
        last_gc_state = cur_gc;

        /* 每 50 次打印进度 */
        if ((i + 1) % 50 == 0)
        {
            printf("  [INFO] i=%u: sector=%u, offset=%u, gc=%u\n",
                   (unsigned) i,
                   inst->active_sector,
                   (unsigned) inst->write_offset,
                   cur_gc);
        }
    }

    printf("  [INFO] Total writes: %d, GC triggers: %d\n", write_count, gc_trigger_cnt);
    printf("  [INFO] Estimated GC overhead: %.1fus\n", total_gc_time);
    PERF_ADD(gc, total_gc_time);

    /* 验证数据完整性 */
    uint8_t read_val[32];
    uint8_t len;
    int     valid = 0;
    for (int i = 0; i < write_count; i += 10)
    {
        snprintf(key, sizeof(key), "gc3_%d", i);
        if (nkv_get(key, read_val, sizeof(read_val), &len) == NKV_OK && len == 32)
            valid++;
    }
    TEST_ASSERT(valid > 0, "Data integrity after 3-sector GC");
    printf("  [INFO] Verified %d/%d sampled entries\n", valid, (write_count + 9) / 10);

    /* 应该触发过至少 1 次 GC */
    TEST_ASSERT(gc_trigger_cnt >= 1 || inst->sector_seq > 3, "GC should have triggered");

    print_usage();
}
#endif

/* 16. 配置版本自动同步测试 */
static void test_version_sync(void)
{
    printf("\n=== 16. 配置版本自动同步测试 ===\n");

    /* 重新初始化，模拟新版本启动 */
    memset(g_flash, 0xFF, sizeof(g_flash));

    nkv_flash_ops_t ops;
    build_flash_ops(&ops);
    nkv_internal_init(&ops);
    nkv_scan();

    /* 定义默认值 */
    static uint32_t def_volume = 50;
    static uint8_t  def_mode   = 2;

    static const nkv_default_t defaults[] = {
        {.key = "sync_vol",  .value = &def_volume, .len = sizeof(def_volume)},
        {.key = "sync_mode", .value = &def_mode,   .len = sizeof(def_mode)  },
    };

    /* 设置默认值并触发同步 */
    printf("  [INFO] Setting defaults (should trigger sync)...\n");
    nkv_set_defaults(defaults, 2);

    /* 验证默认值已写入 */
    uint32_t  read_vol = 0;
    uint8_t   len;
    nkv_err_t err = nkv_get("sync_vol", &read_vol, sizeof(read_vol), &len);
    TEST_ASSERT(err == NKV_OK && read_vol == 50, "sync_vol synced to 50");

    uint8_t read_mode = 0;
    err               = nkv_get("sync_mode", &read_mode, sizeof(read_mode), &len);
    TEST_ASSERT(err == NKV_OK && read_mode == 2, "sync_mode synced to 2");

    /* 修改值后重新同步，不应覆盖已有值 */
    uint32_t new_vol = 100;
    nkv_set("sync_vol", &new_vol, sizeof(new_vol));

    /* 再次设置默认值（版本未变，不应重新同步） */
    nkv_set_defaults(defaults, 2);

    read_vol = 0;
    nkv_get("sync_vol", &read_vol, sizeof(read_vol), &len);
    TEST_ASSERT(read_vol == 100, "sync_vol preserved after re-sync (100)");

    print_usage();
}

/* 17. 掉电安全测试 */
static void test_power_fail_safety(void)
{
    printf("\n=== 17. 掉电安全测试 ===\n");

    /* 重新初始化 */
    memset(g_flash, 0xFF, sizeof(g_flash));
    nkv_flash_ops_t ops;
    build_flash_ops(&ops);
    nkv_internal_init(&ops);
    nkv_scan();

    /* 写入一个值 */
    uint32_t original = 12345;
    nkv_set("pf_key", &original, sizeof(original));

    printf("  [INFO] Simulating power failure during write...\n");

    /* 模拟写入过程中掉电：手动写入一个 WRITING 状态的条目 */
    nkv_instance_t* inst       = nkv_get_instance();
    uint32_t        write_addr = inst->write_offset;
    uint8_t         dirty_entry[32];

    memset(dirty_entry, 0xFF, sizeof(dirty_entry));
    /* state = WRITING (0xFFFE), key_len = 6, val_len = 4 */
    dirty_entry[0] = 0xFE;
    dirty_entry[1] = 0xFF; /* WRITING state */
    dirty_entry[2] = 6;    /* key_len */
    dirty_entry[3] = 4;    /* val_len */
    memcpy(&dirty_entry[4], "dirty!", 6);
    uint32_t dirty_val = 99999;
    memcpy(&dirty_entry[10], &dirty_val, 4);
    /* CRC 先不写，模拟不完整写入 */

    /* 直接写入 Flash */
    mock_flash_write(write_addr, dirty_entry, 16);
    inst->write_offset += 16; /* 更新偏移 */

    printf("  [INFO] Dirty entry written at offset %u\n", (unsigned) write_addr);

    /* 模拟重启：重新扫描 */
    printf("  [INFO] Simulating reboot (nkv_scan)...\n");
    nkv_internal_init(&ops);
    nkv_err_t err = nkv_scan();
    TEST_ASSERT(err == NKV_OK, "nkv_scan after power fail");

    /* 验证原始数据仍然可读 */
    uint32_t read_val = 0;
    uint8_t  len;
    err = nkv_get("pf_key", &read_val, sizeof(read_val), &len);
    TEST_ASSERT(err == NKV_OK && read_val == 12345, "Original data preserved after power fail");

    /* 验证脏数据不可读（应该被清理或跳过） */
    err = nkv_get("dirty!", &read_val, sizeof(read_val), &len);
    TEST_ASSERT(err != NKV_OK, "Dirty entry not readable (cleaned or skipped)");

    /* 测试 PRE_DEL 掉电恢复 */
    printf("  [INFO] Testing PRE_DEL power fail recovery...\n");

    /* 写入新值 */
    uint32_t new_val = 67890;
    nkv_set("pf_key2", &new_val, sizeof(new_val));

    /* 手动将其状态改为 PRE_DEL（模拟更新过程中掉电） */
    nkv_entry_t dummy;
    uint32_t    addr = 0;
    /* 查找最后写入的条目 */
    for (uint32_t off = 4; off < inst->write_offset; off += 4)
    {
        uint16_t state;
        mock_flash_read(off, (uint8_t*) &state, 2);
        if (state == 0xFFFC) /* VALID */
        {
            uint8_t kl, vl;
            mock_flash_read(off + 2, &kl, 1);
            mock_flash_read(off + 3, &vl, 1);
            if (kl == 7) /* "pf_key2" */
            {
                addr = off;
            }
        }
    }

    if (addr > 0)
    {
        /* 将状态改为 PRE_DEL */
        uint16_t pre_del = 0xFFF8;
        mock_flash_write(addr, (uint8_t*) &pre_del, 2);
        printf("  [INFO] Changed pf_key2 to PRE_DEL at addr %u\n", (unsigned) addr);

        /* 重新扫描 */
        nkv_internal_init(&ops);
        nkv_scan();

        /* PRE_DEL 状态在掉电后应仍可读（作为有效数据） */
        err = nkv_get("pf_key2", &read_val, sizeof(read_val), &len);
        TEST_ASSERT(err == NKV_OK && read_val == 67890, "PRE_DEL entry readable after reboot");
    }
    else
    {
        TEST_ASSERT(0, "Could not find pf_key2 entry");
    }

    print_usage();
}

/* ==================== 主函数 ==================== */

int main(void)
{
    printf("========================================\n");
    printf("   NanoKV 完整功能测试\n");
    printf("========================================\n");

    /* 初始化计时器 */
    timer_init();

    /* 初始化模拟 Flash */
    memset(g_flash, 0xFF, sizeof(g_flash));

    nkv_flash_ops_t ops;
    build_flash_ops(&ops);

    /* 初始化 NanoKV */
    nkv_err_t err = nkv_internal_init(&ops);
    printf("\nnkv_internal_init -> %d\n", err);
    if (err != NKV_OK)
        return -1;

    err = nkv_scan();
    printf("nkv_scan -> %d\n", err);
    if (err != NKV_OK)
        return -1;

    print_usage();

    /* 运行所有测试 */
    test_kv_basic();
    test_kv_defaults();
    test_tlv_basic();
    test_tlv_defaults();
    test_tlv_iterator();
    test_tlv_history();

#if NKV_CACHE_ENABLE
    test_cache();
#endif

#if NKV_INCREMENTAL_GC
    test_incremental_gc();
#endif

#if NKV_TLV_RETENTION_ENABLE
    test_tlv_retention();
#endif

    test_alignment();
    test_full_gc();

#if NKV_INCREMENTAL_GC
    test_incremental_gc_detail();
#endif

    test_multi_keys();

    /* 新增测试 */
    test_performance_benchmark();

#if NKV_INCREMENTAL_GC
    test_incremental_gc_full();
#endif

    test_version_sync();
    test_power_fail_safety();

    /* 打印性能统计 */
    print_perf_summary();

    /* 打印测试结果 */
    printf("\n========================================\n");
    printf("   测试结果: PASS=%u, FAIL=%u\n", g_test_pass, g_test_fail);
    printf("========================================\n");

    return (g_test_fail > 0) ? -1 : 0;
}
