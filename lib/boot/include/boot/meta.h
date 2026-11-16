/**
 * @file   meta.h
 * @brief  启动决策数据 entry 的读写与轮转
 *
 * 决策数据是启动决策的权威记录：当前活动槽、启动状态、启动计数、镜像版本、
 * 布局指纹与失败原因。entry 定长 32 字节，整条写入；CRC 覆盖决策字段，
 * 最后写。
 *
 * 决策区由若干个独立的擦除单位组成，每个单位头部放一条 entry。写新 entry
 * 前，若有已擦单位则直接写；否则擦除非最新单位，保留最新有效 entry。擦除后
 * 立即写入，掉电时只可能损坏目标单位，最新有效 entry 仍然可读。
 *
 * 本模块不解释布局地址：擦除单位由分区层给出，entry 只认分区内偏移。
 */

#ifndef __BOOT_META_H__
#define __BOOT_META_H__

#include <stddef.h>
#include <stdint.h>

#include "core/om_def.h"
#include "drivers/storage/partition.h"

#ifdef OM_USE_BOOTCFG
#include "om_bootcfg.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** 决策副本数。默认两份，工程可在 bootcfg 中覆写。 */
#ifndef OM_BOOT_META_COPIES
#define OM_BOOT_META_COPIES 2u
#endif

_Static_assert(OM_BOOT_META_COPIES >= 2u, "决策数据至少需要两份副本");

/** seq 的擦除/无效哨兵，永不写入。 */
#define OM_BOOT_META_SEQ_INVALID 0xFFFFFFFFu

/** entry 标志：本次写入由确认触发。 */
#define OM_BOOT_META_FLAG_CONFIRMED 0x00000001u

/** entry 标志：布局指纹位于高 16 位。 */
#define OM_BOOT_META_FLAG_FINGERPRINT_SHIFT 16u
#define OM_BOOT_META_FLAG_FINGERPRINT_MASK  0xFFFF0000u
#define OM_BOOT_META_FLAG_RESERVED_MASK     0x0000FFFEu

/** 启动状态。EMPTY 只用于描述空白 entry，正常写入从 PENDING 开始。 */
typedef enum OmBootMetaState {
    OM_BOOT_META_STATE_EMPTY   = 0, /**< 空白，无有效决策 */
    OM_BOOT_META_STATE_PENDING = 1, /**< 待启动，启动计数推进 */
    OM_BOOT_META_STATE_VALID   = 2, /**< 已确认，稳态不再写 */
    OM_BOOT_META_STATE_ABORTED = 3, /**< 已回退或放弃 */
} OmBootMetaState;

/** 失败原因。枚举与失败模式矩阵一一对应。 */
typedef enum OmBootMetaReason {
    OM_BOOT_META_REASON_NONE                 = 0, /**< 无失败 */
    OM_BOOT_META_REASON_DOWNLOAD_INTERRUPTED = 1, /**< 下载中断 */
    OM_BOOT_META_REASON_HEADER_INVALID       = 2, /**< 镜像头无效 */
    OM_BOOT_META_REASON_DIGEST_FAILED        = 3, /**< 摘要校验失败 */
    OM_BOOT_META_REASON_COMMIT_FAILED        = 4, /**< 提交擦写失败 */
    OM_BOOT_META_REASON_ROLLBACK_LIMIT       = 5, /**< 达启动次数上限回退 */
    OM_BOOT_META_REASON_NO_VALID_IMAGE       = 6, /**< 两槽均无有效镜像 */
    OM_BOOT_META_REASON_GEOMETRY_MISMATCH    = 7, /**< 布局或几何不匹配 */
    OM_BOOT_META_REASON_SIGNATURE_FAILED     = 8, /**< 签名校验失败 */
} OmBootMetaReason;

/**
 * @brief 决策 entry，32 字节定长
 *
 * 字段小端、自然对齐。CRC-32/ISO-HDLC 覆盖 seq 到 reason 的 28 字节，
 * crc32 自身不参与。
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
 * @brief 决策数据上下文
 *
 * 持有一个分区句柄和每个副本的偏移与大小。分区句柄与注册表须比上下文活得久。
 */
typedef struct OmBootMeta {
    OmPartitionHandle partition;              /**< 决策分区句柄，须比上下文活得久 */
    uint32_t copyCount;                       /**< 逻辑副本数，等于 OM_BOOT_META_COPIES */
    uint32_t copyOffset[OM_BOOT_META_COPIES]; /**< 每个副本的分区内偏移 */
    uint32_t copySize[OM_BOOT_META_COPIES];   /**< 每个副本的擦除单位大小 */
} OmBootMeta;

/**
 * @brief 绑定决策分区
 * @param meta      上下文
 * @param partition 已打开的决策分区句柄（meta 或 meta0 等，由工程布局给出）
 * @return OM_OK / OM_ERR_INVALID_ARG（参数为空、擦除单位数少于 OM_BOOT_META_COPIES、
 *         单位容不下 entry）
 * @note 模块使用分区的前 OM_BOOT_META_COPIES 个擦除单位；分区可以更大，多余单位
 *       由工程保留，不参与读写与校验。每个擦除单位是一份副本，entry 放在单位头部；
 *       分区层负责扇区对齐。
 */
OmRet om_boot_meta_init(OmBootMeta *meta, const OmPartitionHandle *partition);

/**
 * @brief 读取最新有效 entry
 * @param meta       上下文
 * @param entry      输出 entry
 * @param copy_index 输出副本序号，可为 NULL
 * @return OM_OK / OM_ERR_NOT_FOUND（没有有效 entry）/ OM_ERR_INVALID_ARG / 分区错误
 */
OmRet om_boot_meta_read(const OmBootMeta *meta, OmBootMetaEntry *entry, uint32_t *copy_index);

/**
 * @brief 写入一条新 entry
 * @param meta  上下文
 * @param entry 输入字段；seq 与 crc32 由本函数填写
 * @return OM_OK / OM_ERR_INVALID_ARG / OM_ERR_OVERFLOW（seq 耗尽）/ 分区错误
 *
 * 写入前选择目标副本：优先已擦副本；否则擦除非最新副本。永不擦最新有效副本。
 * 写入顺序为前 28 字节、再写 CRC；CRC 未写完时该 entry 无效。
 */
OmRet om_boot_meta_write(OmBootMeta *meta, OmBootMetaEntry *entry);

/**
 * @brief 清理非最新副本
 * @param meta 上下文
 * @return OM_OK / 分区错误
 *
 * 保留最新有效 entry，擦除其余副本；没有有效 entry 时擦除全部副本。
 * 引导期调用一次即可维持至少一份已擦副本。
 */
OmRet om_boot_meta_cleanup(OmBootMeta *meta);

/**
 * @brief 判断 entry 是否有效
 * @param entry entry
 * @return OM_TRUE 表示 seq 非哨兵、CRC 正确、状态与槽号在范围内
 */
OmBool om_boot_meta_entry_valid(const OmBootMetaEntry *entry);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_META_H__ */
