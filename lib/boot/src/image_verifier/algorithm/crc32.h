/**
 * @file   crc32.h
 * @brief  CRC-32/ISO-HDLC 摘要算法实现
 *
 * 供镜像校验器的内置算法表引用，不属于公开 API。算法实现提供流式
 * 上下文类型与计算操作；上下文由算法表或调用方持有，其生命周期与
 * 并发策略由实现方决定。
 */

#ifndef __BOOT_DIGEST_CRC32_H__
#define __BOOT_DIGEST_CRC32_H__

#include <stdint.h>

#include "boot/image_verifier.h"

#ifdef __cplusplus
extern "C" {
#endif

/** CRC-32/ISO-HDLC 的摘要长度。 */
#define OM_BOOT_VERIFY_CRC32_LEN 4u

/** CRC-32/ISO-HDLC 的流式上下文。 */
typedef struct OmBootVerifyCrc32Context {
    uint32_t crc; /**< 累计校验值 */
} OmBootVerifyCrc32Context;

/** 初始化上下文。 */
OmRet om_boot_verify_crc32_begin(void *ctx);

/** 累计一块数据。 */
OmRet om_boot_verify_crc32_update(void *ctx, const uint8_t *data, uint32_t len);

/** 输出小端序摘要值。 */
OmRet om_boot_verify_crc32_finish(void *ctx, uint8_t *digest);

#ifdef __cplusplus
}
#endif

#endif /* __BOOT_DIGEST_CRC32_H__ */
