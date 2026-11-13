/**
 * @file   boot_image_test.c
 * @brief  镜像格式契约 host 测试（头布局 / 摘要原语 / 字段自洽 / 与主机侧工具对拍）
 *
 * 对拍方式：本文件按契约组装同一个镜像（固定负载、固定元数据），把算出的摘要与
 * 主机侧打包工具产出的值比对。摘要覆盖整段头+填充+负载，故摘要一致即两侧字节
 * 布局一致——任何字段宽度/偏移/填充取值的偏差都会让摘要对不上。
 *
 * 退出码 0=全绿；非 0=有 FAIL。
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "algorithm/checksum/crc32.h"
#include "boot/image.h"

/* ===================================================================
 * 对拍基准（由主机侧打包工具对同一负载与元数据产出）
 *   负载 = 1300 字节，公式 (i*37+11)&0xFF；版本 1.2.3；槽 a；flags=SLOT_BOUND；摘要 CRC32
 * =================================================================== */

#define FIX_PAYLOAD_LEN 1300u
#define FIX_VERSION 0x01020300u
#define FIX_SLOT OM_IMAGE_SLOT_A
#define FIX_FLAGS OM_IMAGE_F_SLOT_BOUND
#define FIX_IMAGE_SIZE 1300u
#define FIX_TOTAL_SIZE 2068u /* 0x200 + 1300 + 256 */
#define FIX_DIGEST_OFFSET 0x714u
#define FIX_DIGEST 0x947D81DCu

static int g_pass;
static int g_fail;

#define CHECK(cond, ...)         \
    do                           \
    {                            \
        if (cond)                \
        {                        \
            g_pass++;            \
            printf("  PASS: ");  \
            printf(__VA_ARGS__); \
            printf("\n");        \
        }                        \
        else                     \
        {                        \
            g_fail++;            \
            printf("  FAIL: ");  \
            printf(__VA_ARGS__); \
            printf("\n");        \
        }                        \
    } while (0)

/* ===================================================================
 * 镜像组装（契约的 C 侧复刻，仅供测试构造夹具）
 * =================================================================== */

#define IMG_BUF_SIZE 4096u
static uint8_t g_img[IMG_BUF_SIZE];

static void fill_payload(uint8_t *dst, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        dst[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
    }
}

/** 组装镜像，返回总长；失败返回 0 */
static size_t build_image(uint8_t *out, size_t cap, const uint8_t *payload, size_t len,
                          uint32_t version, uint16_t slot, uint32_t flags)
{
    size_t pad = (4u - (len & 3u)) & 3u; /* 负载补齐到 4 的倍数 */
    uint32_t imageSize = (uint32_t)(len + pad);
    uint32_t payloadOffset = OM_IMAGE_PAYLOAD_OFFSET;
    uint32_t digestOffset = payloadOffset + imageSize;
    uint32_t total = digestOffset + OM_IMAGE_DIGEST_REGION_SIZE;
    uint32_t digest;
    OmImageHeader *h;

    if (total > cap)
    {
        return 0;
    }

    memset(out, 0xFF, total); /* 填充值 = 0xFF */
    memcpy(out + payloadOffset, payload, len);
    if (pad)
    {
        memset(out + payloadOffset + len, 0xFF, pad);
    }

    h = (OmImageHeader *)out;
    h->magic = OM_IMAGE_MAGIC;
    h->hdrVersion = (uint16_t)OM_IMAGE_HDR_VERSION;
    h->hdrSize = (uint16_t)OM_IMAGE_HDR_SIZE;
    h->payloadOffset = payloadOffset;
    h->imageSize = imageSize;
    h->imageTotalSize = total;
    h->imageVersion = version;
    h->flags = flags;
    h->digestAlgo = (uint16_t)OM_IMAGE_DIGEST_CRC32_ISO_HDLC;
    h->digestLen = 4u;
    h->digestOffset = digestOffset;
    h->digestRegionSize = (uint16_t)OM_IMAGE_DIGEST_REGION_SIZE;
    h->slotId = slot;
    h->headerCrc32 = 0u; /* 未启用 */

    digest = om_crc32_iso_hdlc(OM_CRC32_INIT, out, digestOffset);
    memcpy(out + digestOffset, &digest, sizeof(digest)); /* 摘要小端写入 */

    return total;
}

/** 重算摘要并与摘要区比对（摘要算法为 CRC32 时） */
static int digest_matches(const uint8_t *img)
{
    const OmImageHeader *h = (const OmImageHeader *)img;
    uint32_t calc = om_crc32_iso_hdlc(OM_CRC32_INIT, img, h->digestOffset);
    uint32_t stored;

    memcpy(&stored, img + h->digestOffset, sizeof(stored));
    return calc == stored;
}

int main(void)
{
    size_t total;

    printf("== 1. 头布局 ==\n");
    CHECK(sizeof(OmImageHeader) == OM_IMAGE_HDR_SIZE, "sizeof(OmImageHeader)=%u", (unsigned)sizeof(OmImageHeader));
    CHECK(OM_IMAGE_HDR_SIZE == 64u, "头长常量=%u", (unsigned)OM_IMAGE_HDR_SIZE);
    CHECK(offsetof(OmImageHeader, magic) == 0 && offsetof(OmImageHeader, hdrVersion) == 4 &&
              offsetof(OmImageHeader, hdrSize) == 6 && offsetof(OmImageHeader, payloadOffset) == 8,
          "前四字段偏移 0/4/6/8");
    CHECK(offsetof(OmImageHeader, digestAlgo) == 28 && offsetof(OmImageHeader, digestLen) == 30 &&
              offsetof(OmImageHeader, digestOffset) == 32 && offsetof(OmImageHeader, digestRegionSize) == 36,
          "摘要字段偏移 28/30/32/36");
    CHECK(offsetof(OmImageHeader, slotId) == 38 && offsetof(OmImageHeader, headerCrc32) == 40 &&
              offsetof(OmImageHeader, reserved) == 44,
          "尾字段偏移 38/40/44");
    CHECK(OM_IMAGE_MAGIC != 0xFFFFFFFFu && OM_IMAGE_MAGIC != 0u, "magic=0x%08X 非擦除态/非空", OM_IMAGE_MAGIC);

    printf("== 2. 摘要原语 ==\n");
    CHECK(om_crc32_iso_hdlc(OM_CRC32_INIT, "123456789", 9) == 0xCBF43926u, "自检向量 check(\"123456789\")");
    {
        uint32_t whole = om_crc32_iso_hdlc(OM_CRC32_INIT, "123456789", 9);
        uint32_t split = om_crc32_iso_hdlc(OM_CRC32_INIT, "12345", 5);
        split = om_crc32_iso_hdlc(split, "6789", 4);
        CHECK(whole == split, "分段续算与一次性计算一致");
    }
    CHECK(om_crc32_iso_hdlc(OM_CRC32_INIT, "", 0) == 0u, "空数据得 0");
    CHECK(om_crc32_iso_hdlc(OM_CRC32_INIT, "a", 1) ==
              (om_crc32_iso_hdlc(om_crc32_iso_hdlc(OM_CRC32_INIT, "a", 1), "", 0)),
          "零长续算不改变结果");

    printf("== 3. 组装与字段自洽 ==\n");
    {
        uint8_t payload[FIX_PAYLOAD_LEN];
        const OmImageHeader *h;

        fill_payload(payload, sizeof(payload));
        total = build_image(g_img, sizeof(g_img), payload, sizeof(payload), FIX_VERSION, FIX_SLOT, FIX_FLAGS);
        CHECK(total > 0, "组装成功，总长 %u", (unsigned)total);
        if (total == 0)
        {
            printf("组装失败，后续用例跳过\n");
            goto summary;
        }

        h = (const OmImageHeader *)g_img;
        CHECK(h->magic == OM_IMAGE_MAGIC, "magic 就位");
        CHECK(h->hdrVersion == OM_IMAGE_HDR_VERSION && h->hdrSize == OM_IMAGE_HDR_SIZE, "版本与头长就位");
        CHECK(h->payloadOffset == OM_IMAGE_PAYLOAD_OFFSET, "负载偏移=0x%X", (unsigned)h->payloadOffset);
        CHECK(h->imageSize == FIX_IMAGE_SIZE, "负载长=%u", (unsigned)h->imageSize);
        CHECK(h->imageTotalSize == FIX_TOTAL_SIZE, "总长=%u", (unsigned)h->imageTotalSize);
        CHECK(h->imageTotalSize == h->payloadOffset + h->imageSize + h->digestRegionSize, "总长字段自洽");
        CHECK(h->digestOffset == h->payloadOffset + h->imageSize, "摘要偏移= 负载偏移+负载长");
        CHECK(h->slotId == FIX_SLOT && h->imageVersion == FIX_VERSION, "槽与版本就位");
        CHECK(h->headerCrc32 == 0u, "头自身校验未启用（置零）");
        CHECK(g_img[h->payloadOffset - 1] == 0xFFu, "负载前填充为 0xFF");
    }

    printf("== 4. 与主机侧工具对拍 ==\n");
    {
        uint32_t digest;
        memcpy(&digest, g_img + FIX_DIGEST_OFFSET, sizeof(digest));
        CHECK(digest == FIX_DIGEST, "摘要 = 0x%08X（工具产出 0x%08X）", digest, FIX_DIGEST);
        CHECK(digest_matches(g_img), "重算摘要与摘要区一致");
    }

    printf("== 5. 损坏检出 ==\n");
    {
        uint8_t backup = g_img[OM_IMAGE_PAYLOAD_OFFSET + 8];
        g_img[OM_IMAGE_PAYLOAD_OFFSET + 8] ^= 0x01;
        CHECK(!digest_matches(g_img), "负载单比特翻转被检出");
        g_img[OM_IMAGE_PAYLOAD_OFFSET + 8] = backup;

        g_img[0] ^= 0xFF; /* 头内 magic 位翻转 */
        CHECK(!digest_matches(g_img), "头部单比特翻转被检出");
        g_img[0] ^= 0xFF;

        CHECK(digest_matches(g_img), "还原后重新通过");
    }

    printf("== 6. 非 4 倍数负载补齐 ==\n");
    {
        uint8_t payload[101];
        const OmImageHeader *h;

        fill_payload(payload, sizeof(payload));
        total = build_image(g_img, sizeof(g_img), payload, sizeof(payload), FIX_VERSION, FIX_SLOT, FIX_FLAGS);
        CHECK(total > 0, "组装成功，总长 %u", (unsigned)total);
        if (total > 0)
        {
            h = (const OmImageHeader *)g_img;
            CHECK(h->imageSize == 104u, "101 字节负载补齐到 %u", (unsigned)h->imageSize);
            CHECK((h->imageSize & 3u) == 0u, "补齐后长度 4 字节对齐");
            CHECK(digest_matches(g_img), "补齐后摘要自洽");
        }
    }

summary:
    printf("\n结果：%d 通过 / %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
