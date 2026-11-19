/**
 * @file   meta.h
 * @brief  启动状态记录（meta）契约
 *
 * 定义持久化的启动状态记录及其状态、原因与标志位语义。记录是引导方与槽
 * 应用之间交换启动状态的唯一格式；记录如何落盘与轮转由存储模块负责，
 * 记录如何迁移由决策模块负责。
 */

#ifndef __BOOT_CONTRACT_META_H__
#define __BOOT_CONTRACT_META_H__

#include <stddef.h>
#include <stdint.h>

#include "core/om_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 槽数量。 */
#define OM_BOOT_SLOT_COUNT 2u

/*
 * flags 位布局：
 *   bit0     本次写入由确认触发；只描述这次写入，不表示槽状态
 *   bit1     槽 A 曾经启动成功并确认
 *   bit2     槽 B 曾经启动成功并确认
 *   bit3     当前 PENDING 是达限回退产生的试用；控制位，不是诊断信息
 *   bit4..15 保留
 *   bit16..31 布局指纹
 * 两个确认位按槽保存：切换到另一个槽后，原槽是否曾经可用仍是回退决策的
 * 输入。位只记录"启动成功过"，不记录镜像身份。
 */

/** 记录标志：本次写入由确认触发。 */
#define OM_BOOT_META_FLAG_CONFIRMED 0x00000001u

/** 记录标志：槽 A 曾经启动成功并确认。 */
#define OM_BOOT_META_FLAG_SLOT_A_CONFIRMED 0x00000002u

/** 记录标志：槽 B 曾经启动成功并确认。 */
#define OM_BOOT_META_FLAG_SLOT_B_CONFIRMED 0x00000004u

/** 记录标志：当前 PENDING 是达限回退产生的试用。 */
#define OM_BOOT_META_FLAG_TRIAL_FROM_ROLLBACK 0x00000008u

/** 两个槽的确认位。 */
#define OM_BOOT_META_FLAG_SLOT_CONFIRMED_MASK \
    (OM_BOOT_META_FLAG_SLOT_A_CONFIRMED | OM_BOOT_META_FLAG_SLOT_B_CONFIRMED)

/** 按槽号取确认位；槽号只允许 0 或 1。 */
#define OM_BOOT_META_FLAG_SLOT_CONFIRMED(slot) \
    (((slot) == 0u) ? OM_BOOT_META_FLAG_SLOT_A_CONFIRMED : OM_BOOT_META_FLAG_SLOT_B_CONFIRMED)

/** 描述状态迁移历史的标志位；写入新记录时按迁移规则保留或清除。 */
#define OM_BOOT_META_FLAG_STATE_MASK \
    (OM_BOOT_META_FLAG_SLOT_CONFIRMED_MASK | OM_BOOT_META_FLAG_TRIAL_FROM_ROLLBACK)

/** 记录标志：布局指纹位于高 16 位。 */
#define OM_BOOT_META_FLAG_FINGERPRINT_SHIFT 16u
#define OM_BOOT_META_FLAG_FINGERPRINT_MASK  0xFFFF0000u
#define OM_BOOT_META_FLAG_RESERVED_MASK     0x0000FFF0u

/** 启动状态。EMPTY 只用于描述空白记录，正常写入从 PENDING 开始。 */
typedef enum OmBootMetaState {
    OM_BOOT_META_STATE_EMPTY   = 0, /**< 空白，无有效状态 */
    OM_BOOT_META_STATE_PENDING = 1, /**< 待启动，启动计数推进 */
    OM_BOOT_META_STATE_VALID   = 2, /**< 已确认，稳态不再写 */
    OM_BOOT_META_STATE_ABORTED = 3, /**< 已回退或放弃 */
} OmBootMetaState;

/** 失败原因。枚举与失败模式矩阵一一对应。 */
typedef enum OmBootMetaReason {
    OM_BOOT_META_REASON_NONE                 = 0,  /**< 无失败 */
    OM_BOOT_META_REASON_DOWNLOAD_INTERRUPTED = 1,  /**< 下载中断 */
    OM_BOOT_META_REASON_HEADER_INVALID       = 2,  /**< 镜像头无效 */
    OM_BOOT_META_REASON_DIGEST_FAILED        = 3,  /**< 摘要校验失败 */
    OM_BOOT_META_REASON_COMMIT_FAILED        = 4,  /**< 提交擦写失败 */
    OM_BOOT_META_REASON_ROLLBACK_LIMIT       = 5,  /**< 达启动次数上限回退 */
    OM_BOOT_META_REASON_NO_VALID_IMAGE       = 6,  /**< 两槽均无有效镜像 */
    OM_BOOT_META_REASON_GEOMETRY_MISMATCH    = 7,  /**< 布局或几何不匹配 */
    OM_BOOT_META_REASON_SIGNATURE_FAILED     = 8,  /**< 签名校验失败 */
    OM_BOOT_META_REASON_META_CORRUPT         = 9,  /**< 决策数据损坏 */
    OM_BOOT_META_REASON_META_READ_FAILED     = 10, /**< 决策数据读取失败 */
    OM_BOOT_META_REASON_SLOT_READ_FAILED     = 11, /**< 槽读取失败 */
} OmBootMetaReason;

/**
 * @brief 启动状态记录，32 字节定长
 *
 * 字段小端、自然对齐。CRC-32/ISO-HDLC 覆盖 seq 到 reason 的 28 字节，
 * crc32 自身不参与；落盘完整性由存储模块校验。
 */
typedef struct OmBootMetaEntry {
    uint32_t seq;          /**< 0  单调递增；0xFFFFFFFF 为无效哨兵 */
    uint32_t activeSlot;   /**< 4  当前应启动的槽，0=A / 1=B */
    uint32_t state;        /**< 8  OmBootMetaState */
    uint32_t bootCount;    /**< 12 未确认窗口内的已启动次数 */
    uint32_t imageVersion; /**< 16 所选槽的镜像版本快照 */
    uint32_t flags;        /**< 20 标志位与布局指纹 */
    uint32_t reason;       /**< 24 OmBootMetaReason */
    uint32_t crc32;        /**< 28 前 28 字节的 CRC，最后写 */
} OmBootMetaEntry;

_Static_assert(sizeof(OmBootMetaEntry) == 32u, "OmBootMetaEntry 必须为 32 字节");
_Static_assert(offsetof(OmBootMetaEntry, seq) == 0u, "字段偏移与契约不符");
_Static_assert(offsetof(OmBootMetaEntry, activeSlot) == 4u, "字段偏移与契约不符");
_Static_assert(offsetof(OmBootMetaEntry, state) == 8u, "字段偏移与契约不符");
_Static_assert(offsetof(OmBootMetaEntry, bootCount) == 12u, "字段偏移与契约不符");
_Static_assert(offsetof(OmBootMetaEntry, imageVersion) == 16u, "字段偏移与契约不符");
_Static_assert(offsetof(OmBootMetaEntry, flags) == 20u, "字段偏移与契约不符");
_Static_assert(offsetof(OmBootMetaEntry, reason) == 24u, "字段偏移与契约不符");
_Static_assert(offsetof(OmBootMetaEntry, crc32) == 28u, "字段偏移与契约不符");

/**
 * @brief 判断记录的状态字段是否符合契约
 * @param entry 记录
 * @return OM_TRUE 表示字段在合法范围内
 *
 * 只检查状态与槽号范围，不检查 seq 与 CRC；落盘完整性由存储模块校验。
 * 原因字段不参与校验，允许后续版本增加新原因而不让旧引导方把记录判为损坏。
 */
OmBool om_boot_meta_entry_schema_valid(const OmBootMetaEntry *entry);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_CONTRACT_META_H__ */
