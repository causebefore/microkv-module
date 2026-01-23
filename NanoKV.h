/**
 * @file NanoKV.h
 * @brief NanoKV - 轻量级嵌入式KV/TLV存储库
 * @version 3.0
 * @date 2026-01-18
 *
 * @details 特性介绍：
 * - 追加写入：无需擦除即可更新，减少Flash磨损
 * - 多扇区环形：自动磨损均衡，充分利用存储空间
 * - 掉电安全：状态机 + CRC校验保障数据完整性
 * - LFU缓存：加速热点数据访问，提升读取性能
 * - 增量GC：分摊垃圾回收开销，适合实时系统
 * - 默认值支持：配置项可回退到预设值
 */

#ifndef __NANOKV_H
#define __NANOKV_H

#include "NanoKV_cfg.h"

#include <stdint.h>
#include <string.h>

/* ==================== 跨平台定义 ==================== */
#ifndef NKV_PACKED
    #if defined(__GNUC__) || defined(__clang__)
        #define NKV_PACKED __attribute__((packed))
    #else
        #define NKV_PACKED
    #endif
#endif

/* ==================== 常量定义 ==================== */
#define NKV_MAGIC           0x4B56 /* 扇区魔数 "KV" */
#define NKV_STATE_ERASED    0xFFFF /* 已擦除状态 (1111 1111 1111 1111) */
#define NKV_STATE_WRITING   0xFFFE /* 写入中状态 (1111 1111 1111 1110) */
#define NKV_STATE_VALID     0xFFFC /* 有效状态   (1111 1111 1111 1100) */
#define NKV_STATE_PRE_DEL   0xFFF8 /* 预删除状态 (1111 1111 1111 1000) */
#define NKV_STATE_DELETED   0x0000 /* 已删除状态 (0000 0000 0000 0000) */
#define NKV_HEADER_SIZE     4      /* 条目头大小 */
#define NKV_CRC_SIZE        2      /* CRC校验大小 */
#define NKV_SECTOR_HDR_SIZE 4      /* 扇区头大小 */

/* ==================== 错误码 ==================== */
typedef enum
{
    NKV_OK = 0,        /* 成功 */
    NKV_ERR_NOT_FOUND, /* 未找到 */
    NKV_ERR_NO_SPACE,  /* 空间不足 */
    NKV_ERR_INVALID,   /* 参数无效 */
    NKV_ERR_FLASH,     /* Flash操作失败 */
    NKV_ERR_CRC,       /* CRC校验失败 */
} nkv_err_t;

/* ==================== 基础结构体 ==================== */

/* 扇区头 */
typedef struct
{
    uint16_t magic; /* 魔数 */
    uint16_t seq;   /* 序号 */
} NKV_PACKED nkv_sector_hdr_t;

/* KV条目头 */
typedef struct
{
    uint16_t state;   /* 状态 */
    uint8_t  key_len; /* 键长度 */
    uint8_t  val_len; /* 值长度 */
} NKV_PACKED nkv_entry_t;

/* 默认值条目 */
typedef struct
{
    const char* key;
    const void* value;
    uint8_t     len;
} nkv_default_t;

/* Flash操作回调 */
typedef int (*nkv_read_fn)(uint32_t addr, uint8_t* buf, uint32_t len);
typedef int (*nkv_write_fn)(uint32_t addr, const uint8_t* buf, uint32_t len);
typedef int (*nkv_erase_fn)(uint32_t addr);

/* Flash操作配置 */
typedef struct
{
    nkv_read_fn  read;
    nkv_write_fn write;
    nkv_erase_fn erase;
    uint32_t     base;         /* Flash基地址 */
    uint32_t     sector_size;  /* 扇区大小 */
    uint8_t      sector_count; /* 扇区数量 */
    uint8_t      align;        /* 对齐字节数 */
} nkv_flash_ops_t;

/* ==================== 缓存结构 ==================== */
#if NKV_CACHE_ENABLE
typedef struct
{
    char     key[NKV_MAX_KEY_LEN];
    uint8_t  value[NKV_MAX_VALUE_LEN];
    uint8_t  key_len;
    uint8_t  val_len;
    uint8_t  valid;
    uint32_t access_count;
} nkv_cache_entry_t;

typedef struct
{
    uint32_t hit_count;
    uint32_t miss_count;
    float    hit_rate;
} nkv_cache_stats_t;

typedef struct
{
    nkv_cache_entry_t entries[NKV_CACHE_SIZE];
    uint32_t          hit_count;
    uint32_t          miss_count;
} nkv_cache_t;
#endif

/* ==================== 主实例结构 ==================== */
typedef struct
{
    nkv_flash_ops_t flash;
    uint8_t         initialized;
    uint8_t         active_sector;
    uint16_t        sector_seq;
    uint32_t        write_offset;
#if NKV_INCREMENTAL_GC
    uint8_t  gc_src_sector;
    uint32_t gc_src_offset;
    uint8_t  gc_active;
    uint8_t  gc_bitmap[32];
#endif
    const nkv_default_t* defaults;
    uint16_t             default_count;
#if NKV_CACHE_ENABLE
    nkv_cache_t cache;
#endif
} nkv_instance_t;

/* ==================== KV API ==================== */

/* 初始化相关（由port层调用） */
nkv_err_t nkv_internal_init(const nkv_flash_ops_t* ops); /* 内部初始化 */
nkv_err_t nkv_scan(void);                                /* 扫描并恢复状态 */
nkv_err_t nkv_format(void);                              /* 格式化存储区 */

/* 基础操作 */
nkv_err_t nkv_set(const char* key, const void* value, uint8_t len);            /* 设置键值 */
nkv_err_t nkv_get(const char* key, void* buf, uint8_t size, uint8_t* out_len); /* 获取键值 */
nkv_err_t nkv_del(const char* key);                                            /* 删除键 */
uint8_t   nkv_exists(const char* key);                                         /* 检查键是否存在 */
void      nkv_get_usage(uint32_t* used, uint32_t* total);                      /* 获取使用情况 */

/* 默认值支持 */
void                 nkv_set_defaults(const nkv_default_t* defs, uint16_t count);
nkv_err_t            nkv_get_default(const char* key, void* buf, uint8_t size, uint8_t* out_len);
const nkv_default_t* nkv_find_default(const char* key);
nkv_err_t            nkv_reset_key(const char* key);
nkv_err_t            nkv_reset_all(void);

/* 内部函数导出 */
nkv_instance_t* nkv_get_instance(void);
uint8_t         nkv_is_sector_valid(uint8_t idx);

/* ==================== 增量GC API ==================== */
#if NKV_INCREMENTAL_GC
uint8_t nkv_gc_step(uint8_t steps); /* 手动执行GC步骤 */
uint8_t nkv_gc_active(void);        /* 获取GC状态 */
#endif

/* ==================== 缓存API ==================== */
#if NKV_CACHE_ENABLE
void nkv_cache_stats(nkv_cache_stats_t* stats);
void nkv_cache_clear(void);
#endif

/* ==================== 默认值辅助宏 ==================== */
#define NKV_DEFAULT_SIZE(t)   (sizeof(t) / sizeof((t)[0]))
#define NKV_DEF_STR(k, v)     {.key = (k), .value = (v), .len = sizeof(v) - 1}
#define NKV_DEF_INT(k, v)     {.key = (k), .value = &(uint32_t) {v}, .len = sizeof(uint32_t)}
#define NKV_DEF_DATA(k, v, l) {.key = (k), .value = (v), .len = (l)}

/* ==================== TLV扩展 ==================== */

/* TLV类型范围 */
#define TLV_TYPE_RESERVED 0x00
#define TLV_TYPE_APP_MIN  0x01
#define TLV_TYPE_APP_MAX  0x7F
#define TLV_TYPE_SYS_MIN  0x80
#define TLV_TYPE_SYS_MAX  0xFF

/* TLV默认值 */
typedef struct
{
    uint8_t     type;
    const void* value;
    uint8_t     len;
} nkv_tlv_default_t;

/* TLV迭代器 */
typedef struct
{
    uint8_t  sector_idx;
    uint32_t sector_offset;
    uint8_t  finished;
} nkv_tlv_iter_t;

/* TLV条目信息 */
typedef struct
{
    uint8_t  type;
    uint8_t  len;
    uint32_t flash_addr;
} nkv_tlv_entry_t;

/* TLV历史记录 */
typedef struct
{
    uint8_t  type;
    uint8_t  len;
    uint32_t flash_addr;
    uint32_t write_order;
} nkv_tlv_history_t;

/* TLV基础API */
nkv_err_t nkv_tlv_set(uint8_t type, const void* value, uint8_t len);
nkv_err_t nkv_tlv_get(uint8_t type, void* buf, uint8_t size, uint8_t* out_len);
nkv_err_t nkv_tlv_del(uint8_t type);
uint8_t   nkv_tlv_exists(uint8_t type);

/* TLV默认值API */
void      nkv_tlv_set_defaults(const nkv_tlv_default_t* defs, uint16_t count);
nkv_err_t nkv_tlv_get_default(uint8_t type, void* buf, uint8_t size, uint8_t* out_len);
nkv_err_t nkv_tlv_reset_type(uint8_t type);
nkv_err_t nkv_tlv_reset_all(void);

/* TLV迭代器API */
void      nkv_tlv_iter_init(nkv_tlv_iter_t* iter);
uint8_t   nkv_tlv_iter_next(nkv_tlv_iter_t* iter, nkv_tlv_entry_t* info);
nkv_err_t nkv_tlv_iter_read(const nkv_tlv_entry_t* info, void* buf, uint8_t size);

/* TLV工具函数 */
void    nkv_tlv_stats(uint16_t* count, uint32_t* used);
uint8_t nkv_tlv_has_data(void);

/* TLV历史记录API */
nkv_err_t nkv_tlv_get_history(uint8_t type, nkv_tlv_history_t* history, uint8_t max, uint8_t* count);
nkv_err_t nkv_tlv_read_history(const nkv_tlv_history_t* entry, void* buf, uint8_t size);

/* TLV保留策略API */
#if NKV_TLV_RETENTION_ENABLE
nkv_err_t nkv_tlv_set_retention(uint8_t type, uint16_t keep_newest);
void      nkv_tlv_clear_retention(uint8_t type);
#endif

/* TLV辅助宏 */
#define NKV_TLV_DEF_U8(t, v)      {.type = t, .value = &(uint8_t) {v}, .len = 1}
#define NKV_TLV_DEF_U16(t, v)     {.type = t, .value = &(uint16_t) {v}, .len = 2}
#define NKV_TLV_DEF_U32(t, v)     {.type = t, .value = &(uint32_t) {v}, .len = 4}
#define NKV_TLV_DEF_DATA(t, p, l) {.type = t, .value = p, .len = l}
#define NKV_TLV_DEFAULT_SIZE(t)   (sizeof(t) / sizeof((t)[0]))

#endif /* __NANOKV_H */
