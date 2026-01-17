/**
 * @file MicroKV_port.c
 * @brief MicroKV移植层实现 - 使用FAL抽象层
 * @author liu
 * @date 2025-12-20
 * @version 2.0
 * @copyright Copyright (c) 2025 by liu lbq08@foxmail.com, All Rights Reserved.
 *
 * @details 本文件实现MicroKV的Flash硬件操作层，通过FAL抽象层访问"kv"分区
 */

#include "MicroKV_port.h"

#include "FAL.h"
#include "MicroKV.h"

#include <mlog.h>

/* FAL分区名称 */
#define MKV_FAL_PART_NAME "kv"

/* ===========================================================================
 *                          Flash操作实现区
 * ===========================================================================*/

/**
 * @brief MicroKV Flash读取函数 - 使用FAL读取
 */
static int mkv_flash_read_impl(uint32_t addr, uint8_t* buf, uint32_t len)
{
    /* 计算分区内偏移量 */
    uint32_t offset = addr - MKV_FLASH_BASE;

    if (flash_read(MKV_FAL_PART_NAME, offset, buf, len) != FLASH_OK)
    {
        return -1;
    }
    return 0;
}

/**
 * @brief MicroKV Flash写入函数 - 使用FAL写入
 */
static int mkv_flash_write_impl(uint32_t addr, const uint8_t* buf, uint32_t len)
{
    uint32_t offset = addr - MKV_FLASH_BASE;

    if (flash_write(MKV_FAL_PART_NAME, offset, buf, len) != FLASH_OK)
    {
        return -1;
    }
    return 0;
}

/**
 * @brief MicroKV Flash擦除函数 - 使用FAL擦除
 * @note 擦除一个MKV扇区(128KB)
 */
static int mkv_flash_erase_impl(uint32_t addr)
{
    uint32_t offset = addr - MKV_FLASH_BASE;

    if (flash_erase(MKV_FAL_PART_NAME, offset, MKV_SECTOR_SIZE) != FLASH_OK)
    {
        return -1;
    }
    return 0;
}

/* ===========================================================================
 *                          统一初始化接口
 * ===========================================================================*/

/* Flash操作函数结构体 */
static const MKV_FlashOps_t g_mkv_flash_ops = {
    .read_func    = mkv_flash_read_impl,
    .write_func   = mkv_flash_write_impl,
    .erase_func   = mkv_flash_erase_impl,
    .flash_base   = MKV_FLASH_BASE,
    .sector_size  = MKV_SECTOR_SIZE,
    .sector_count = MKV_SECTOR_COUNT,
    .align_size   = 4 /* STM32F4内部Flash需要4字节对齐 */
};

/**
 * @brief MicroKV 初始化函数
 */
MKV_Error_t mkv_init(void)
{
    log_i("MicroKV initializing...");

    /* 1. 初始化FAL */
    if (flash_init() != FLASH_OK)
    {
        log_e("FAL init failed");
        return MKV_ERR_FLASH;
    }

    /* 2. 检查kv分区是否存在 */
    if (flash_find_part(MKV_FAL_PART_NAME) == NULL)
    {
        log_e("FAL partition '%s' not found", MKV_FAL_PART_NAME);
        return MKV_ERR_INVALID;
    }

    /* 3. 内部初始化 */
    MKV_Error_t result = mkv_internal_init(&g_mkv_flash_ops);
    if (result != MKV_OK)
    {
        log_e("Failed to init MicroKV: %d", result);
        return result;
    }

    /* 4. 扫描并恢复状态 */
    result = mkv_scan();
    if (result != MKV_OK)
    {
        log_e("Failed to scan MicroKV: %d", result);
        return result;
    }

    log_i("MicroKV initialized!");
    log_i("Partition: '%s', Base: 0x%08X, Size: %u KB", MKV_FAL_PART_NAME, MKV_FLASH_BASE, MKV_FLASH_SIZE / 1024);

    uint32_t used, total;
    mkv_get_usage(&used, &total);
    log_i("Usage: %u/%u bytes (%.1f%%)", used, total, (float) used / total * 100.0f);

#if MKV_CACHE_ENABLE
    log_i("Cache: LFU, %d entries", MKV_CACHE_SIZE);
#endif

    return MKV_OK;
}

/**
 * @brief MicroKV维护任务（可选）
 */
void mkv_task(void)
{
    /* 可以添加定期维护任务 */
}
