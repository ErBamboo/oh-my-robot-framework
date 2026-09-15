/**
 * @file   crc32.h
 * @brief  CRC-32/ISO-HDLC 计算原语（反射多项式，初值与末尾异或全 1）
 *
 * 设计思想：
 * - **只提供一个标准变体，不做多项式/反射参数化**：跨实现一致性要求"同一
 *   标识在所有平台上产出逐字节相同的结果"，可配置参数会把一致性变成调用方
 *   义务，而校验值不一致是静默失败（两侧都"算完了"，只是对不上）。
 * - 支持**分段续算**（crc 传入上次返回值），大块数据可边读边算，无需整块缓冲。
 * - 本实现为可移植版本；平台侧可用硬件外设加速，但必须映射回本变体的口径
 *   （软件侧结果为准）。
 *
 * 自检向量：om_crc32_iso_hdlc(0, "123456789", 9) == 0xCBF43926
 *
 * 裁剪姿态：可裁剪组件（消费才引入）。纯计算、零依赖、无动态内存、无 OS 调用。
 */

#ifndef __ALGORITHM_CRC32_H__
#define __ALGORITHM_CRC32_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 首次调用时传入的初值。 */
#define OM_CRC32_INIT 0u

/**
 * @brief 计算 CRC-32/ISO-HDLC，支持分段续算
 * @param crc  上一段的结果；首段传 OM_CRC32_INIT
 * @param data 数据块
 * @param len  字节数
 * @return 累计校验值（可直接作为下一段的 crc 入参）
 *
 * 对同一串字节按任意方式分段计算，结果与一次性计算结果一致；与
 * zlib.crc32（同初值）逐位相同。
 */
uint32_t om_crc32_iso_hdlc(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __ALGORITHM_CRC32_H__ */
