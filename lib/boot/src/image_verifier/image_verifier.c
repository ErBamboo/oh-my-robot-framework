/**
 * @file   image_verifier.c
 * @brief  镜像校验器实现
 */

#include "boot/image_verifier.h"

#include <string.h>

#include "algorithm/crc32.h"
#include "boot/contract/image.h"

/*
 * 校验顺序不能随意调整：头内尺寸字段在被验证之前不可信，任何基于它们的
 * 地址访问都必须放在尺寸自洽检查之后。摘要最后算，因为摘要覆盖头本身，
 * 头部任一字段的改动都必须先通过结构检查再进入摘要比对。
 */

/** 摘要计算的分块大小；只为避免整镜像缓冲。 */
#define IMAGE_VERIFIER_CHUNK_SIZE 256u

/** 内置 CRC32 算法的上下文；由算法表持有。 */
static OmBootVerifyCrc32Context g_crc32_context;

/** 内置校验算法表。新增算法时在此追加描述符。 */
static const OmBootImageVerifyAlgorithm g_verify_algorithms[] = {
    {
        (uint16_t)OM_IMAGE_DIGEST_CRC32_ISO_HDLC,
        (uint16_t)OM_BOOT_VERIFY_CRC32_LEN,
        &g_crc32_context,
        om_boot_verify_crc32_begin,
        om_boot_verify_crc32_update,
        om_boot_verify_crc32_finish,
    },
};

const OmBootImageVerifyAlgorithm *om_boot_image_verify_algorithms(uint32_t *count)
{
    if (count != NULL)
    {
        *count = (uint32_t)(sizeof(g_verify_algorithms) / sizeof(g_verify_algorithms[0]));
    }
    return g_verify_algorithms;
}

/** 保存校验通过后的镜像属性，供上层决策使用。 */
static void fill_info(OmBootImageInfo *info, const OmImageHeader *hdr)
{
    info->imageVersion = hdr->imageVersion;
    info->flags = hdr->flags;
    info->payloadOffset = hdr->payloadOffset;
    info->imageSize = hdr->imageSize;
    info->imageTotalSize = hdr->imageTotalSize;
}

/*
 * 保留区必须是擦除值。写入端若把这里当作新字段使用，却没有同步升头版本，
 * 旧引导方会在本检查处明确拒绝，而不是把未知数据当成保留位忽略。
 */
static OmBool reserved_is_erased(const OmImageHeader *hdr)
{
    size_t i; /* 保留区字节序号 */

    for (i = 0u; i < sizeof(hdr->reserved); i++)
    {
        if (hdr->reserved[i] != 0xFFu)
        {
            return OM_FALSE;
        }
    }
    return OM_TRUE;
}

/** 按镜像头声明的算法标识查表；未注册返回 NULL。 */
static const OmBootImageVerifyAlgorithm *find_algorithm(const OmBootImageVerifier *verifier,
                                                        uint16_t algo)
{
    uint32_t i; /* 算法表序号 */

    for (i = 0u; i < verifier->algorithmCount; i++)
    {
        if (verifier->algorithms[i].algo == algo)
        {
            return &verifier->algorithms[i];
        }
    }
    return NULL;
}

/*
 * 按算法描述符流式计算 [0, digestOffset) 的摘要并与存储值比较。上下文
 * 由校验器提供，算法实现不持有全局状态；分段读取保证镜像不需要整体入 RAM。
 */
static OmBootImageStatus verify_digest(const OmBootImageVerifyAlgorithm *algorithm,
                                       const OmPartitionHandle *source,
                                       const OmImageHeader *hdr)
{
    uint8_t stored[OM_IMAGE_DIGEST_REGION_SIZE];   /* 镜像内保存的摘要值 */
    uint8_t computed[OM_IMAGE_DIGEST_REGION_SIZE]; /* 算法算出的摘要值 */
    uint8_t buf[IMAGE_VERIFIER_CHUNK_SIZE];        /* 分块读取缓冲 */
    uint32_t offset = 0u;                          /* 当前已读到的镜像偏移 */
    OmRet ret;                                     /* 算法或读取返回值 */

    ret = om_partition_read(source, hdr->digestOffset, stored, algorithm->digestLen);
    if (ret != OM_OK)
    {
        return OM_BOOT_IMAGE_READ_ERROR;
    }

    ret = algorithm->begin(algorithm->context);
    if (ret != OM_OK)
    {
        return OM_BOOT_IMAGE_DIGEST_ERROR;
    }

    while (offset < hdr->digestOffset)
    {
        uint32_t len = hdr->digestOffset - offset; /* 本次读取长度，最后一块可能不满 */

        if (len > IMAGE_VERIFIER_CHUNK_SIZE)
        {
            len = IMAGE_VERIFIER_CHUNK_SIZE;
        }
        ret = om_partition_read(source, offset, buf, len);
        if (ret != OM_OK)
        {
            return OM_BOOT_IMAGE_READ_ERROR;
        }
        ret = algorithm->update(algorithm->context, buf, len);
        if (ret != OM_OK)
        {
            return OM_BOOT_IMAGE_DIGEST_ERROR;
        }
        offset += len;
    }

    ret = algorithm->finish(algorithm->context, computed);
    if (ret != OM_OK)
    {
        return OM_BOOT_IMAGE_DIGEST_ERROR;
    }
    return (memcmp(stored, computed, algorithm->digestLen) == 0) ? OM_BOOT_IMAGE_VALID
                                                                 : OM_BOOT_IMAGE_DIGEST_MISMATCH;
}

OmRet om_boot_image_verifier_init(OmBootImageVerifier *verifier,
                                  const OmBootImageVerifyAlgorithm *algorithms,
                                  uint32_t count)
{
    uint32_t i; /* 算法表序号 */

    if (verifier == NULL || algorithms == NULL || count == 0u)
    {
        return OM_ERR_INVALID_ARG;
    }

    for (i = 0u; i < count; i++)
    {
        const OmBootImageVerifyAlgorithm *algorithm = &algorithms[i]; /* 当前算法 */

        if (algorithm->begin == NULL || algorithm->update == NULL ||
            algorithm->finish == NULL)
        {
            return OM_ERR_INVALID_ARG;
        }
        if (algorithm->digestLen == 0u ||
            algorithm->digestLen > OM_IMAGE_DIGEST_REGION_SIZE)
        {
            return OM_ERR_INVALID_ARG;
        }
    }

    verifier->algorithms = algorithms;
    verifier->algorithmCount = count;
    return OM_OK;
}

OmRet om_boot_image_verifier_verify(const OmBootImageVerifier *verifier,
                                    const OmPartitionHandle *source,
                                    uint32_t expected_slot,
                                    OmBootImageResult *result)
{
    OmImageHeader hdr;                           /* 从镜像源读出的镜像头 */
    const OmBootImageVerifyAlgorithm *algorithm; /* 头声明的摘要算法 */
    uint64_t digest_offset;                      /* 摘要区起点 */
    uint64_t total;                              /* 镜像声明的总长 */
    uint32_t capacity;                           /* 镜像源容量 */
    OmRet ret;                                   /* 读取返回值 */

    if (result == NULL)
    {
        return OM_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    result->status = OM_BOOT_IMAGE_INVALID_ARG;
    if (verifier == NULL || source == NULL || expected_slot > (uint32_t)OM_IMAGE_SLOT_B)
    {
        return OM_ERR_INVALID_ARG;
    }

    ret = om_partition_read(source, 0u, &hdr, sizeof(hdr));
    if (ret != OM_OK)
    {
        result->status = OM_BOOT_IMAGE_READ_ERROR;
        return OM_OK;
    }

    if (hdr.magic == 0xFFFFFFFFu)
    {
        result->status = OM_BOOT_IMAGE_BLANK;
        return OM_OK;
    }
    if (hdr.magic != OM_IMAGE_MAGIC)
    {
        result->status = OM_BOOT_IMAGE_BAD_MAGIC;
        return OM_OK;
    }
    if (hdr.hdrVersion != (uint16_t)OM_IMAGE_HDR_VERSION)
    {
        result->status = OM_BOOT_IMAGE_BAD_VERSION;
        return OM_OK;
    }
    if (hdr.hdrSize != (uint16_t)OM_IMAGE_HDR_SIZE)
    {
        result->status = OM_BOOT_IMAGE_BAD_HEADER_SIZE;
        return OM_OK;
    }
    if ((hdr.flags & OM_IMAGE_F_NON_BOOTABLE) != 0u)
    {
        result->status = OM_BOOT_IMAGE_NOT_BOOTABLE;
        return OM_OK;
    }

    /*
     * 用 64 位中间值做加法，先排除溢出与越界，再允许后续按 digestOffset
     * 读取。这里同时要求总长与镜像源容量相符，避免镜像声明一个比源还大的
     * 范围。
     */
    capacity = om_partition_capacity(source);
    digest_offset = (uint64_t)hdr.payloadOffset + (uint64_t)hdr.imageSize;
    total = digest_offset + (uint64_t)hdr.digestRegionSize;
    if (hdr.imageSize == 0u ||
        hdr.digestRegionSize != (uint16_t)OM_IMAGE_DIGEST_REGION_SIZE ||
        digest_offset != (uint64_t)hdr.digestOffset ||
        total != (uint64_t)hdr.imageTotalSize ||
        total > (uint64_t)capacity)
    {
        result->status = OM_BOOT_IMAGE_BAD_SIZES;
        return OM_OK;
    }

    if (hdr.payloadOffset < (uint32_t)hdr.hdrSize ||
        (hdr.payloadOffset & 3u) != 0u)
    {
        result->status = OM_BOOT_IMAGE_BAD_OFFSET;
        return OM_OK;
    }

    algorithm = find_algorithm(verifier, hdr.digestAlgo);
    if (algorithm == NULL)
    {
        result->status = OM_BOOT_IMAGE_BAD_DIGEST_ALGO;
        return OM_OK;
    }
    if (hdr.digestLen != algorithm->digestLen)
    {
        result->status = OM_BOOT_IMAGE_BAD_DIGEST_LEN;
        return OM_OK;
    }

    if (hdr.slotId > (uint16_t)OM_IMAGE_SLOT_B)
    {
        result->status = OM_BOOT_IMAGE_SLOT_MISMATCH;
        return OM_OK;
    }
    if ((hdr.flags & OM_IMAGE_F_SLOT_BOUND) != 0u &&
        (uint32_t)hdr.slotId != expected_slot)
    {
        result->status = OM_BOOT_IMAGE_SLOT_MISMATCH;
        return OM_OK;
    }

    if (reserved_is_erased(&hdr) == OM_FALSE)
    {
        result->status = OM_BOOT_IMAGE_BAD_RESERVED;
        return OM_OK;
    }

    result->status = verify_digest(algorithm, source, &hdr);
    if (result->status == OM_BOOT_IMAGE_VALID)
    {
        fill_info(&result->info, &hdr);
    }
    return OM_OK;
}
