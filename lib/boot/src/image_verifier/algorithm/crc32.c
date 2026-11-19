/**
 * @file   crc32.c
 * @brief  CRC-32/ISO-HDLC 摘要算法实现
 */

#include "crc32.h"

#include "algorithm/checksum/crc32.h"

OmRet om_boot_verify_crc32_begin(void *ctx)
{
    if (ctx == NULL)
    {
        return OM_ERR_INVALID_ARG;
    }
    ((OmBootVerifyCrc32Context *)ctx)->crc = OM_CRC32_INIT;
    return OM_OK;
}

OmRet om_boot_verify_crc32_update(void *ctx, const uint8_t *data, uint32_t len)
{
    OmBootVerifyCrc32Context *crc_ctx = (OmBootVerifyCrc32Context *)ctx; /* CRC32 上下文 */

    if (crc_ctx == NULL || (data == NULL && len > 0u))
    {
        return OM_ERR_INVALID_ARG;
    }
    crc_ctx->crc = om_crc32_iso_hdlc(crc_ctx->crc, data, len);
    return OM_OK;
}

OmRet om_boot_verify_crc32_finish(void *ctx, uint8_t *digest)
{
    uint32_t crc; /* 最终校验值 */

    if (ctx == NULL || digest == NULL)
    {
        return OM_ERR_INVALID_ARG;
    }
    crc = ((OmBootVerifyCrc32Context *)ctx)->crc;
    digest[0] = (uint8_t)(crc & 0xFFu);
    digest[1] = (uint8_t)((crc >> 8) & 0xFFu);
    digest[2] = (uint8_t)((crc >> 16) & 0xFFu);
    digest[3] = (uint8_t)((crc >> 24) & 0xFFu);
    return OM_OK;
}
