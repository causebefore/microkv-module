/**
 * @file MicroKV_port.h
 * @brief MicroKV移植层接口 (单实例)
 * @author liu
 * @date 2025-12-20
 * @version 2.0
 * @copyright Copyright (c) 2025 by liu lbq08@foxmail.com, All Rights Reserved.
 */

#ifndef __MICROKV_PORT_H
#define __MICROKV_PORT_H

#include "MicroKV.h"

#ifdef __cplusplus
extern "C"
{
#endif
/* ==================== Flash配置 ==================== */

/** @brief Flash起始地址
 *  @note 通过FAL分区管理，此地址为kv分区起始地址
 *  @note STM32F407: 0x08080000 (分区偏移0x80000) */
#define MKV_FLASH_BASE 0x08080000

/** @brief 扩区大小（字节）
 *  @note STM32F407扩区8-11为128KB，MicroKV需要与Flash扩区大小匹配 */
#define MKV_SECTOR_SIZE (128 * 1024)

/** @brief 扩区数量
 *  @note FAL kv分区共512KB = 4个128KB扩区 */
#define MKV_SECTOR_COUNT 4

/** @brief Flash总大小（自动计算） */
#define MKV_FLASH_SIZE (MKV_SECTOR_SIZE * MKV_SECTOR_COUNT)


    /* ===========================================================================
     *                          接口函数声明
     * ===========================================================================*/

    /**
     * @brief MicroKV 初始化
     * @return MKV_OK 成功，其他失败
     * @note 在使用其他 mkv_xxx() 函数前调用
     */
    MKV_Error_t mkv_init(void);

    /**
     * @brief MicroKV维护任务（可选）
     */
    void mkv_task(void);

#ifdef __cplusplus
}
#endif

#endif /* __MICROKV_PORT_H */
