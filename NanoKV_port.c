/**
 * @file NanoKV_port.c
 * @brief NanoKV移植层实现
 * @note 实现Flash读写擦除操作
 */

#include "NanoKV_port.h"

#include "NanoKV.h"


/* Flash配置 - STM32F407 kv分区 */
#define NKV_FLASH_BASE   0x08080000   /* Flash起始地址 */
#define NKV_SECTOR_SIZE  (128 * 1024) /* 扇区大小(128KB) */
#define NKV_SECTOR_COUNT 4            /* 扇区数量 */
#define NKV_FLASH_SIZE   (NKV_SECTOR_SIZE * NKV_SECTOR_COUNT)

/* Flash读取 */
static int flash_read_impl(uint32_t addr, uint8_t* buf, uint32_t len)
{
    uint32_t offset = addr - NKV_FLASH_BASE;

    return 0;  // 需根据具体 Flash 接口实现
}

/* Flash写入 */
static int flash_write_impl(uint32_t addr, const uint8_t* buf, uint32_t len)
{
    uint32_t offset = addr - NKV_FLASH_BASE;

    return 0;  // 需根据具体 Flash 接口实现
}

/* Flash擦除 */
static int flash_erase_impl(uint32_t addr)
{
    uint32_t offset = addr - NKV_FLASH_BASE;

    return 0;  // 需根据具体 Flash 接口实现
}

/* Flash操作配置 */
static const nkv_flash_ops_t g_flash_ops = {
    .read         = flash_read_impl,
    .write        = flash_write_impl,
    .erase        = flash_erase_impl,
    .base         = NKV_FLASH_BASE,
    .sector_size  = NKV_SECTOR_SIZE,
    .sector_count = NKV_SECTOR_COUNT,
    .align        = 4 /* STM32F4需要4字节对齐 */
};

/* 初始化NanoKV */
nkv_err_t nkv_init(void)
{
    NKV_LOG_I("NanoKV initializing...");

    /* 内部初始化 */
    nkv_err_t ret = nkv_internal_init(&g_flash_ops);
    if (ret != NKV_OK)
    {
        NKV_LOG_E("Internal init failed: %d", ret);
        return ret;
    }

    /* 扫描并恢复状态 */
    ret = nkv_scan();
    if (ret != NKV_OK)
    {
        NKV_LOG_E("Scan failed: %d", ret);
        return ret;
    }

    /* 打印状态 */
    uint32_t used, total;
    nkv_get_usage(&used, &total);
    NKV_LOG_I("NanoKV OK! Size: %uKB, Usage: %u/%u (%.1f%%)",
              NKV_FLASH_SIZE / 1024,
              used,
              total,
              (float) used / total * 100.0f);

#if NKV_CACHE_ENABLE
    NKV_LOG_I("Cache: LFU, %d entries", NKV_CACHE_SIZE);
#endif

    return NKV_OK;
}

/* 维护任务 */
void nkv_task(void)
{
    /* 可添加定期维护任务 */
}
