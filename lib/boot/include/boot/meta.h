/**
 * @file   meta.h
 * @brief  启动状态记录的持久化对象
 *
 * Meta 对象持有 meta 分区句柄，负责双副本的读取、写入与清理：副本轮转、
 * seq 分配、CRC 校验、擦除与 flags 编解码全部在内部完成。对外只暴露
 * OmBootMetaData 与读取结论，不暴露 32 字节记录布局。
 */

#ifndef __BOOT_META_OBJECT_H__
#define __BOOT_META_OBJECT_H__

#include <stddef.h>
#include <stdint.h>

#include "boot/contract/meta.h"
#include "boot/meta_data.h"
#include "core/om_def.h"
#include "drivers/storage/partition.h"

#ifdef OM_USE_BOOTCFG
#include "om_bootcfg.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** meta 分区副本数。当前固定两份；其他值在编译期拒绝。 */
#ifndef OM_BOOT_META_COPIES
#define OM_BOOT_META_COPIES 2u
#endif

_Static_assert(OM_BOOT_META_COPIES == 2u, "meta 记录当前只支持两份副本");

/** seq 的擦除/无效哨兵，永不写入。 */
#define OM_BOOT_META_SEQ_INVALID 0xFFFFFFFFu

/**
 * @brief 启动状态记录存储对象
 *
 * 持有 meta 分区句柄与每个副本的偏移、大小。句柄按值复制，注册表与器件
 * 须比对象活得久。
 */
typedef struct OmBootMeta {
    OmPartitionHandle partition;              /**< meta 分区句柄 */
    uint32_t copyCount;                       /**< 逻辑副本数 */
    uint32_t copyOffset[OM_BOOT_META_COPIES]; /**< 每个副本的分区内偏移 */
    uint32_t copySize[OM_BOOT_META_COPIES];   /**< 每个副本的擦除单位大小 */
} OmBootMeta;

/**
 * @brief 绑定 meta 分区
 * @param meta      Meta 对象
 * @param partition 已打开的分区句柄
 * @return OM_OK / OM_ERR_INVALID_ARG（参数为空、擦除单位数不足、单位容不下记录）
 */
OmRet om_boot_meta_init(OmBootMeta *meta, const OmPartitionHandle *partition);

/**
 * @brief 读取最新有效记录
 * @param meta Meta 对象
 * @param out  输出读取结论与记录内容，不可为空
 * @return OM_OK（读取完成，out->status 表达结论）/ OM_ERR_INVALID_ARG
 */
OmRet om_boot_meta_read(const OmBootMeta *meta, OmBootMetaReadResult *out);

/**
 * @brief 写入一条新记录
 * @param meta Meta 对象
 * @param data 记录内容；seq、CRC 与目标副本由本函数填写
 * @return OM_OK / OM_ERR_INVALID_ARG / OM_ERR_OVERFLOW / 分区错误
 *
 * 选择副本的顺序：已擦副本优先，其次损坏副本，最后擦除最旧的非最新有效
 * 副本。最新有效副本永不擦除。写入顺序为先写前 28 字节，最后写 CRC。
 */
OmRet om_boot_meta_write(OmBootMeta *meta, const OmBootMetaData *data);

/**
 * @brief 清理非最新副本
 * @param meta Meta 对象
 * @return OM_OK / 分区错误
 *
 * 保留最新有效副本，擦除其余副本；没有有效记录时擦除全部副本。
 */
OmRet om_boot_meta_cleanup(OmBootMeta *meta);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_META_OBJECT_H__ */
