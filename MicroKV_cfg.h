/**
 * @file MicroKV_cfg.h
 * @brief MicroKV 配置文件
 * @author liu
 * @date 2025-12-20
 * @version 2.0
 *
 * @details 本文件包含 MicroKV 的所有可配置参数，用户可根据实际需求修改：
 *          - 键值长度限制
 *          - Flash 存储区配置
 *          - 缓存功能开关
 *          - 增量 GC 策略
 */

/* ==================== 键值配置 ==================== */

/** @brief 最大键名长度（字节）
 *  @note 过长的键名会占用更多Flash空间，建议 8-16 字节 */

#ifndef __MICROKV_CFG_H
#define __MICROKV_CFG_H

#define MKV_MAX_KEY_LEN 16

/** @brief 最大值长度（字节）
 *  @note 受限于uint8_t类型，最大255字节 */
#define MKV_MAX_VALUE_LEN 255


/* ==================== 缓存配置 ==================== */

/** @brief 启用缓存功能
 *  @note 0=禁用，1=启用。启用后使用LFU算法缓存热点数据 */
#define MKV_CACHE_ENABLE 1

/** @brief 缓存条目数量
 *  @note 每个条目占用约 MKV_MAX_KEY_LEN + MKV_MAX_VALUE_LEN + 10 字节RAM */
#define MKV_CACHE_SIZE 4

/* ==================== 增量GC配置 ==================== */

/** @brief 启用增量垃圾回收
 *  @note 0=禁用（使用传统全量GC），1=启用（分摊GC开销） */
#define MKV_INCREMENTAL_GC 1

/** @brief 每次写入后迁移的条目数
 *  @note 值越大GC越快但单次写入耗时越长，建议1-4 */
#define MKV_GC_ENTRIES_PER_WRITE 2

/** @brief GC触发阈值（使用率百分比）
 *  @note 当活跃扇区使用率超过此值时启动GC，建议60-80 */
#define MKV_GC_THRESHOLD_PERCENT 70

#endif /* __MICROKV_CFG_H */
