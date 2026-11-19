/**
 * @file   image_data.h
 * @brief  镜像对象的值类型
 *
 * 定义校验结论、通过校验后的镜像描述以及一次校验的完整结果。这些类型
 * 同时供镜像对象与校验器使用，只依赖标准整数类型。
 */

#ifndef __BOOT_IMAGE_DATA_H__
#define __BOOT_IMAGE_DATA_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 校验结果。判定顺序与镜像契约一致，先头后摘要。 */
typedef enum OmBootImageStatus {
    OM_BOOT_IMAGE_VALID           = 0,  /**< 头与摘要均通过 */
    OM_BOOT_IMAGE_INVALID_ARG     = 1,  /**< 参数为空或槽号越界 */
    OM_BOOT_IMAGE_READ_ERROR      = 2,  /**< 镜像源读取失败 */
    OM_BOOT_IMAGE_BLANK           = 3,  /**< 魔数为擦除值，视为空白槽 */
    OM_BOOT_IMAGE_BAD_MAGIC       = 4,  /**< 魔数不符 */
    OM_BOOT_IMAGE_BAD_VERSION     = 5,  /**< 头布局版本不认识 */
    OM_BOOT_IMAGE_BAD_HEADER_SIZE = 6,  /**< 头长字段与当前结构不符 */
    OM_BOOT_IMAGE_NOT_BOOTABLE    = 7,  /**< 头 flags 的 NON_BOOTABLE 位置位 */
    OM_BOOT_IMAGE_BAD_SIZES       = 8,  /**< 尺寸字段不自洽或超出容器容量 */
    OM_BOOT_IMAGE_BAD_OFFSET      = 9,  /**< 负载偏移未对齐或小于头长 */
    OM_BOOT_IMAGE_BAD_DIGEST_ALGO = 10, /**< 校验算法未注册 */
    OM_BOOT_IMAGE_BAD_DIGEST_LEN  = 11, /**< 摘要长度与算法不符 */
    OM_BOOT_IMAGE_BAD_RESERVED    = 12, /**< 保留区不是约定的擦除值 */
    OM_BOOT_IMAGE_SLOT_MISMATCH   = 13, /**< 槽号与所在槽不符 */
    OM_BOOT_IMAGE_DIGEST_MISMATCH = 14, /**< 摘要不一致 */
    OM_BOOT_IMAGE_DIGEST_ERROR    = 15, /**< 校验算法执行失败 */
} OmBootImageStatus;

/** 通过校验后可供决策使用的镜像属性。 */
typedef struct OmBootImageInfo {
    uint32_t imageVersion;   /**< 镜像版本 */
    uint32_t flags;          /**< 镜像标志位 */
    uint32_t payloadOffset;  /**< 负载相对槽基址的偏移 */
    uint32_t imageSize;      /**< 负载长度 */
    uint32_t imageTotalSize; /**< 头、填充、负载与摘要区的总长 */
} OmBootImageInfo;

/**
 * @brief 一次校验的完整结果
 *
 * status 为 VALID 时 info 整体有效，表示可启动镜像的属性；其他状态下
 * info 无意义，调用方不得读取。失败诊断只使用 status，不复用 info。
 */
typedef struct OmBootImageResult {
    OmBootImageStatus status; /**< 校验结论 */
    OmBootImageInfo info;     /**< 仅 VALID 时有效 */
} OmBootImageResult;

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_IMAGE_DATA_H__ */
