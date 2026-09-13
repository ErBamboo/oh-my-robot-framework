/**
 * @file   partition.h
 * @brief  分区表抽象——可擦存储器件族的上层语义（boot/OTA/存储上层消费面）
 *
 * 族边界：本模块服务可擦存储族（erase 语义 = 区域生命周期操作）。随机器件族
 * （EEPROM/FRAM）与块设备族（SD/eMMC）的分区形态另属（命名窗口 / GPT 式），
 * 不并入本抽象；免擦可擦器件（MRAM 类）经 erase 缺省语义并入。
 *
 * 裁剪姿态：可裁剪组件（非必备）——boot/OTA/存储上层才消费；静态库
 * "无引用不抽取"天然裁剪（无独立开关）。依赖面 = flash 设备族同步面，
 * 与 OM_FLASH_SYNC_ONLY / osal-none 裁剪组合兼容（本模块零 osal 依赖）。
 *
 * 形态（v2）：注册表由调用方持有（可整体 const、可驻 ROM 或调用方 RAM），
 * 句柄由 om_partition_open 产出。**本模块零状态**——无私有全局、无 init
 * 顺序依赖、无并发保护需求（全部 API 天然可重入）。
 *
 * 信任模型（v2，与 v1 的差异见设计文档 §9）：句柄内的 index 在操作期受
 * `index < reg->count` 域校验约束，offset/size 恒从表取（调用方改不动）；
 * 几何正确性由 open 期按分区校验 + 可选的全表 registry_validate 保证。
 * 本模型与 Zephyr flash_area / MCUboot flash_area 同形（两者均无来源校验），
 * 且额外保留 open 期几何校验。
 *
 * 错误码约定：注册表**作为入参被检验**（registry_validate / registry_at）→
 * OM_ERR_INVALID_ARG；**注册表本身不可用/为空**（reg NULL、table NULL、
 * count==0）→ 空表语义：query / open 返回 OM_ERR_NOT_FOUND；名字未命中亦
 * OM_ERR_NOT_FOUND（例外：open 的表中存在畸形条目时改报 OM_ERR_INVALID_ARG，
 * 见其 @return）；registry_at 的 index >= count 同报 OM_ERR_NOT_FOUND。
 *
 * 偏移语义：便捷层 off 一律为分区内偏移；越界返回 OM_ERR_INVALID_ARG。
 * 对齐语义：erase/erase_range 的扇区对齐由底层器件访问层强制（配置错误在
 * 调用期显式报错，不静默波及邻区）。
 */

#ifndef OM_PARTITION_H
#define OM_PARTITION_H

#include <stddef.h>
#include <stdint.h>

#include "core/om_def.h"

typedef struct OmPartitionEntry {
    const char *name;    /* 逻辑名（表内唯一）——字符串本体在表内，只读 */
    const char *devName; /* 器件名（flash0…） */
    uint32_t offset;     /* 器件内偏移 */
    uint32_t size;       /* 分区大小 */
} OmPartitionEntry;

/** 注册表：表 + 条目数。可整体 const（ROM 常量）；表可指向 ROM 或调用方 RAM */
typedef struct OmPartitionRegistry {
    const OmPartitionEntry *table; /* 模块只读该表 */
    uint32_t count;
} OmPartitionRegistry;

/**
 * 编译期常量注册表：免运行期注册。
 * @warning 实参**必须是数组**（勿传指针）：传指针时 sizeof 比值退化为 0，
 *          得到空表语义（首次 open 即 NOT_FOUND）——响亮失败，不会越界。
 */
#define OM_PARTITION_REGISTRY(table_) \
    {(table_), (uint32_t)(sizeof(table_) / sizeof((table_)[0]))}

/** 句柄：open 产出，数据通路的唯一入口。调用方持有，不得手改字段
 *  @note 句柄按指针引用注册表——注册表对象须比它派生的任何句柄活得更久
 *        （栈上注册表返回后，其句柄即悬垂） */
typedef struct OmPartitionHandle {
    const OmPartitionRegistry *reg; /* 所属注册表 */
    uint32_t index;                 /* 操作期校验 index < reg->count */
} OmPartitionHandle;

/**
 * @brief 全表校验（可选，fail-fast）：结构 + 几何
 * @param reg 注册表
 * @return OM_OK / OM_ERR_INVALID_ARG（reg 空、count==0、条目字段非法、重名、
 *         或**器件已注册时**越容量/非扇区友好）
 * @note 器件未注册的条目跳过几何校验（顺序解耦）——与 v1 注册期语义一致；
 *       这些条目的几何正确性由 open 期兜底。应用在上电初始化调用；
 *       引导程序亦建议调用（配置错就停住的价值最高）。
 */
OmRet om_partition_registry_validate(const OmPartitionRegistry *reg);

/** @brief 条目数（reg 为 NULL 时返回 0） */
uint32_t om_partition_registry_count(const OmPartitionRegistry *reg);

/**
 * @brief 按索引取条目（枚举用，by-value 拷贝）
 * @return OM_OK / OM_ERR_INVALID_ARG / OM_ERR_NOT_FOUND（index 越界）
 */
OmRet om_partition_registry_at(const OmPartitionRegistry *reg, uint32_t index,
                               OmPartitionEntry *out);

/**
 * @brief 按名查询（by-value 纯信息，**不碰器件**，沿用 v1 语义）
 * @return OM_OK / OM_ERR_NOT_FOUND（含空表语义）/ OM_ERR_INVALID_ARG
 */
OmRet om_partition_query(const OmPartitionRegistry *reg, const char *name,
                         OmPartitionEntry *out);

/**
 * @brief 解析分区为句柄（名字 + 器件解析 + 几何校验）
 * @return OM_OK / OM_ERR_NOT_FOUND（名字未命中 / 器件不存在 / 空表）
 *         / OM_ERR_INVALID_ARG（越器件容量 / 非扇区友好 / 未命中且表中存在
 *         畸形条目 name==NULL）
 * @note open 不是全表校验器：首个名字命中即返回——表中别处存在畸形条目时，
 *       命中项仍返回 OM_OK；全表 fail-fast 是 registry_validate 的职责
 */
OmRet om_partition_open(const OmPartitionRegistry *reg, const char *name,
                        OmPartitionHandle *h);

/**
 * @brief 分区内偏移读（句柄域校验 + 双端越界校验）
 * @note len == 0 亦为合法调用（off 仍须在域内、buf 仍须非空）；与 erase_range
 *       的 len==0 无操作语义不同。write 同契约
 */
OmRet om_partition_read(const OmPartitionHandle *h, uint32_t off, void *buf, size_t len);

/** @brief 分区内偏移写（同 read 契约） */
OmRet om_partition_write(const OmPartitionHandle *h, uint32_t off, const void *data, size_t len);

/** @brief 整分区擦除（扇区对齐由器件层强制，配置错误显式报错） */
OmRet om_partition_erase(const OmPartitionHandle *h);

/**
 * @brief 分区内按范围擦除（扇区对齐由器件层强制，不静默扩擦）
 * @note len == 0 为无操作（返回 OM_OK）；句柄有效性仍先行校验
 */
OmRet om_partition_erase_range(const OmPartitionHandle *h, uint32_t off, size_t len);

#endif /* OM_PARTITION_H */
