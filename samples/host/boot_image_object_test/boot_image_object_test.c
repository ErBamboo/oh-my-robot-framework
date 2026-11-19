/**
 * @file   boot_image_object_test.c
 * @brief  镜像对象与校验器 host 测试
 *
 * 覆盖镜像对象的构造与校验、校验器的详细状态与算法注入。夹具为一个 32K
 * 模拟 flash，app_a/app_b 各占 2 个 4K 扇区。
 *
 * 退出码 0=全绿；非 0=有 FAIL。
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "algorithm/checksum/crc32.h"
#include "boot/contract/image.h"
#include "boot/image.h"
#include "boot/image_verifier.h"
#include "drivers/model/device.h"
#include "drivers/peripheral/flash/pal_flash_dev.h"
#include "drivers/storage/partition.h"

#include "flash_sim.h"

#define SECT_SIZE 0x1000u
#define FLASH_CAP 0x8000u
#define SLOT_SIZE (SECT_SIZE * 2u)
#define A_OFF 0x3000u
#define B_OFF 0x5000u
#define IMG_BUF_SIZE 4096u

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

static const FlashGeometry g_geom = {
    .capacity = FLASH_CAP,
    .erasedValue = 0xFFu,
    .writeUnit = 4u,
    .pageSize = 0u,
    .sectorSize = SECT_SIZE,
    .sectorCount = FLASH_CAP / SECT_SIZE,
    .sectorRegions = NULL,
};

static const FlashOps g_ops = {
    .read = flash_sim_read,
    .write = flash_sim_write,
    .erase = flash_sim_erase,
};

static const OmPartitionEntry g_table[] = {
    {"app_a", "flash0", A_OFF, SLOT_SIZE},
    {"app_b", "flash0", B_OFF, SLOT_SIZE},
};
static const OmPartitionRegistry g_registry = OM_PARTITION_REGISTRY(g_table);

static FlashDev g_dev;
static FlashSim g_sim;
static OmBootImageVerifier g_verifier;
static OmPartitionHandle g_a_handle;
static OmPartitionHandle g_b_handle;
static OmBootImage g_a_image;
static OmBootImage g_b_image;
static uint8_t g_img[IMG_BUF_SIZE];

static void reset_storage(void)
{
    memset(g_sim.mem, 0xFF, FLASH_CAP);
    flash_sim_set_fail(&g_sim, false);
    flash_sim_set_read_fail(&g_sim, false);
    flash_sim_set_silent_erase_fail(&g_sim, false);
}

static int part_write(const char *part, const uint8_t *img, size_t len)
{
    OmPartitionHandle handle;

    if (om_partition_open(&g_registry, part, &handle) != OM_OK)
    {
        return 0;
    }
    if (om_partition_erase(&handle) != OM_OK)
    {
        return 0;
    }
    return om_partition_write(&handle, 0u, img, len) == OM_OK;
}

/** 绕过写粒度校验，把非对齐长度的镜像直接放进模拟介质。 */
static int part_write_raw(const char *part, const uint8_t *img, size_t len)
{
    OmPartitionHandle handle;

    if (om_partition_open(&g_registry, part, &handle) != OM_OK)
    {
        return 0;
    }
    if (om_partition_erase(&handle) != OM_OK)
    {
        return 0;
    }
    if (len > SLOT_SIZE)
    {
        return 0;
    }
    memcpy(g_sim.mem + ((part[4] == 'a') ? A_OFF : B_OFF), img, len);
    return 1;
}

static size_t build_image(uint8_t *out, size_t cap, uint32_t payload_offset,
                          size_t payload_len, uint32_t version, uint16_t slot,
                          uint32_t flags)
{
    size_t pad = (4u - (payload_len & 3u)) & 3u;
    uint32_t image_size = (uint32_t)(payload_len + pad);
    uint32_t digest_offset;
    uint32_t total;
    uint32_t crc;
    OmImageHeader *h;
    size_t i;

    digest_offset = payload_offset + image_size;
    total = digest_offset + OM_IMAGE_DIGEST_REGION_SIZE;
    if (total > cap)
    {
        return 0;
    }

    memset(out, 0xFF, total);
    for (i = 0u; i < payload_len; i++)
    {
        out[payload_offset + i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
    }

    h = (OmImageHeader *)out;
    h->magic = OM_IMAGE_MAGIC;
    h->hdrVersion = (uint16_t)OM_IMAGE_HDR_VERSION;
    h->hdrSize = (uint16_t)OM_IMAGE_HDR_SIZE;
    h->payloadOffset = payload_offset;
    h->imageSize = image_size;
    h->imageTotalSize = total;
    h->imageVersion = version;
    h->flags = flags;
    h->digestAlgo = (uint16_t)OM_IMAGE_DIGEST_CRC32_ISO_HDLC;
    h->digestLen = 4u;
    h->digestOffset = digest_offset;
    h->digestRegionSize = (uint16_t)OM_IMAGE_DIGEST_REGION_SIZE;
    h->slotId = slot;

    crc = om_crc32_iso_hdlc(OM_CRC32_INIT, out, digest_offset);
    out[digest_offset + 0u] = (uint8_t)(crc & 0xFFu);
    out[digest_offset + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
    out[digest_offset + 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    out[digest_offset + 3u] = (uint8_t)((crc >> 24) & 0xFFu);
    return total;
}

static OmBootImageStatus verify_part(const char *part, uint32_t bound_slot,
                                     OmBootImageInfo *info)
{
    OmPartitionHandle handle;
    OmBootImage image;
    OmBootImageResult result;

    if (om_partition_open(&g_registry, part, &handle) != OM_OK)
    {
        return OM_BOOT_IMAGE_READ_ERROR;
    }
    if (om_boot_image_init(&image, &handle, bound_slot, &g_verifier) != OM_OK)
    {
        return OM_BOOT_IMAGE_INVALID_ARG;
    }
    (void)om_boot_image_verify(&image, &result);
    if (info != NULL)
    {
        *info = result.info;
    }
    return result.status;
}

/* ===================================================================
 * 假摘要算法夹具
 * =================================================================== */

#define FAKE_DIGEST_ALGO 0x7F01u

typedef struct FakeDigestContext
{
    uint32_t value;
} FakeDigestContext;

static FakeDigestContext g_fake_context;

static OmRet fake_begin(void *ctx)
{
    ((FakeDigestContext *)ctx)->value = 0u;
    return OM_OK;
}

static OmRet fake_update(void *ctx, const uint8_t *data, uint32_t len)
{
    FakeDigestContext *digest_ctx = (FakeDigestContext *)ctx;
    uint32_t i;

    for (i = 0u; i < len; i++)
    {
        digest_ctx->value = digest_ctx->value * 31u + data[i];
    }
    return OM_OK;
}

static OmRet fake_finish(void *ctx, uint8_t *digest)
{
    uint32_t value = ((FakeDigestContext *)ctx)->value;

    digest[0] = (uint8_t)(value & 0xFFu);
    digest[1] = (uint8_t)((value >> 8) & 0xFFu);
    digest[2] = (uint8_t)((value >> 16) & 0xFFu);
    digest[3] = (uint8_t)((value >> 24) & 0xFFu);
    return OM_OK;
}

static uint32_t fake_digest_value(const uint8_t *data, uint32_t len)
{
    uint32_t value = 0u;
    uint32_t i;

    for (i = 0u; i < len; i++)
    {
        value = value * 31u + data[i];
    }
    return value;
}

static const OmBootImageVerifyAlgorithm g_fake_algorithm = {
    FAKE_DIGEST_ALGO,
    4u,
    &g_fake_context,
    fake_begin,
    fake_update,
    fake_finish,
};

static size_t build_fake_image(uint8_t *out, size_t cap, size_t payload_len,
                               uint32_t version, uint16_t slot, uint32_t flags)
{
    OmImageHeader *hdr;
    uint32_t digest_offset;
    uint32_t value;
    size_t len;

    len = build_image(out, cap, OM_IMAGE_PAYLOAD_OFFSET, payload_len, version, slot, flags);
    if (len == 0u)
    {
        return 0u;
    }

    hdr = (OmImageHeader *)out;
    hdr->digestAlgo = (uint16_t)FAKE_DIGEST_ALGO;
    hdr->digestLen = 4u;
    digest_offset = hdr->digestOffset;
    value = fake_digest_value(out, digest_offset);
    out[digest_offset + 0u] = (uint8_t)(value & 0xFFu);
    out[digest_offset + 1u] = (uint8_t)((value >> 8) & 0xFFu);
    out[digest_offset + 2u] = (uint8_t)((value >> 16) & 0xFFu);
    out[digest_offset + 3u] = (uint8_t)((value >> 24) & 0xFFu);
    return len;
}

/* ===================================================================
 * 1. 镜像校验状态
 * =================================================================== */

static void test_verifier_statuses(void)
{
    OmBootImageInfo info;
    size_t len;
    uint8_t *raw = g_img;

    printf("[1] image verification statuses\n");
    reset_storage();

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    CHECK(len > 0u && part_write("app_a", g_img, len), "write slot A image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_VALID, "valid image accepted");
    CHECK(info.imageVersion == 0x01020300u, "info version");
    CHECK(verify_part("app_a", 1u, &info) == OM_BOOT_IMAGE_SLOT_MISMATCH,
          "slot-bound image rejected on the other slot");
    CHECK(verify_part("app_b", 1u, &info) == OM_BOOT_IMAGE_BLANK, "blank slot");

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    raw[0] = 0x00u;
    CHECK(part_write("app_a", raw, len), "write bad-magic image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_BAD_MAGIC, "bad magic");
    CHECK(info.imageVersion == 0u, "info ignored on failure");

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    ((OmImageHeader *)raw)->hdrVersion = 2u;
    CHECK(part_write("app_a", raw, len), "write bad-version image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_BAD_VERSION, "bad version");

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    ((OmImageHeader *)raw)->hdrSize = 63u;
    CHECK(part_write("app_a", raw, len), "write bad-header-size image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_BAD_HEADER_SIZE,
          "bad header size");

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A,
                      OM_IMAGE_F_SLOT_BOUND | OM_IMAGE_F_NON_BOOTABLE);
    CHECK(part_write("app_a", raw, len), "write non-bootable image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_NOT_BOOTABLE,
          "non-bootable flag");

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    ((OmImageHeader *)raw)->imageTotalSize += 4u;
    CHECK(part_write("app_a", raw, len), "write bad-size image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_BAD_SIZES,
          "inconsistent sizes");

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET + 2u, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    CHECK(len > 0u && part_write_raw("app_a", raw, len), "write unaligned-payload image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_BAD_OFFSET,
          "unaligned payload offset");

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    ((OmImageHeader *)raw)->digestAlgo = (uint16_t)OM_IMAGE_DIGEST_NONE;
    CHECK(part_write("app_a", raw, len), "write unsupported-algo image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_BAD_DIGEST_ALGO,
          "unsupported digest algorithm");

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    ((OmImageHeader *)raw)->digestLen = 8u;
    CHECK(part_write("app_a", raw, len), "write bad-digest-len image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_BAD_DIGEST_LEN,
          "bad digest length");

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    ((OmImageHeader *)raw)->reserved[0] = 0x00u;
    CHECK(part_write("app_a", raw, len), "write bad-reserved image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_BAD_RESERVED,
          "reserved byte not erased");

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020300u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    raw[OM_IMAGE_PAYLOAD_OFFSET + 8u] ^= 0x01u;
    CHECK(part_write("app_a", raw, len), "write digest-mismatch image");
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_DIGEST_MISMATCH,
          "payload corruption detected");

    flash_sim_set_read_fail(&g_sim, true);
    CHECK(verify_part("app_a", 0u, &info) == OM_BOOT_IMAGE_READ_ERROR, "read error");
    flash_sim_set_read_fail(&g_sim, false);

    CHECK(verify_part("app_a", 2u, &info) == OM_BOOT_IMAGE_INVALID_ARG,
          "invalid expected slot");
}

/* ===================================================================
 * 2. 镜像对象身份
 * =================================================================== */

static void test_image_identity(void)
{
    OmPartitionHandle empty_handle;
    OmBootImage empty_image;
    OmBootImageResult result;

    printf("[2] image identity\n");
    reset_storage();
    memset(&empty_handle, 0, sizeof(empty_handle));

    CHECK(om_boot_image_slot_id(&g_a_image) == 0u &&
              om_boot_image_slot_id(&g_b_image) == 1u,
          "image slot ids");
    CHECK(om_partition_capacity(om_boot_image_partition(&g_a_image)) == SLOT_SIZE &&
              om_partition_capacity(om_boot_image_partition(&g_b_image)) == SLOT_SIZE,
          "image partitions are bound");
    CHECK(om_boot_image_slot_id(NULL) == 0u && om_boot_image_partition(NULL) == NULL,
          "NULL accessors");

    CHECK(om_boot_image_init(&empty_image, &empty_handle, 0u, &g_verifier) ==
              OM_ERR_INVALID_ARG,
          "unavailable partition rejected");
    CHECK(om_boot_image_init(&empty_image, &g_a_handle, 2u, &g_verifier) ==
              OM_ERR_INVALID_ARG,
          "invalid slot id rejected");
    CHECK(om_boot_image_init(NULL, &g_a_handle, 0u, &g_verifier) == OM_ERR_INVALID_ARG,
          "NULL image rejected");
    CHECK(om_boot_image_init(&empty_image, NULL, 0u, &g_verifier) == OM_ERR_INVALID_ARG,
          "NULL partition rejected");
    CHECK(om_boot_image_init(&empty_image, &g_a_handle, 0u, NULL) == OM_ERR_INVALID_ARG,
          "NULL verifier rejected");

    CHECK(om_boot_image_verify(&g_a_image, NULL) == OM_ERR_INVALID_ARG,
          "NULL result rejected");
    CHECK(om_boot_image_verify(NULL, &result) == OM_ERR_INVALID_ARG,
          "NULL image rejected on verify");
}

/* ===================================================================
 * 3. 校验算法注入
 * =================================================================== */

static void test_verifier_injection(void)
{
    OmBootImageVerifier verifier;
    OmBootImage image;
    OmPartitionHandle part;
    OmBootImageResult result;
    size_t len;

    printf("[3] verifier algorithm injection\n");
    reset_storage();

    len = build_image(g_img, sizeof(g_img), OM_IMAGE_PAYLOAD_OFFSET, 128u,
                      0x01020000u, (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    CHECK(len > 0u && part_write("app_a", g_img, len), "write CRC32 image");
    CHECK(om_partition_open(&g_registry, "app_a", &part) == OM_OK, "open app_a source");

    CHECK(om_boot_image_verifier_init(&verifier, &g_fake_algorithm, 1u) == OM_OK,
          "init verifier with injected algorithm");
    CHECK(om_boot_image_init(&image, &part, 0u, &verifier) == OM_OK,
          "bind injected verifier to image");
    (void)om_boot_image_verify(&image, &result);
    CHECK(result.status == OM_BOOT_IMAGE_BAD_DIGEST_ALGO,
          "injected table rejects unregistered CRC32");

    len = build_fake_image(g_img, sizeof(g_img), 128u, 0x01020000u,
                           (uint16_t)OM_IMAGE_SLOT_A, OM_IMAGE_F_SLOT_BOUND);
    CHECK(len > 0u && part_write("app_a", g_img, len), "write fake-digest image");

    (void)om_boot_image_verify(&image, &result);
    CHECK(result.status == OM_BOOT_IMAGE_VALID, "injected algorithm verifies image");
    CHECK(result.info.imageVersion == 0x01020000u, "injected algorithm reports info");

    (void)om_boot_image_verify(&g_a_image, &result);
    CHECK(result.status == OM_BOOT_IMAGE_BAD_DIGEST_ALGO,
          "default table does not register the fake algorithm");
}

int main(void)
{
    const OmBootImageVerifyAlgorithm *algorithms;
    uint32_t algorithm_count = 0u;

    flash_sim_init(&g_sim, FLASH_CAP, 0xFFu, 4u);
    g_sim.strictProgram = true;

    if (flash_register(&g_dev, "flash0", &g_geom, &g_ops, &g_sim) != OM_OK)
    {
        printf("FAIL: register flash0\n");
        return 1;
    }

    algorithms = om_boot_image_verify_algorithms(&algorithm_count);
    if (om_boot_image_verifier_init(&g_verifier, algorithms, algorithm_count) != OM_OK)
    {
        printf("FAIL: init image verifier\n");
        return 1;
    }

    if (om_partition_open(&g_registry, "app_a", &g_a_handle) != OM_OK ||
        om_partition_open(&g_registry, "app_b", &g_b_handle) != OM_OK)
    {
        printf("FAIL: open image partitions\n");
        return 1;
    }
    if (om_boot_image_init(&g_a_image, &g_a_handle, 0u, &g_verifier) != OM_OK ||
        om_boot_image_init(&g_b_image, &g_b_handle, 1u, &g_verifier) != OM_OK)
    {
        printf("FAIL: init images\n");
        return 1;
    }

    test_verifier_statuses();
    test_image_identity();
    test_verifier_injection();

    printf("\n结果：%d 通过 / %d 失败\n", g_pass, g_fail);
    flash_sim_deinit(&g_sim);
    return (g_fail == 0) ? 0 : 1;
}
