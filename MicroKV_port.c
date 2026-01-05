/**
 * @file MicroKV_port.c
 * @brief MicroKV移植层实现 - STM32F10x Flash操作 (单实例)
 * @author liu
 * @date 2025-12-20
 * @version 2.0
 * @copyright Copyright (c) 2025 by liu lbq08@foxmail.com, All Rights Reserved.
 *
 * @details 本文件实现MicroKV的Flash硬件操作层，包括：
 *          - Flash读写擦除函数实现
 *          - STM32 Flash特性处理
 *          - 统一初始化接口
 *
 * @note 移植到其他平台时，只需修改本文件中的Flash操作函数即可
 */

#include "MicroKV.h"
#include "MicroKV_port.h"
#include "stm32f10x.h"

#include <mlog.h>

/* ===========================================================================
 *                          Flash操作实现区
 *                   (移植时主要修改这三个函数)
 * ===========================================================================*/

/* STM32F10x Flash页大小 (HD: 2KB, MD: 1KB) */
#define STM32_FLASH_PAGE_SIZE 2048

/**
 * @brief MicroKV Flash读取函数 - 从内部Flash读取数据
 * @param addr Flash地址
 * @param buf 接收缓冲区
 * @param len 数据长度
 * @return int 状态码
 * @retval 0 成功
 * @retval -1 失败
 *
 * @note 移植说明：
 *       - STM32: 直接内存映射读取
 *       - SPI Flash: 需要SPI读取命令
 *       - 模拟Flash: 从数组读取
 */
static int mkv_flash_read_impl(uint32_t addr, uint8_t* buf, uint32_t len)
{
    /* STM32内部Flash直接内存映射读取 */
    for (uint32_t i = 0; i < len; i++)
    {
        buf[i] = *(__IO uint8_t*) (addr + i);
    }
    return 0; /* 成功 */
}

/**
 * @brief MicroKV Flash写入函数 - 向内部Flash写入数据
 * @param addr Flash地址
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return int 状态码
 * @retval 0 成功
 * @retval -1 失败
 *
 * @note 实现要点：
 *       - STM32 Flash需要按半字写入
 *       - 需要解锁/上锁Flash
 *       - 检查地址对齐和范围
 *       - 验证写入结果
 * @warning Flash特性：只能将bit从1变为0，不能从0变为1，写入前必须擦除
 */
static int mkv_flash_write_impl(uint32_t addr, const uint8_t* buf, uint32_t len)
{
    /* 检查地址范围 */
    if (addr < MKV_FLASH_BASE || addr + len > MKV_FLASH_SIZE + MKV_FLASH_BASE)
    {
        log_e("Write addr out of range: 0x%08X, len=%u", addr, len);
        return -1;
    }

    if (!buf || len == 0)
    {
        log_e("Write invalid param: buf=%p, len=%u", buf, len);
        return -1;
    }

    /* 清除Flash错误标志 */
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);

    FLASH_Unlock();

    FLASH_Status   flash_status = FLASH_COMPLETE;
    uint32_t       write_addr   = addr;
    uint32_t       remaining    = len;
    const uint8_t* src_ptr      = buf;

    /* 处理未对齐的起始地址 */
    if (write_addr & 0x01)
    {
        /* 地址未对齐到半字边界，需要特殊处理 */
        FLASH_Lock();
        log_e("Write addr not half-word aligned: 0x%08X", write_addr);
        return -1;
    }

    /* 按半字(16位)写入 */
    while (remaining >= 2)
    {
        uint16_t half_word     = src_ptr[0] | (src_ptr[1] << 8);
        uint16_t current_value = *(volatile uint16_t*) write_addr;

        /* Flash特性：只能将bit从1变为0，不能从0变为1 */
        /* 检查是否需要擦除 */
        if (current_value != 0xFFFF && (current_value & half_word) != half_word)
        {
            log_e("Flash not erased at 0x%08X (cur=0x%04X, new=0x%04X)", write_addr, current_value, half_word);
            log_e("Please call MicroFS_Format() first to erase the Flash area!");
            log_e("Or use MicroFS_Port_EraseAll() to force erase all blocks.");
            FLASH_Lock();
            return -1;
        }

        /* 如果已经是目标值，跳过写入 */
        if (current_value == half_word)
        {
            write_addr += 2;
            src_ptr += 2;
            remaining -= 2;
            continue;
        }

        flash_status = FLASH_ProgramHalfWord(write_addr, half_word);

        if (flash_status != FLASH_COMPLETE)
        {
            log_e("Flash write failed at 0x%08X, status=%d", write_addr, flash_status);
            FLASH_Lock();
            return -1;
        }

        /* 验证写入 */
        if (*(volatile uint16_t*) write_addr != half_word)
        {
            log_e("Flash write verify failed at 0x%08X", write_addr);
            FLASH_Lock();
            return -1;
        }

        write_addr += 2;
        src_ptr += 2;
        remaining -= 2;
    }
    /* 处理剩余的单字节 */
    if (remaining == 1)
    {
        /* 读取当前半字，修改低字节后写回 */
        uint16_t half_word = 0xFFFF;
        if (write_addr < MKV_FLASH_BASE + MKV_FLASH_SIZE)
        {
            half_word    = (0xFF00) | src_ptr[0];
            flash_status = FLASH_ProgramHalfWord(write_addr, half_word);

            if (flash_status != FLASH_COMPLETE)
            {
                log_e("Flash write failed at 0x%08X, status=%d", write_addr, flash_status);
                FLASH_Lock();
                return -1;
            }
        }
    }
    FLASH_Lock();
    return 0;
}

/**
 * @brief MicroKV Flash擦除函数 - 擦除Flash扇区
 * @param addr 扇区起始地址
 * @return int 状态码
 * @retval 0 成功
 * @retval -1 失败
 *
 * @note 实现要点：
 *       - STM32需要按页擦除，一个MKV扇区=多个Flash页
 *       - 需要解锁/上锁Flash
 *       - 检查擦除状态
 */
static int mkv_flash_erase_impl(uint32_t addr)
{
    FLASH_Status status;
    uint32_t     pages = MKV_SECTOR_SIZE / STM32_FLASH_PAGE_SIZE;

    FLASH_Unlock();

    for (uint32_t i = 0; i < pages; i++)
    {
        status = FLASH_ErasePage(addr + i * STM32_FLASH_PAGE_SIZE);
        if (status != FLASH_COMPLETE)
        {
            FLASH_Lock();
            return -1;
        }
    }

    FLASH_Lock();
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
    .align_size   = 2  // STM32内部Flash需要2字节对齐
};

/**
 * @brief MicroKV 初始化函数
 * @return MKV_Error_t 错误码
 * @retval MKV_OK 成功
 * @retval MKV_ERR_INVALID 参数无效
 * @retval MKV_ERR_FLASH Flash操作失败
 *
 * @note 在使用其他 mkv_xxx() 函数前调用
 * @note 此函数会自动扫描 Flash 并恢复状态，如无有效数据则自动格式化
 */
MKV_Error_t mkv_init(void)
{
    log_i("MicroKV initializing...");

    // 1. 内部初始化
    MKV_Error_t result = mkv_internal_init(&g_mkv_flash_ops);
    if (result != MKV_OK)
    {
        log_e("Failed to init MicroKV: %d", result);
        return result;
    }

    // 2. 扫描并恢复状态
    result = mkv_scan();
    if (result != MKV_OK)
    {
        log_e("Failed to scan MicroKV: %d", result);
        return result;
    }

    log_i("MicroKV initialized!");
    log_i("Flash: 0x%08X, Size: %u KB", MKV_FLASH_BASE, MKV_FLASH_SIZE / 1024);

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
 * @note 如果启用了增量GC，可在此处定期调用 mkv_gc_step() 分摄GC开销
 * @note 当前实现为空，用户可根据需要添加维护代码
 */
void mkv_task(void)
{
    // 可以添加定期维护任务
}
