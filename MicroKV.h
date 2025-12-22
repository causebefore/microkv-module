/**
 * @file MicroKV.h
 * @brief 轻量级键值存储
 * @author liu
 * @date 2025-12-20
 * @version 2.0
 *
 * @details MicroKV 是一个专为嵌入式系统设计的轻量级KV存储库，特点：
 *
 * ## 主要特性
 * - 快速初始化（几毫秒）
 * - 追加写入，无需擦除
 * - 双扇区交替，自动整理
 * - 掉电安全
 * - 简洁的单实例 API
 * - LFU 缓存加速读取
 * - 增量GC分摊延迟
 * - 默认值支持
 *
 * ## 使用示例
 * @code
 * // 1. 初始化
 * mkv_init();
 *
 * // 2. 写入数据
 * uint32_t timeout = 5000;
 * mkv_set("timeout", &timeout, sizeof(timeout));
 * mkv_set("wifi_ssid", "MyWiFi", 6);
 *
 * // 3. 读取数据
 * uint32_t value;
 * uint8_t len;
 * if (mkv_get("timeout", &value, sizeof(value), &len) == MKV_OK) {
 *     printf("timeout = %u\n", value);
 * }
 *
 * // 4. 删除数据
 * mkv_del("timeout");
 *
 * // 5. 默认值支持
 * static const MKV_Default_t defaults[] = {
 *     MKV_DEF_STR("wifi_ssid", "DefaultSSID"),
 *     MKV_DEF_INT("timeout", 3000),
 * };
 * mkv_set_defaults(defaults, MKV_DEFAULT_TABLE_SIZE(defaults));
 * mkv_get_default("wifi_ssid", buffer, sizeof(buffer), &len);
 * @endcode
 *
 * ## 内存布局
 * @code
 * Flash布局:
 * +-------------+-------------+-----+-------------+
 * | Sector 0    | Sector 1    | ... | Sector N-1  |
 * +-------------+-------------+-----+-------------+
 *
 * 扇区结构:
 * +---------+---------+---------+-----+---------+
 * | Header  | Entry 1 | Entry 2 | ... | 0xFFFF  |
 * +---------+---------+---------+-----+---------+
 *
 * 条目结构:
 * +-------+-----+-----+-----+-------+------+
 * | State | KL  | VL  | Key | Value | CRC  |
 * +-------+-----+-----+-----+-------+------+
 *   2B     1B    1B    N     M       2B
 * @endcode
 */

#ifndef __MICROKV_H
#define __MICROKV_H

#include <stdint.h>
#include <string.h>

/* 包含配置文件 - 必须在stdint.h之后 */
#include "MicroKV_cfg.h"

/* 魔数和状态 */
/** @brief 扇区魔数，用于验证扇区有效性 */
#define MKV_MAGIC 0x4B56 // "KV"

/** @brief 条目状态：已擦除（Flash擦除后的初始状态） */
#define MKV_STATE_ERASED 0xFFFF

/** @brief 条目状态：写入中（中间状态，用于提高掉电安全） */
#define MKV_STATE_WRITING 0xFF00

/** @brief 条目状态：有效（数据写入完成且CRC校验通过） */
#define MKV_STATE_VALID 0x0000

/* ==================== 默认值机制 ==================== */
/**
 * @brief 默认值条目结构
 * @note 用于定义键的默认值表，当键不存在时自动返回默认值
 */
typedef struct
{
    const char *key;   // 键名
    const void *value; // 默认值指针
    uint8_t     len;   // 值长度
} MKV_Default_t;

/**
 * @brief 扇区头结构
 * @note 每个扇区的起始位置存放扇区头，用于标识扇区有效性和顺序
 */
typedef struct __attribute__((packed))
{
    uint16_t magic; /**< 魔数 0x4B56 */
    uint16_t seq;   /**< 序号（用于判断哪个扇区更新） */
} MKV_SectorHeader_t;

/** @brief 扇区头大小（4字节） */
#define MKV_SECTOR_HEADER_SIZE 4

/* ==================== 错误码 ==================== */
/**
 * @brief MicroKV 错误码
 */
typedef enum
{
    MKV_OK = 0,        /**< 操作成功 */
    MKV_ERR_NOT_FOUND, /**< 键不存在 */
    MKV_ERR_NO_SPACE,  /**< 空间不足 */
    MKV_ERR_INVALID,   /**< 参数无效 */
    MKV_ERR_FLASH,     /**< Flash操作失败 */
} MKV_Error_t;

/* ==================== KV条目结构 ==================== */
/**
 * @brief KV条目结构体
 * @note 内存布局: [state:2B][key_len:1B][val_len:1B][key:N][value:M][crc:2B]
 *       采用紧凑封装，避免对齐填充
 */
typedef struct __attribute__((packed))
{
    uint16_t state;   /**< 条目状态: MKV_STATE_ERASED/WRITING/VALID */
    uint8_t  key_len; /**< 键长度 */
    uint8_t  val_len; /**< 值长度 */
    // 后面紧跟: key[key_len] + value[val_len] + crc16[2]
} MKV_Entry_t;

/** @brief 条目头大小（state + key_len + val_len） */
#define MKV_ENTRY_HEADER_SIZE 4

/** @brief 条目CRC校验大小（CRC16） */
#define MKV_ENTRY_CRC_SIZE 2

/* ==================== 缓存结构 ==================== */
#if MKV_CACHE_ENABLE
/**
 * @brief 缓存条目结构
 * @note 使用LFU（Least Frequently Used）算法管理缓存
 */
typedef struct
{
    char     key[MKV_MAX_KEY_LEN];     /**< 键名 */
    uint8_t  value[MKV_MAX_VALUE_LEN]; /**< 缓存的值 */
    uint8_t  key_len;                  /**< 键长度 */
    uint8_t  val_len;                  /**< 值长度 */
    uint8_t  valid;                    /**< 缓存有效标志 */
    uint32_t access_count;             /**< LFU: 访问次数 */
} MKV_CacheEntry_t;

/**
 * @brief 缓存统计信息
 */
typedef struct
{
    uint32_t hit_count;  /**< 缓存命中次数 */
    uint32_t miss_count; /**< 缓存未命中次数 */
    float    hit_rate;   /**< 缓存命中率（%） */
} MKV_CacheStats_t;
#endif

/* ==================== 移植接口 ==================== */

/**
 * @brief Flash读取函数类型
 * @param addr Flash地址
 * @param buf 接收缓冲区
 * @param len 数据长度
 * @return int 0=成功，-1=失败
 */
typedef int (*MKV_ReadFunc_t)(uint32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief Flash写入函数类型
 * @param addr Flash地址
 * @param buf 数据缓冲区
 * @param len 数据长度
 * @return int 0=成功，-1=失败
 */
typedef int (*MKV_WriteFunc_t)(uint32_t addr, const uint8_t *buf, uint32_t len);

/**
 * @brief Flash擦除函数类型
 * @param addr 扇区地址
 * @return int 0=成功，-1=失败
 */
typedef int (*MKV_EraseFunc_t)(uint32_t addr);

/**
 * @brief Flash操作回调函数结构体
 * @note 用于封装底层Flash操作，便于移植到不同平台
 */
typedef struct
{
    MKV_ReadFunc_t  read_func;  /**< Flash读取函数 */
    MKV_WriteFunc_t write_func; /**< Flash写入函数 */
    MKV_EraseFunc_t erase_func; /**< Flash擦除函数 */

    /* Flash配置 */
    uint32_t flash_base;   /**< Flash基地址 */
    uint32_t sector_size;  /**< 扇区大小（字节） */
    uint8_t  sector_count; /**< 扇区数量 */
    uint8_t  align_size;   /**< 对齐字节数 (2=半字对齐, 4=字对齐) */
} MKV_FlashOps_t;

#if MKV_CACHE_ENABLE
/* 缓存系统结构体 */
typedef struct
{
    MKV_CacheEntry_t entries[MKV_CACHE_SIZE]; // 缓存条目数组
    uint32_t         hit_count;               // 命中次数
    uint32_t         miss_count;              // 未命中次数
} MKV_Cache_t;
#endif

/**
 * @brief MicroKV实例结构体
 * @note 单实例设计，此结构在MicroKV.c中定义为静态全局变量
 */
typedef struct
{
    /* Flash操作回调和配置 */
    MKV_FlashOps_t flash_ops; /**< Flash操作函数和配置 */

    /* 运行状态 */
    uint8_t  initialized;   /**< 初始化标志 */
    uint8_t  active_sector; /**< 当前活跃扇区索引 */
    uint16_t sector_seq;    /**< 当前扇区序号 */
    uint32_t write_offset;  /**< 下一个写入位置 */

#if MKV_INCREMENTAL_GC
    /* 增量GC状态 */
    uint8_t  gc_src_sector; /**< GC源扇区索引 */
    uint32_t gc_src_offset; /**< GC源扇区当前扫描位置 */
    uint8_t  gc_active;     /**< GC进行中标志 */
    uint8_t  gc_bitmap[32]; /**< 已迁移键的哈希位图 */
#endif

    /* 默认值表 */
    const MKV_Default_t *defaults;      /**< 默认值表指针 */
    uint16_t             default_count; /**< 默认值条目数量 */

#if MKV_CACHE_ENABLE
    /* 缓存系统 */
    MKV_Cache_t cache; /**< 缓存系统 */
#endif
} MKV_Instance_t;

/* ==================== 内部初始化 (由 port 层调用) ==================== */

/**
 * @brief 内部初始化函数（由移植层调用）
 * @param ops Flash操作函数结构体
 * @return MKV_Error_t 错误码
 * @retval MKV_OK 成功
 * @retval MKV_ERR_INVALID 参数无效
 * @note 此函数由mkv_init()调用，用户不应直接调用
 */
MKV_Error_t mkv_internal_init(const MKV_FlashOps_t *ops);

/**
 * @brief 扫描并初始化 KV 存储
 * @return MKV_Error_t 错误码
 * @retval MKV_OK 成功
 * @retval MKV_ERR_FLASH Flash操作失败
 * @note 扫描所有扇区，找到最新的活跃扇区，如果没有有效扇区则自动格式化
 */
MKV_Error_t mkv_scan(void);

/* ==================== 基础 API ==================== */

/**
 * @brief 设置键值
 * @param key 键名（以\0结尾的字符串）
 * @param value 值指针
 * @param len 值长度（字节）
 * @return MKV_Error_t 错误码
 * @retval MKV_OK 成功
 * @retval MKV_ERR_INVALID 参数无效
 * @retval MKV_ERR_NO_SPACE 空间不足
 * @retval MKV_ERR_FLASH Flash操作失败
 * @note 如果键已存在，新值会追加在后面（旧值仍在，但不会被读取）
 */
MKV_Error_t mkv_set(const char *key, const void *value, uint8_t len);

/**
 * @brief 获取键值
 * @param key 键名（以\0结尾的字符串）
 * @param buffer 输出缓冲区
 * @param buf_size 缓冲区大小（字节）
 * @param out_len 输出：实际长度（可为NULL）
 * @return MKV_Error_t 错误码
 * @retval MKV_OK 成功
 * @retval MKV_ERR_NOT_FOUND 键不存在
 * @retval MKV_ERR_INVALID 参数无效
 * @retval MKV_ERR_FLASH Flash操作失败
 * @note 会优先从缓存中查找，缓存未命中时从 Flash 读取
 */
MKV_Error_t mkv_get(const char *key, void *buffer, uint8_t buf_size, uint8_t *out_len);

/**
 * @brief 删除键
 * @param key 键名（以\0结尾的字符串）
 * @return MKV_Error_t 错误码
 * @retval MKV_OK 成功
 * @note 实际上是写入一个长度为0的条目，不会立即擦除 Flash
 */
MKV_Error_t mkv_del(const char *key);

/**
 * @brief 检查键是否存在
 * @param key 键名（以\0结尾的字符串）
 * @return uint8_t 存在性
 * @retval 1 键存在
 * @retval 0 键不存在或已被删除
 */
uint8_t mkv_exists(const char *key);

/**
 * @brief 格式化存储区
 * @return MKV_Error_t 错误码
 * @retval MKV_OK 成功
 * @retval MKV_ERR_FLASH Flash操作失败
 * @warning 此操作会擦除所有数据，不可恢复！
 */
MKV_Error_t mkv_format(void);

/**
 * @brief 获取存储使用情况
 * @param used 输出：已用字节（可为NULL）
 * @param total 输出：总字节（可为NULL）
 * @note 已用字节仅指活跃扇区，总字节为所有扇区总和
 */
void mkv_get_usage(uint32_t *used, uint32_t *total);

/* ==================== 增量GC API ==================== */
#if MKV_INCREMENTAL_GC
/**
 * @brief 手动执行一次增量GC
 * @param steps 执行步数（迁移条目数）
 * @return uint8_t GC状态
 * @retval 1 GC进行中
 * @retval 0 无需GC或已完成
 * @note 可在空闲时调用，分摄GC开销，减少单次写入延迟
 * @see mkv_gc_is_active()
 */
uint8_t mkv_gc_step(uint8_t steps);

/**
 * @brief 获取GC状态
 * @return uint8_t GC状态
 * @retval 1 GC进行中
 * @retval 0 GC空闲
 * @see mkv_gc_step()
 */
uint8_t mkv_gc_is_active(void);
#endif

/* ==================== 缓存 API ==================== */
#if MKV_CACHE_ENABLE
/**
 * @brief 获取缓存统计信息
 * @param stats 输出：缓存统计结构体
 * @note 可用于调优缓存大小和策略
 */
void mkv_get_cache_stats(MKV_CacheStats_t *stats);

/**
 * @brief 清空缓存
 * @note 清空所有缓存条目和统计信息
 */
void mkv_cache_clear(void);
#endif

/* ==================== 默认值 API ==================== */

/**
 * @brief 设置默认值表
 * @param defaults 默认值表（必须是静态/全局数组）
 * @param count 默认值条目数量
 * @note 使用宏 MKV_DEFAULT_TABLE_SIZE() 自动计算数量
 * @warning 默认值表必须为静态或全局变量，不能是局部数组！
 * @code
 * static const MKV_Default_t my_defaults[] = {
 *     MKV_DEF_STR("wifi_ssid", "MyWiFi"),
 *     MKV_DEF_INT("timeout", 5000),
 * };
 * mkv_set_defaults(my_defaults, MKV_DEFAULT_TABLE_SIZE(my_defaults));
 * @endcode
 */
void mkv_set_defaults(const MKV_Default_t *defaults, uint16_t count);

/**
 * @brief 获取值（支持默认值回退）
 * @param key 键名
 * @param buffer 输出缓冲区
 * @param buf_size 缓冲区大小
 * @param out_len 输出：实际长度
 * @return MKV_Error_t 错误码
 * @retval MKV_OK 成功
 * @retval MKV_ERR_NOT_FOUND 键和默认值都不存在
 * @note 优先返回Flash中的值，不存在时返回默认值
 * @see mkv_get(), mkv_set_defaults()
 */
MKV_Error_t mkv_get_default(const char *key, void *buffer, uint8_t buf_size, uint8_t *out_len);

/**
 * @brief 重置键为默认值
 * @param key 键名
 * @return MKV_Error_t 错误码
 * @retval MKV_OK 成功
 * @retval MKV_ERR_NOT_FOUND 默认值不存在
 * @note 将指定键的值覆盖为默认值表中定义的值
 * @see mkv_reset_all()
 */
MKV_Error_t mkv_reset_key(const char *key);

/**
 * @brief 重置所有键为默认值
 * @return MKV_Error_t 错误码
 * @retval MKV_OK 成功
 * @note 遍历默认值表，将所有键覆盖为默认值
 * @warning 此操作会覆盖所有用户设置，谨慎使用！
 * @see mkv_reset_key()
 */
MKV_Error_t mkv_reset_all(void);

/**
 * @brief 查找默认值
 * @param key 键名
 * @return const MKV_Default_t* 默认值条目指针，NULL表示不存在
 * @note 内部函数，用户一般不直接调用
 */
const MKV_Default_t *mkv_find_default(const char *key);

/**
 * @brief 计算默认值表大小
 * @param table 默认值表数组
 * @return 数组元素个数
 * @code
 * static const MKV_Default_t defaults[] = { ... };
 * mkv_set_defaults(defaults, MKV_DEFAULT_TABLE_SIZE(defaults));
 * @endcode
 */
#define MKV_DEFAULT_TABLE_SIZE(table) (sizeof(table) / sizeof((table)[0]))

/**
 * @brief 定义字符串默认值
 * @param k 键名
 * @param v 字符串值（常量字符串）
 * @note v 必须为常量字符串，不能是变量
 * @code
 * MKV_DEF_STR("wifi_ssid", "MyWiFi")
 * @endcode
 */
#define MKV_DEF_STR(k, v) {.key = (k), .value = (v), .len = sizeof(v) - 1}

/**
 * @brief 定义整数默认值
 * @param k 键名
 * @param v 整数值
 * @note 自动创建一个uint32_t匿名变量存储值
 * @code
 * MKV_DEF_INT("timeout", 5000)
 * @endcode
 */
#define MKV_DEF_INT(k, v) {.key = (k), .value = &(uint32_t) {v}, .len = sizeof(uint32_t)}

/**
 * @brief 定义任意类型默认值
 * @param k 键名
 * @param v 值指针
 * @param l 值长度（字节）
 * @code
 * uint8_t mac[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
 * MKV_DEF_DATA("mac_addr", mac, sizeof(mac))
 * @endcode
 */
#define MKV_DEF_DATA(k, v, l) {.key = (k), .value = (v), .len = (l)}

#endif /* __MICROKV_H */
