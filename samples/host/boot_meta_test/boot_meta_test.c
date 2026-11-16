/**
 * @file   boot_meta_test.c
 * @brief  启动决策数据读写 host 测试（双份轮转、擦除与掉电破坏）
 *
 * 夹具：16K 均匀扇区的模拟 flash，meta 分区 = 2 个擦除单位。测试按 Plan A
 * 的写纪律覆盖：空分区、单份写入、双份都有效、写者擦除非最新副本、boot 清理、
 * 半写 entry、擦除失败、写入失败与 seq 推进。
 *
 * 退出码 0=全绿；非 0=有 FAIL。
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "algorithm/checksum/crc32.h"
#include "boot/meta.h"
#include "drivers/model/device.h"
#include "drivers/peripheral/flash/pal_flash_dev.h"
#include "drivers/storage/partition.h"

#include "flash_sim.h"

#define SECT_SIZE 0x4000u
#define META_CAP (SECT_SIZE * 2u)

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
    .capacity = META_CAP,
    .erasedValue = 0xFFu,
    .writeUnit = 4u,
    .pageSize = 0u,
    .sectorSize = SECT_SIZE,
    .sectorCount = META_CAP / SECT_SIZE,
    .sectorRegions = NULL,
};

static const FlashOps g_ops = {
    .read = flash_sim_read,
    .write = flash_sim_write,
    .erase = flash_sim_erase,
};

static const OmPartitionEntry g_table[] = {
    {"meta", "flash0", 0u, META_CAP},
};
static const OmPartitionRegistry g_registry = OM_PARTITION_REGISTRY(g_table);

static FlashDev g_dev;
static FlashSim g_sim;
static OmPartitionHandle g_part;
static OmBootMeta g_meta;

static uint32_t entry_crc(const OmBootMetaEntry *entry)
{
    return om_crc32_iso_hdlc(OM_CRC32_INIT, entry, offsetof(OmBootMetaEntry, crc32));
}

static OmBootMetaEntry make_entry(uint32_t slot, uint32_t state, uint32_t version, uint32_t flags,
                                  uint32_t reason)
{
    OmBootMetaEntry entry;

    memset(&entry, 0, sizeof(entry));
    entry.activeSlot = slot;
    entry.state = state;
    entry.imageVersion = version;
    entry.flags = flags;
    entry.reason = reason;
    return entry;
}

static OmRet raw_read(uint32_t off, void *buf, size_t len)
{
    return om_partition_read(&g_part, off, buf, len);
}

static OmRet raw_write(uint32_t off, const void *buf, size_t len)
{
    return om_partition_write(&g_part, off, buf, len);
}

static void write_raw_entry(uint32_t copy, OmBootMetaEntry *entry)
{
    entry->crc32 = entry_crc(entry);
    (void)raw_write(g_meta.copyOffset[copy], entry, sizeof(*entry));
}

static int copy_is_erased(uint32_t copy)
{
    OmBootMetaEntry entry;
    const uint8_t *bytes = (const uint8_t *)&entry;
    size_t i;

    if (raw_read(g_meta.copyOffset[copy], &entry, sizeof(entry)) != OM_OK)
    {
        return 0;
    }
    for (i = 0u; i < sizeof(entry); i++)
    {
        if (bytes[i] != 0xFFu)
        {
            return 0;
        }
    }
    return 1;
}

static void test_init_and_empty(void)
{
    OmBootMetaEntry entry;

    printf("[T1] init and empty read\n");
    CHECK(g_meta.copyCount == 2u, "copy count = 2");
    CHECK(g_meta.copyOffset[0] == 0u && g_meta.copyOffset[1] == SECT_SIZE,
          "copies start at erase-unit boundaries");
    CHECK(g_meta.copySize[0] == SECT_SIZE && g_meta.copySize[1] == SECT_SIZE,
          "each copy owns one erase unit");
    CHECK(om_boot_meta_read(&g_meta, &entry, NULL) == OM_ERR_NOT_FOUND,
          "blank partition has no valid entry");
}

static void test_write_valid(void)
{
    OmBootMetaEntry entry = make_entry(0u, OM_BOOT_META_STATE_VALID, 0x01020300u,
                                       0x12340000u, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry readback;
    uint32_t copy = 0xFFFFFFFFu;

    printf("[T2] first write\n");
    CHECK(om_boot_meta_write(&g_meta, &entry) == OM_OK, "write VALID entry");
    CHECK(entry.seq == 1u && entry.crc32 == entry_crc(&entry), "seq and CRC filled by writer");
    CHECK(om_boot_meta_read(&g_meta, &readback, &copy) == OM_OK, "read back VALID entry");
    CHECK(copy == 0u, "first write uses copy 0");
    CHECK(readback.seq == 1u && readback.state == OM_BOOT_META_STATE_VALID &&
              readback.activeSlot == 0u && readback.imageVersion == 0x01020300u &&
              readback.flags == 0x12340000u,
          "entry fields round-trip");
    CHECK(copy_is_erased(1u) == 1, "other copy remains erased");
}

static void test_write_pending(void)
{
    OmBootMetaEntry entry = make_entry(1u, OM_BOOT_META_STATE_PENDING, 0x01020400u,
                                       0x12340000u, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry readback;
    uint32_t copy = 0xFFFFFFFFu;

    printf("[T3] second write uses erased copy\n");
    CHECK(om_boot_meta_write(&g_meta, &entry) == OM_OK, "write PENDING entry");
    CHECK(entry.seq == 2u, "seq advances");
    CHECK(om_boot_meta_read(&g_meta, &readback, &copy) == OM_OK, "read latest entry");
    CHECK(copy == 1u && readback.state == OM_BOOT_META_STATE_PENDING && readback.activeSlot == 1u,
          "latest entry is the PENDING one in copy 1");
}

static void test_write_when_both_valid(void)
{
    OmBootMetaEntry entry = make_entry(0u, OM_BOOT_META_STATE_VALID, 0x01020500u,
                                       0x12340000u, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry readback;
    uint32_t copy = 0xFFFFFFFFu;

    printf("[T4] both copies valid, writer erases older\n");
    CHECK(om_boot_meta_write(&g_meta, &entry) == OM_OK, "write VALID with no erased copy");
    CHECK(entry.seq == 3u, "seq advances past two valid copies");
    CHECK(om_boot_meta_read(&g_meta, &readback, &copy) == OM_OK, "read latest entry");
    CHECK(copy == 0u && readback.seq == 3u && readback.state == OM_BOOT_META_STATE_VALID,
          "writer erased older copy and wrote new latest");
}

static void test_cleanup(void)
{
    OmBootMetaEntry readback;
    uint32_t copy = 0xFFFFFFFFu;

    printf("[T5] boot cleanup keeps latest and one erased copy\n");
    CHECK(om_boot_meta_cleanup(&g_meta) == OM_OK, "cleanup succeeds");
    CHECK(copy_is_erased(1u) == 1, "non-latest copy erased");
    CHECK(om_boot_meta_read(&g_meta, &readback, &copy) == OM_OK, "latest entry survives cleanup");
    CHECK(copy == 0u && readback.seq == 3u, "latest entry unchanged");
}

static void test_write_after_cleanup(void)
{
    OmBootMetaEntry entry = make_entry(1u, OM_BOOT_META_STATE_PENDING, 0x01020600u,
                                       0x12340000u, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry readback;
    uint32_t copy = 0xFFFFFFFFu;

    printf("[T6] write uses the erased copy left by cleanup\n");
    CHECK(om_boot_meta_write(&g_meta, &entry) == OM_OK, "write after cleanup");
    CHECK(entry.seq == 4u, "seq advances");
    CHECK(om_boot_meta_read(&g_meta, &readback, &copy) == OM_OK, "read latest entry");
    CHECK(copy == 1u && readback.seq == 4u, "latest entry in copy 1");
}

static void test_torn_write(void)
{
    OmBootMetaEntry torn = make_entry(0u, OM_BOOT_META_STATE_VALID, 0u, 0u, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry readback;
    uint32_t copy = 0xFFFFFFFFu;

    printf("[T7] half-written entry is ignored\n");
    CHECK(om_boot_meta_cleanup(&g_meta) == OM_OK, "cleanup before torn-write case");
    CHECK(copy_is_erased(0u) == 1, "copy 0 is erased");

    torn.seq = 99u;
    torn.crc32 = 0u; /* 故意错误：模拟 CRC 未写完 */
    CHECK(raw_write(g_meta.copyOffset[0], &torn, sizeof(torn)) == OM_OK,
          "program torn entry into erased copy");
    CHECK(om_boot_meta_read(&g_meta, &readback, &copy) == OM_OK, "read ignores torn entry");
    CHECK(copy == 1u && readback.seq == 4u, "latest valid entry still wins");

    torn = make_entry(0u, OM_BOOT_META_STATE_VALID, 0u, 0u, OM_BOOT_META_REASON_NONE);
    CHECK(om_boot_meta_write(&g_meta, &torn) == OM_OK, "write over torn copy");
    CHECK(torn.seq == 5u, "seq advances past torn copy");
    CHECK(om_boot_meta_read(&g_meta, &readback, &copy) == OM_OK, "read new latest entry");
    CHECK(copy == 0u && readback.seq == 5u, "new entry in copy 0");
}

static void test_erase_failure(void)
{
    OmBootMetaEntry older = make_entry(1u, OM_BOOT_META_STATE_VALID, 0u, 0u, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry next = make_entry(1u, OM_BOOT_META_STATE_PENDING, 0u, 0u, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry readback;
    uint32_t copy = 0xFFFFFFFFu;

    printf("[T8] erase failure keeps latest valid entry\n");
    CHECK(om_boot_meta_cleanup(&g_meta) == OM_OK, "cleanup before erase-failure case");
    older.seq = 1u;
    write_raw_entry(1u, &older); /* 让目标副本非空白，迫使写入前擦除 */
    CHECK(om_boot_meta_read(&g_meta, &readback, NULL) == OM_OK && readback.seq == 5u,
          "latest valid entry present before failure");

    flash_sim_set_fail(&g_sim, true);
    CHECK(om_boot_meta_write(&g_meta, &next) == OM_ERR_FLASH_IO, "erase failure propagates");
    flash_sim_set_fail(&g_sim, false);

    CHECK(om_boot_meta_read(&g_meta, &readback, &copy) == OM_OK, "read after erase failure");
    CHECK(copy == 0u && readback.seq == 5u, "latest valid entry survived");
}

static void test_write_failure(void)
{
    OmBootMetaEntry next = make_entry(0u, OM_BOOT_META_STATE_VALID, 0u, 0u, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry readback;
    uint32_t copy = 0xFFFFFFFFu;

    printf("[T9] program failure keeps latest valid entry\n");
    CHECK(om_boot_meta_cleanup(&g_meta) == OM_OK, "cleanup before program-failure case");
    CHECK(copy_is_erased(1u) == 1, "copy 1 is erased");

    flash_sim_set_fail(&g_sim, true);
    CHECK(om_boot_meta_write(&g_meta, &next) == OM_ERR_FLASH_IO, "program failure propagates");
    flash_sim_set_fail(&g_sim, false);

    CHECK(om_boot_meta_read(&g_meta, &readback, &copy) == OM_OK, "read after program failure");
    CHECK(copy == 0u && readback.seq == 5u, "latest valid entry survived");
}

static void test_invalid_args(void)
{
    OmBootMetaEntry entry = make_entry(0u, 99u, 0u, 0u, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry readback;

    printf("[T10] invalid arguments rejected\n");
    CHECK(om_boot_meta_write(&g_meta, &entry) == OM_ERR_INVALID_ARG, "invalid state rejected");
    entry.state = OM_BOOT_META_STATE_VALID;
    entry.activeSlot = 2u;
    CHECK(om_boot_meta_write(&g_meta, &entry) == OM_ERR_INVALID_ARG, "invalid slot rejected");
    CHECK(om_boot_meta_read(NULL, &readback, NULL) == OM_ERR_INVALID_ARG, "NULL context rejected");
}

int main(void)
{
    flash_sim_init(&g_sim, META_CAP, 0xFFu, 4u);
    g_sim.strictProgram = true;

    if (flash_register(&g_dev, "flash0", &g_geom, &g_ops, &g_sim) != OM_OK)
    {
        printf("FAIL: register flash0\n");
        return 1;
    }
    if (om_partition_open(&g_registry, "meta", &g_part) != OM_OK)
    {
        printf("FAIL: open meta partition\n");
        return 1;
    }
    if (om_boot_meta_init(&g_meta, &g_part) != OM_OK)
    {
        printf("FAIL: init meta context\n");
        return 1;
    }

    test_init_and_empty();
    test_write_valid();
    test_write_pending();
    test_write_when_both_valid();
    test_cleanup();
    test_write_after_cleanup();
    test_torn_write();
    test_erase_failure();
    test_write_failure();
    test_invalid_args();

    printf("\n结果：%d 通过 / %d 失败\n", g_pass, g_fail);
    flash_sim_deinit(&g_sim);
    return (g_fail == 0) ? 0 : 1;
}
