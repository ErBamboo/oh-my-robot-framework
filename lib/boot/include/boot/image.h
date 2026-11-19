/**
 * @file   image.h
 * @brief  镜像对象
 *
 * 镜像对象拥有自己的存储区域、槽身份与校验能力：分区句柄在构造时注入，
 * 校验器作为内部协作者持有。对外提供镜像描述与校验结果，不解释 meta 状态。
 */

#ifndef __BOOT_IMAGE_OBJECT_H__
#define __BOOT_IMAGE_OBJECT_H__

#include <stdint.h>

#include "boot/image_data.h"
#include "boot/image_verifier.h"
#include "core/om_def.h"
#include "drivers/storage/partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 镜像对象。 */
typedef struct OmBootImage {
    OmPartitionHandle partition;         /**< 镜像所在分区，按值复制 */
    uint32_t slotId;                     /**< 槽号，0=A / 1=B */
    const OmBootImageVerifier *verifier; /**< 校验器引用，须比镜像对象活得久 */
} OmBootImage;

/**
 * @brief 初始化镜像对象
 * @param image     镜像对象
 * @param partition 已打开的分区句柄
 * @param slot_id   槽号，0=A / 1=B
 * @param verifier  校验器引用，不可为空
 * @return OM_OK / OM_ERR_INVALID_ARG
 *
 * 镜像对象只保存校验器引用，不复制校验器状态。分区注册表、器件与校验器
 * 须比镜像对象活得久。
 */
OmRet om_boot_image_init(OmBootImage *image, const OmPartitionHandle *partition,
                         uint32_t slot_id, const OmBootImageVerifier *verifier);

/**
 * @brief 校验镜像并输出描述与结论
 * @param image  镜像对象
 * @param result 输出结果，不可为空
 * @return OM_OK（校验完成，result->status 表达结论）/ OM_ERR_INVALID_ARG
 */
OmRet om_boot_image_verify(const OmBootImage *image, OmBootImageResult *result);

/** @brief 槽号；对象不可用时返回 0 */
uint32_t om_boot_image_slot_id(const OmBootImage *image);

/** @brief 分区句柄；对象不可用时返回 NULL */
const OmPartitionHandle *om_boot_image_partition(const OmBootImage *image);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_IMAGE_OBJECT_H__ */
