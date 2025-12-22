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
