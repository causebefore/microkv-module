/**
 * @file NanoKV_port.h
 * @brief NanoKV移植层接口
 * @note Flash配置和初始化函数声明
 */

#ifndef __NANOKV_PORT_H
#define __NANOKV_PORT_H

#include "NanoKV.h"

#ifdef __cplusplus
extern "C"
{
#endif


    /* 初始化函数 */
    nkv_err_t nkv_init(void); /* 初始化NanoKV */
    void      nkv_task(void); /* 维护任务(可选) */

#ifdef __cplusplus
}
#endif

#endif /* __NANOKV_PORT_H */
