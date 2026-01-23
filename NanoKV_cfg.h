/**
 * @file NanoKV_cfg.h
 * @brief NanoKV 配置文件
 * @note 所有可配置参数：键值长度、缓存、GC策略、TLV保留策略
 */

#ifndef __NANOKV_CFG_H
#define __NANOKV_CFG_H

/* 键值长度配置 */
#define NKV_MAX_KEY_LEN   16  /* 最大键名长度(字节)，建议8-16 */
#define NKV_MAX_VALUE_LEN 255 /* 最大值长度(字节)，受限于uint8_t */

/* 版本自动更新配置 */
#define NKV_SETTING_VER 1 /* 配置版本号，增加新默认参数时需递增此值 */

/* 缓存配置 */
#define NKV_CACHE_ENABLE 1 /* 启用LFU缓存：0=禁用, 1=启用 */
#define NKV_CACHE_SIZE   4 /* 缓存条目数量 */

/* 增量GC配置 */
#define NKV_INCREMENTAL_GC       1  /* 启用增量GC：0=禁用(全量GC), 1=启用 */
#define NKV_GC_ENTRIES_PER_WRITE 2  /* 每次写入后迁移的条目数，建议1-4 */
#define NKV_GC_THRESHOLD_PERCENT 70 /* GC触发阈值(使用率%)，建议60-80 */

/* TLV保留策略配置 */
#define NKV_TLV_RETENTION_ENABLE 1 /* 启用TLV保留策略：0=禁用, 1=启用 */
#define NKV_TLV_RETENTION_MAX    8 /* TLV保留策略表最大条目数 */

/* 可靠性增强配置 */
#define NKV_VERIFY_ON_READ      1 /* 读取时CRC校验：0=禁用, 1=启用 */
#define NKV_CLEAN_DIRTY_ON_BOOT 1 /* 启动时清理WRITING状态的脏数据：0=禁用, 1=启用 */

/* 打印调试配置 */
#define NKV_DEBUG_ENABLE 1

#if NKV_DEBUG_ENABLE
    #include <stdio.h>
    #ifndef NKV_PRINTF
        #define NKV_PRINTF(...) printf(__VA_ARGS__)
    #endif
    #ifndef NKV_LOG_I
        #define NKV_LOG_I(...)                                                                                         \
            do                                                                                                         \
            {                                                                                                          \
                NKV_PRINTF("[NanoKV][I] " __VA_ARGS__);                                                                \
                NKV_PRINTF("\n");                                                                                      \
            }                                                                                                          \
            while (0)
    #endif
    #ifndef NKV_LOG_E
        #define NKV_LOG_E(...)                                                                                         \
            do                                                                                                         \
            {                                                                                                          \
                NKV_PRINTF("[NanoKV][E] " __VA_ARGS__);                                                                \
                NKV_PRINTF("\n");                                                                                      \
            }                                                                                                          \
            while (0)
    #endif
#else
    #ifndef NKV_PRINTF
        #define NKV_PRINTF(...) ((void) 0)
    #endif
    #ifndef NKV_LOG_I
        #define NKV_LOG_I(...) ((void) 0)
    #endif
    #ifndef NKV_LOG_E
        #define NKV_LOG_E(...) ((void) 0)
    #endif
#endif


#ifndef NKV_ASSERT
    #ifdef NDEBUG
        #define NKV_ASSERT(expr) ((void) 0) /* Release模式禁用断言 */
    #else
        #define NKV_ASSERT(expr)                                                                                       \
            do                                                                                                         \
            {                                                                                                          \
                if (!(expr))                                                                                           \
                {                                                                                                      \
                    NKV_LOG_E("ASSERT FAILED: (%s) at %s:%d", #expr, __FILE__, __LINE__);                              \
                    while (1)                                                                                          \
                        ;                                                                                              \
                }                                                                                                      \
            }                                                                                                          \
            while (0)
    #endif
#endif

#endif /* __NANOKV_CFG_H */
