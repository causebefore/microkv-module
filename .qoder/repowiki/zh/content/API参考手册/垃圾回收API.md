# 垃圾回收API

<cite>
**本文引用的文件**
- [NanoKV.h](file://NanoKV.h)
- [NanoKV.c](file://NanoKV.c)
- [NanoKV_cfg.h](file://NanoKV_cfg.h)
- [NanoKV_port.h](file://NanoKV_port.h)
- [NanoKV_port.c](file://NanoKV_port.c)
</cite>

## 更新摘要
**所做更改**
- 完全重写以反映从MicroKV到NanoKV的完全替换
- 新增增强的增量GC策略和全量GC选项
- 更新GC状态监控和查询机制
- 添加TLV保留策略与GC的协同机制
- 更新配置参数和使用示例
- 重新设计架构图以反映新的实现

## 目录
1. [简介](#简介)
2. [项目结构](#项目结构)
3. [核心组件](#核心组件)
4. [架构总览](#架构总览)
5. [详细组件分析](#详细组件分析)
6. [依赖关系分析](#依赖关系分析)
7. [性能考量](#性能考量)
8. [故障排除指南](#故障排除指南)
9. [结论](#结论)
10. [附录](#附录)

## 简介
本文档详细介绍NanoKV的垃圾回收（GC）API，包括增强的增量GC控制接口：nkv_gc_step与nkv_gc_active。NanoKV作为全新的轻量级嵌入式KV/TLV存储库，提供了：
- 增量GC与全量GC双重策略
- TLV保留策略与GC的智能协同
- 分步执行的增量GC机制
- GC状态监控与触发条件
- 执行时机与性能影响分析
- 配置参数优化建议
- 系统集成指导与故障排除

## 项目结构
NanoKV采用现代化的"头文件声明 + 单一实现文件 + 配置头 + 移植层"的组织方式，垃圾回收API位于公共头文件中，并在实现文件中提供完整的增量GC逻辑。

```mermaid
graph TB
A["NanoKV.h<br/>公共API与结构体声明"] --> B["NanoKV.c<br/>实现与GC逻辑"]
C["NanoKV_cfg.h<br/>编译期配置"] --> B
D["NanoKV_port.h<br/>移植层接口声明"] --> E["NanoKV_port.c<br/>移植层实现"]
E --> B
B --> F["增量GC子系统"]
B --> G["全量GC子系统"]
F --> H["TLV保留策略"]
G --> H
```

**图表来源**
- [NanoKV.h](file://NanoKV.h#L170-L174)
- [NanoKV.c](file://NanoKV.c#L592-L735)
- [NanoKV_cfg.h](file://NanoKV_cfg.h#L21-L28)
- [NanoKV_port.c](file://NanoKV_port.c#L53-L88)

**章节来源**
- [NanoKV.h](file://NanoKV.h#L170-L174)
- [NanoKV.c](file://NanoKV.c#L592-L735)
- [NanoKV_cfg.h](file://NanoKV_cfg.h#L21-L28)
- [NanoKV_port.c](file://NanoKV_port.c#L53-L88)

## 核心组件
- **增量GC控制API**
  - nkv_gc_step(steps): 手动执行指定步数的增量GC
  - nkv_gc_active(): 查询当前GC是否处于活跃状态
- **全量GC机制**
  - 自动全量GC：在空间不足时自动触发
  - 支持TLV保留策略的全量GC
- **关键内部结构**
  - nkv_instance_t中的gc_src_sector、gc_src_offset、gc_active、gc_bitmap等字段用于增量GC的状态跟踪与位图去重
- **配置参数**
  - NKV_INCREMENTAL_GC：启用/禁用增量GC
  - NKV_GC_ENTRIES_PER_WRITE：每次写入后迁移的条目数
  - NKV_GC_THRESHOLD_PERCENT：GC触发阈值（使用率百分比）
  - NKV_TLV_RETENTION_ENABLE：启用TLV保留策略

**章节来源**
- [NanoKV.h](file://NanoKV.h#L125-L143)
- [NanoKV.h](file://NanoKV.h#L170-L174)
- [NanoKV_cfg.h](file://NanoKV_cfg.h#L21-L28)

## 架构总览
NanoKV的写入路径会在必要时触发增量GC；同时提供手动步进接口以配合应用调度。下图展示写入与GC的交互流程，包括增量GC和全量GC两种策略。

```mermaid
sequenceDiagram
participant App as "应用"
participant KV as "nkv_set"
participant Impl as "do_incremental_gc"
participant GC as "增量GC子系统"
participant Compact as "全量GC"
participant Flash as "Flash操作"
App->>KV : "写入键值"
KV->>KV : "检测空间/扇区切换"
KV->>Impl : "写入成功后尝试GC"
alt 空闲扇区充足
Impl->>GC : "增量GC分步执行"
GC->>Flash : "迁移有效条目"
else 空闲扇区不足
Impl->>Compact : "全量GC"
Compact->>Flash : "扫描所有扇区"
Compact->>Flash : "迁移保留条目"
end
GC-->>KV : "返回状态"
KV-->>App : "返回写入结果"
```

**图表来源**
- [NanoKV.c](file://NanoKV.c#L927-L929)
- [NanoKV.c](file://NanoKV.c#L865-L882)
- [NanoKV.c](file://NanoKV.c#L498-L590)
- [NanoKV.c](file://NanoKV.c#L612-L718)

## 详细组件分析

### 增量GC控制接口
- **nkv_gc_step(steps)**
  - 功能：按步数推进增量GC，每步处理一个有效条目
  - 行为：若当前未激活且满足启动条件则先启动；随后最多执行steps步
  - 返回：若已无更多工作则返回0；否则返回1
- **nkv_gc_active()**
  - 功能：查询当前GC是否仍在进行中
  - 返回：1表示活跃，0表示空闲

```mermaid
flowchart TD
Start(["调用 nkv_gc_step(steps)"]) --> CheckInit["检查实例是否已初始化"]
CheckInit --> InitOk{"已初始化？"}
InitOk --> |否| Return0["返回0"]
InitOk --> |是| EnsureActive["确保GC已启动"]
EnsureActive --> Active{"当前GC活跃？"}
Active --> |否| TryStart["若需启动则启动"]
TryStart --> Active
Active --> |是| Loop["循环执行最多steps步"]
Loop --> Step["incremental_gc_step()"]
Step --> More{"仍有可处理条目？"}
More --> |是| Loop
More --> |否| Done["返回0无更多工作"]
Loop --> Done
```

**图表来源**
- [NanoKV.c](file://NanoKV.c#L1018-L1038)
- [NanoKV.c](file://NanoKV.c#L654-L718)

**章节来源**
- [NanoKV.h](file://NanoKV.h#L170-L174)
- [NanoKV.c](file://NanoKV.c#L1018-L1038)

### 增量GC内部机制
- **启动条件**
  - 当前未处于GC活跃状态
  - 空闲扇区少于阈值（至少1个可用）
- **启动过程**
  - 选择最旧的非活动扇区作为源
  - 初始化源偏移、位图与活跃标记
- **步进执行**
  - 逐条扫描源扇区的有效条目
  - 对于KV条目，基于键哈希判断是否需要迁移
  - 对于TLV条目，结合保留策略决定是否迁移
  - 将有效且需要保留的条目迁移到新扇区
  - 每步处理完成后返回，允许让出CPU
- **完成与收尾**
  - 源扇区扫描完毕后擦除该扇区
  - 清理活跃标记，若仍无足够空闲扇区则再次启动

```mermaid
flowchart TD
S0["开始"] --> CheckFree["统计空闲扇区数量"]
CheckFree --> NeedStart{"空闲扇区 < 1？"}
NeedStart --> |否| Idle["不启动GC"]
NeedStart --> |是| PickOldest["选择最旧非活动扇区"]
PickOldest --> Init["初始化源扇区/偏移/位图/活跃标记"]
Init --> ScanLoop["扫描源扇区"]
ScanLoop --> Entry{"遇到有效条目？"}
Entry --> |否| Next["偏移前进"]
Entry --> |是| TLVCheck{"是否为TLV条目？"}
TLVCheck --> |是| Retention{"保留策略允许迁移？"}
Retention --> |否| Next
Retention --> |是| KVCheck{"KV条目？"}
TLVCheck --> |否| KVCheck
KVCheck --> |是| HashCheck{"键哈希已在位图？"}
HashCheck --> |是| Next
HashCheck --> |否| Migrate["迁移条目到新扇区"]
Migrate --> Mark["标记哈希并前进"]
Mark --> Next
Next --> ScanLoop
ScanLoop --> Done{"扫描结束？"}
Done --> |否| ScanLoop
Done --> |是| Erase["擦除源扇区"]
Erase --> Clear["清除活跃标记"]
Clear --> CheckAgain{"仍无空闲扇区？"}
CheckAgain --> |是| PickOldest
CheckAgain --> |否| End["结束"]
```

**图表来源**
- [NanoKV.c](file://NanoKV.c#L594-L610)
- [NanoKV.c](file://NanoKV.c#L612-L652)
- [NanoKV.c](file://NanoKV.c#L654-L718)

**章节来源**
- [NanoKV.c](file://NanoKV.c#L594-L610)
- [NanoKV.c](file://NanoKV.c#L612-L652)
- [NanoKV.c](file://NanoKV.c#L654-L718)

### 全量GC机制
- **触发条件**
  - 空闲扇区不足时自动触发
  - 空闲扇区少于NKV_GC_THRESHOLD_PERCENT阈值
- **执行过程**
  - 切换到下一个扇区作为目标
  - 扫描所有扇区的有效条目
  - 应用TLV保留策略过滤条目
  - 基于键哈希去重迁移条目
  - 处理迁移空间不足的情况

```mermaid
flowchart TD
Start["全量GC触发"] --> Switch["切换到下一个扇区"]
Switch --> Iterate["遍历所有扇区"]
Iterate --> CheckEntry{"有效条目？"}
CheckEntry --> |否| NextSector["下一个扇区"]
CheckEntry --> |是| TLVCheck{"TLV条目？"}
TLVCheck --> |是| Retention{"保留策略检查"}
Retention --> |否| NextSector
Retention --> |是| KVCheck{"KV条目？"}
TLVCheck --> |否| KVCheck
KVCheck --> |是| HashCheck{"键哈希去重"}
KVCheck --> |否| NextSector
HashCheck --> |是| Migrate["迁移条目"]
HashCheck --> |否| NextSector
Migrate --> SpaceCheck{"空间充足？"}
SpaceCheck --> |否| SwitchNext["切换到下一个扇区"]
SpaceCheck --> |是| NextSector
NextSector --> Iterate
Iterate --> Complete["完成"]
```

**图表来源**
- [NanoKV.c](file://NanoKV.c#L498-L590)
- [NanoKV.c](file://NanoKV.c#L508-L589)

**章节来源**
- [NanoKV.c](file://NanoKV.c#L498-L590)

### GC状态监控与查询
- **nkv_gc_active()**直接返回内部活跃标志，便于上层调度器了解GC是否正在进行
- **nkv_get_usage()**可评估使用率，辅助判断是否需要主动触发nkv_gc_step()
- **count_free_sectors()**统计空闲扇区数量，用于GC触发决策

**章节来源**
- [NanoKV.h](file://NanoKV.h#L170-L174)
- [NanoKV.c](file://NanoKV.c#L1010-L1016)
- [NanoKV.c](file://NanoKV.c#L594-L602)

### 与其他模块的协调与一致性
- **与写入路径的协作**
  - nkv_set成功写入后会尝试执行增量GC，避免长时间阻塞
  - 若空间不足，优先尝试切换扇区；若无可切换扇区，则执行全量GC
- **与缓存的协调**
  - 写入成功后更新缓存；删除键时清理缓存
- **与TLV保留策略的协同**
  - 增量GC和全量GC在迁移前检查保留策略，确保保留规则生效

**章节来源**
- [NanoKV.c](file://NanoKV.c#L927-L929)
- [NanoKV.c](file://NanoKV.c#L865-L882)
- [NanoKV.c](file://NanoKV.c#L500-L502)
- [NanoKV.c](file://NanoKV.c#L615-L617)

## 依赖关系分析
- **头文件与实现**
  - NanoKV.h声明公共API与实例结构，NanoKV.c提供实现
- **配置依赖**
  - 增量GC开关与步进参数来自NanoKV_cfg.h
  - TLV保留策略配置影响GC行为
- **移植层依赖**
  - Flash操作接口由移植层提供，实现文件通过nkv_flash_ops_t调用

```mermaid
graph LR
Port["移植层接口<br/>NanoKV_port.h/.c"] --> Impl["实现<br/>NanoKV.c"]
Cfg["配置<br/>NanoKV_cfg.h"] --> Impl
Impl --> API["公共API<br/>NanoKV.h"]
Impl --> GC["GC子系统"]
GC --> TLV["TLV保留策略"]
```

**图表来源**
- [NanoKV_port.h](file://NanoKV_port.h#L18-L21)
- [NanoKV_port.c](file://NanoKV_port.c#L43-L51)
- [NanoKV_cfg.h](file://NanoKV_cfg.h#L21-L28)
- [NanoKV.h](file://NanoKV.h#L170-L174)
- [NanoKV.c](file://NanoKV.c#L592-L735)

**章节来源**
- [NanoKV_port.h](file://NanoKV_port.h#L18-L21)
- [NanoKV_port.c](file://NanoKV_port.c#L43-L51)
- [NanoKV_cfg.h](file://NanoKV_cfg.h#L21-L28)
- [NanoKV.h](file://NanoKV.h#L170-L174)
- [NanoKV.c](file://NanoKV.c#L592-L735)

## 性能考量
- **增量GC的优势**
  - 分步执行，避免长时间阻塞，适合实时系统
  - 通过位图避免重复迁移相同键，降低重复工作
  - 支持TLV保留策略，智能过滤不需要迁移的条目
- **影响因素**
  - NKV_GC_ENTRIES_PER_WRITE：每次写入后迁移条目数，越大越快但单次开销越大
  - 使用率阈值：当使用率超过阈值时更易触发GC，建议60%-80%
  - 扇区数量与大小：扇区越多，可并行管理的迁移越灵活
  - TLV保留策略：合理配置可减少不必要的迁移
- **资源消耗**
  - CPU：步进式执行，单步开销主要在读取条目头、键与CRC校验，以及写入新扇区
  - Flash：迁移涉及多次写入与一次擦除；擦除次数与触发频率相关
  - RAM：位图占用固定空间（32字节），实例结构包含源位置与活跃标记

**章节来源**
- [NanoKV_cfg.h](file://NanoKV_cfg.h#L21-L28)
- [NanoKV.c](file://NanoKV.c#L654-L718)
- [NanoKV.c](file://NanoKV.c#L508-L589)

## 故障排除指南
- **常见问题与定位**
  - nkv_gc_step返回0但nkv_gc_active仍为1
    - 可能原因：源扇区扫描已完成但尚未擦除；或迁移过程中发生空间不足导致切换
    - 建议：等待后续步进或主动再次调用nkv_gc_step
  - nkv_gc_active一直为0，但空间不足
    - 可能原因：未满足启动条件（空闲扇区>=1）或未触发
    - 建议：调用nkv_gc_step(steps)强制启动；或降低使用率阈值
  - 迁移异常或写入失败
    - 可能原因：Flash写入/擦除接口实现错误
    - 建议：检查移植层实现与硬件接口
- **调试与监控**
  - 使用nkv_get_usage()观察使用率变化
  - 使用nkv_gc_active()确认GC状态
  - 在移植层开启日志（如NKV_LOG_I/NKV_LOG_E）查看初始化与错误信息

**章节来源**
- [NanoKV.c](file://NanoKV.c#L1034-L1037)
- [NanoKV.c](file://NanoKV.c#L1010-L1016)
- [NanoKV_port.c](file://NanoKV_port.c#L56-L87)

## 结论
NanoKV的垃圾回收系统通过nkv_gc_step与nkv_gc_active提供了可控、低延迟的垃圾回收能力。其分步执行策略与位图去重机制在保证数据一致性的同时，显著降低了单次操作的开销。新增的TLV保留策略进一步增强了系统的智能化水平。结合合理的配置参数与系统调度，可在实时性与空间利用率之间取得良好平衡。

## 附录

### API参考与使用示例
- **nkv_gc_step(steps)**
  - 用途：手动推进增量GC，适合应用侧精确控制
  - 示例场景：在空闲时间片内循环调用，逐步完成GC
  - 参数：steps - 要执行的步数
  - 返回：1表示仍有工作，0表示已完成
- **nkv_gc_active()**
  - 用途：查询GC是否仍在进行，便于上层调度
  - 示例场景：在主循环中周期性检查，决定是否继续执行其他任务
  - 返回：1表示活跃，0表示空闲

**章节来源**
- [NanoKV.h](file://NanoKV.h#L170-L174)
- [NanoKV.c](file://NanoKV.c#L1018-L1038)

### 配置参数与建议
- **NKV_INCREMENTAL_GC**
  - 建议：启用（1），以获得更好的实时性
- **NKV_GC_ENTRIES_PER_WRITE**
  - 建议：1-4，视系统负载与实时性要求调整
- **NKV_GC_THRESHOLD_PERCENT**
  - 建议：60%-80%，避免过早触发导致频繁迁移
- **NKV_TLV_RETENTION_ENABLE**
  - 建议：启用（1），配合TLV保留策略使用

**章节来源**
- [NanoKV_cfg.h](file://NanoKV_cfg.h#L21-L28)

### 系统集成指导
- **写入路径集成**
  - nkv_set成功后自动尝试增量GC；若空间不足，优先切换扇区，再考虑全量GC
- **主循环集成**
  - 在空闲时段调用nkv_gc_step(若干步)，直至nkv_gc_active返回0
- **监控与告警**
  - 定期调用nkv_get_usage()与nkv_gc_active()，结合日志输出进行监控
- **TLV保留策略集成**
  - 使用nkv_tlv_set_retention()配置保留策略，确保重要数据不被GC删除

**章节来源**
- [NanoKV.c](file://NanoKV.c#L927-L929)
- [NanoKV.c](file://NanoKV.c#L1034-L1037)
- [NanoKV.c](file://NanoKV.c#L1010-L1016)
- [NanoKV.c](file://NanoKV.c#L1464-L1486)