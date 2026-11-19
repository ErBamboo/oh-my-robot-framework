/**
 * @file   image_verifier.h
 * @brief  镜像校验器
 *
 * 校验器持有校验算法表，对镜像源执行完整性校验：先读头并校验字段自洽，
 * 再按头声明的算法流式计算摘要。校验器拥有完整的校验协议，调用方不需要
 * 自行安排读取顺序。
 *
 * 校验器不持有分区，也不认识槽；它由镜像对象持有引用，输出与槽无关的
 * 校验结论。校验算法以算法表注入，新增算法只需实现算法描述符。
 */

#ifndef __BOOT_IMAGE_VERIFIER_H__
#define __BOOT_IMAGE_VERIFIER_H__

#include <stdint.h>

#include "boot/image_data.h"
#include "core/om_def.h"
#include "drivers/storage/partition.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 校验算法描述符
 *
 * 一条描述符绑定一种镜像头可声明的校验算法：算法标识、摘要长度、实现方
 * 持有的上下文与计算操作。框架只把 context 透传给回调，不分配、不对齐、
 * 不校验其内容；上下文的生命周期与并发策略由实现方决定。
 */
typedef struct OmBootImageVerifyAlgorithm {
    uint16_t algo;                                                     /**< 镜像头 digestAlgo 取值 */
    uint16_t digestLen;                                                /**< 该算法要求的摘要长度 */
    void *context;                                                     /**< 实现方上下文，可为 NULL */
    OmRet (*begin)(void *context);                                     /**< 初始化上下文 */
    OmRet (*update)(void *context, const uint8_t *data, uint32_t len); /**< 累计一块数据 */
    OmRet (*finish)(void *context, uint8_t *digest);                   /**< 输出 digestLen 字节摘要 */
} OmBootImageVerifyAlgorithm;

/** 镜像校验器：持有校验算法表，按镜像契约执行校验。 */
typedef struct OmBootImageVerifier {
    const OmBootImageVerifyAlgorithm *algorithms; /**< 算法表，须比校验器活得久 */
    uint32_t algorithmCount;                      /**< 算法表项数 */
} OmBootImageVerifier;

/**
 * @brief 取内置校验算法表
 * @param count 输出表项数，可为 NULL
 * @return 算法表首地址，始终非空
 *
 * 内置表当前包含 CRC-32/ISO-HDLC。新增算法时在此表中补充描述符。
 */
const OmBootImageVerifyAlgorithm *om_boot_image_verify_algorithms(uint32_t *count);

/**
 * @brief 初始化校验器
 * @param verifier   校验器
 * @param algorithms 校验算法表，须比校验器活得久
 * @param count      算法表项数
 * @return OM_OK / OM_ERR_INVALID_ARG
 *
 * 算法表由调用方注入，测试可替换为假算法。每项要求操作齐全、摘要长度
 * 不超过摘要区容量。
 */
OmRet om_boot_image_verifier_init(OmBootImageVerifier *verifier,
                                  const OmBootImageVerifyAlgorithm *algorithms,
                                  uint32_t count);

/**
 * @brief 校验镜像源
 * @param verifier     校验器
 * @param source       镜像源分区，只读
 * @param expected_slot 期望槽号（0=A / 1=B），用于槽绑定校验
 * @param result       输出完整结果，不可为空
 * @return OM_OK（校验完成，result->status 表达结论）/ OM_ERR_INVALID_ARG
 *
 * 不修改校验器与镜像源。参数错误时 result->status 置 INVALID_ARG 并返回
 * OM_ERR_INVALID_ARG；其余情况下返回 OM_OK，由 result->status 表达读取、
 * 头部、算法与摘要的结论。
 */
OmRet om_boot_image_verifier_verify(const OmBootImageVerifier *verifier,
                                    const OmPartitionHandle *source,
                                    uint32_t expected_slot,
                                    OmBootImageResult *result);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_IMAGE_VERIFIER_H__ */
