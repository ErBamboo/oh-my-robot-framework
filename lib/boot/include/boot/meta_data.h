/**
 * @file   meta_data.h
 * @brief  Meta 对象的值类型
 *
 * 记录内容与读取结论分开表达：OmBootMetaData 回答“记录是什么”，
 * OmBootMetaReadStatus 回答“本次读取有没有拿到可用记录”。读取结果平铺
 * 保存四组字段，data 仅在 status 为 OK 时有效。
 */

#ifndef __BOOT_META_DATA_H__
#define __BOOT_META_DATA_H__

#include <stdint.h>

#include "boot/contract/meta.h"
#include "core/om_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一次读取的存储层结论。 */
typedef enum OmBootMetaReadStatus {
    OM_BOOT_META_READ_OK        = 0, /**< 找到最新有效记录 */
    OM_BOOT_META_READ_NO_RECORD = 1, /**< 全部副本为擦除态 */
    OM_BOOT_META_READ_DAMAGED   = 2, /**< 无有效记录，存在非擦除的损坏副本 */
    OM_BOOT_META_READ_IO_ERROR  = 3, /**< 至少一个副本读取失败，无法判定 */
} OmBootMetaReadStatus;

/** 一条启动状态记录的内容。 */
typedef struct OmBootMetaData {
    OmBootMetaState state;                /**< PENDING / VALID / ABORTED */
    uint32_t activeSlot;                  /**< 当前活动槽，0=A / 1=B */
    uint32_t bootCount;                   /**< 未确认窗口内已启动次数 */
    uint32_t imageVersion;                /**< 活动槽的镜像版本快照 */
    OmBool confirmed[OM_BOOT_SLOT_COUNT]; /**< 按槽确认位 */
    OmBool trialFromRollback;             /**< 当前 PENDING 由达限回退产生 */
    OmBool confirmedWrite;                /**< 本次写入由确认触发 */
    uint32_t layoutFingerprint;           /**< 记录所属布局的 16 位指纹 */
    OmBootMetaReason reason;              /**< 记录迁移原因 */
} OmBootMetaData;

/** 一条读取到的记录：存储位置、序号与解码后的内容一起有效。 */
typedef struct OmBootMetaRecord {
    uint32_t copyIndex;  /**< 记录所在副本 */
    uint32_t seq;        /**< 记录序号 */
    OmBootMetaData data; /**< 解码后的记录内容 */
} OmBootMetaRecord;

/**
 * @brief Meta 读取结果
 *
 * status 为 OM_BOOT_META_READ_OK 时 record 整体有效；其他状态下 record
 * 无意义。读取操作成功完成时返回 OM_OK，由 status 表达 NO_RECORD、DAMAGED
 * 与 IO_ERROR；OM_ERR_INVALID_ARG 只用于参数错误。
 */
typedef struct OmBootMetaReadResult {
    OmBootMetaReadStatus status;
    OmBootMetaRecord record; /**< 仅 OK 时有效 */
} OmBootMetaReadResult;

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_META_DATA_H__ */
