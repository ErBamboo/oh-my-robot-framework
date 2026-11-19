/**
 * @file   boot_meta_store_test.c
 * @brief  Meta 对象 host 测试（双副本轮转、擦除与掉电破坏）
 *
 * 夹具：16K 均匀扇区的模拟 flash，meta 分区 = 3 个擦除单位，模块只使用前 2 个。
 * 测试覆盖空分区、单份写入、双份都有效、写者擦除非最新副本、清理、半写记录、
 * 擦除失败、写入失败与 seq 推进；额外擦除单位必须保持不动。
 *
 * 退出码 0=全绿；非 0=有 FAIL。
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "algorithm/checksum/crc32.h"
#include "boot/contract/meta.h"
#include "boot/meta.h"
#include "drivers/model/device.h"
#include "drivers/peripheral/flash/pal_flash_dev.h"
#include "drivers/storage/partition.h"

#include "flash_sim.h"

#define SECT_SIZE 0x4000u
#define META_CAP (SECT_SIZE * 3u)

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

static OmBootMetaData make_data(uint32_t slot, OmBootMetaState state, uint32_t version,
                                uint32_t confirmed_mask, OmBool trial, OmBootMetaReason reason)
{
    OmBootMetaData data;

    memset(&data, 0, sizeof(data));
    data.state = state;
    data.activeSlot = slot;
    data.imageVersion = version;
    data.confirmed[0] =
        ((confirmed_mask & OM_BOOT_META_FLAG_SLOT_A_CONFIRMED) != 0u) ? OM_TRUE : OM_FALSE;
    data.confirmed[1] =
        ((confirmed_mask & OM_BOOT_META_FLAG_SLOT_B_CONFIRMED) != 0u) ? OM_TRUE : OM_FALSE;
    data.trialFromRollback = trial;
    data.layoutFingerprint = 0x1234u;
    data.reason = reason;
    return data;
}

static OmBootMetaEntry make_raw_entry(uint32_t slot, uint32_t state, uint32_t version,
                                      uint32_t flags, uint32_t reason)
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

static OmRet read_raw_entry(uint32_t copy, OmBootMetaEntry *entry)
{
    return raw_read(g_meta.copyOffset[copy], entry, sizeof(*entry));
}

static void write_raw_entry(uint32_t copy, OmBootMetaEntry *entry)
{
    entry->crc32 = entry_crc(entry);
    (void)raw_write(g_meta.copyOffset[copy], entry, sizeof(*entry));
}

static int offset_is_erased(uint32_t offset)
{
    OmBootMetaEntry entry;
    const uint8_t *bytes = (const uint8_t *)&entry;
    size_t i;

    if (raw_read(offset, &entry, sizeof(entry)) != OM_OK)
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

static int copy_is_erased(uint32_t copy)
{
    return offset_is_erased(g_meta.copyOffset[copy]);
}

static void test_init_and_empty(void)
{
    OmBootMetaReadResult read;

    printf("[T1] init and empty read\n");
    CHECK(g_meta.copyCount == 2u, "copy count = 2");
    CHECK(g_meta.copyOffset[0] == 0u && g_meta.copyOffset[1] == SECT_SIZE,
          "copies start at erase-unit boundaries");
    CHECK(g_meta.copySize[0] == SECT_SIZE && g_meta.copySize[1] == SECT_SIZE,
          "each copy owns one erase unit");
    CHECK(offset_is_erased(SECT_SIZE * 2u) == 1, "extra erase unit remains untouched");
    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK &&
              read.status == OM_BOOT_META_READ_NO_RECORD,
          "blank partition has no record");
}

static void test_write_valid(void)
{
    OmBootMetaData data = make_data(0u, OM_BOOT_META_STATE_VALID, 0x01020300u,
                                    OM_BOOT_META_FLAG_SLOT_A_CONFIRMED, OM_FALSE,
                                    OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry raw;
    OmBootMetaReadResult read;

    printf("[T2] first write\n");
    CHECK(om_boot_meta_write(&g_meta, &data) == OM_OK, "write VALID record");
    CHECK(read_raw_entry(0u, &raw) == OM_OK, "read raw copy 0");
    CHECK(raw.seq == 1u && raw.crc32 == entry_crc(&raw), "seq and CRC filled by writer");
    CHECK((raw.flags >> OM_BOOT_META_FLAG_FINGERPRINT_SHIFT) == 0x1234u,
          "layout fingerprint encoded");
    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK && read.status == OM_BOOT_META_READ_OK,
          "read back VALID record");
    CHECK(read.record.copyIndex == 0u && read.record.seq == 1u, "first write uses copy 0");
    CHECK(read.record.data.state == OM_BOOT_META_STATE_VALID && read.record.data.activeSlot == 0u &&
              read.record.data.imageVersion == 0x01020300u && read.record.data.confirmed[0] == OM_TRUE,
          "record fields round-trip");
    CHECK(copy_is_erased(1u) == 1, "other copy remains erased");
}

static void test_write_pending(void)
{
    OmBootMetaData data = make_data(1u, OM_BOOT_META_STATE_PENDING, 0x01020400u, 0u,
                                    OM_FALSE, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry raw;
    OmBootMetaReadResult read;

    printf("[T3] second write uses erased copy\n");
    CHECK(om_boot_meta_write(&g_meta, &data) == OM_OK, "write PENDING record");
    CHECK(read_raw_entry(1u, &raw) == OM_OK && raw.seq == 2u, "seq advances");
    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK && read.record.copyIndex == 1u &&
              read.record.data.state == OM_BOOT_META_STATE_PENDING && read.record.data.activeSlot == 1u,
          "latest record is the PENDING one in copy 1");
}

static void test_write_when_both_valid(void)
{
    OmBootMetaData data = make_data(0u, OM_BOOT_META_STATE_VALID, 0x01020500u,
                                    OM_BOOT_META_FLAG_SLOT_A_CONFIRMED, OM_FALSE,
                                    OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry raw;
    OmBootMetaReadResult read;

    printf("[T4] both copies valid, writer erases older\n");
    CHECK(om_boot_meta_write(&g_meta, &data) == OM_OK, "write VALID with no erased copy");
    CHECK(read_raw_entry(0u, &raw) == OM_OK && raw.seq == 3u,
          "seq advances past two valid copies");
    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK && read.record.copyIndex == 0u &&
              read.record.seq == 3u && read.record.data.state == OM_BOOT_META_STATE_VALID,
          "writer erased older copy and wrote new latest");
}

static void test_cleanup(void)
{
    OmBootMetaReadResult read;

    printf("[T5] boot cleanup keeps latest and one erased copy\n");
    CHECK(om_boot_meta_cleanup(&g_meta) == OM_OK, "cleanup succeeds");
    CHECK(copy_is_erased(1u) == 1, "non-latest copy erased");
    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK && read.record.copyIndex == 0u && read.record.seq == 3u,
          "latest record survives cleanup");
}

static void test_write_after_cleanup(void)
{
    OmBootMetaData data = make_data(1u, OM_BOOT_META_STATE_PENDING, 0x01020600u, 0u,
                                    OM_FALSE, OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry raw;
    OmBootMetaReadResult read;

    printf("[T6] write uses the erased copy left by cleanup\n");
    CHECK(om_boot_meta_write(&g_meta, &data) == OM_OK, "write after cleanup");
    CHECK(read_raw_entry(1u, &raw) == OM_OK && raw.seq == 4u, "seq advances");
    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK && read.record.copyIndex == 1u &&
              read.record.seq == 4u,
          "latest record in copy 1");
}

static void test_torn_write(void)
{
    OmBootMetaEntry torn = make_raw_entry(0u, OM_BOOT_META_STATE_VALID, 0u, 0u,
                                          OM_BOOT_META_REASON_NONE);
    OmBootMetaData data = make_data(0u, OM_BOOT_META_STATE_VALID, 0u, 0u, OM_FALSE,
                                    OM_BOOT_META_REASON_NONE);
    OmBootMetaEntry raw;
    OmBootMetaReadResult read;

    printf("[T7] half-written record is ignored\n");
    CHECK(om_boot_meta_cleanup(&g_meta) == OM_OK, "cleanup before torn-write case");
    CHECK(copy_is_erased(0u) == 1, "copy 0 is erased");

    torn.seq = 99u;
    torn.crc32 = 0u; /* 故意错误：模拟 CRC 未写完 */
    CHECK(raw_write(g_meta.copyOffset[0], &torn, sizeof(torn)) == OM_OK,
          "program torn record into erased copy");
    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK && read.record.copyIndex == 1u &&
              read.record.seq == 4u,
          "read ignores torn record");

    CHECK(om_boot_meta_write(&g_meta, &data) == OM_OK, "write over torn copy");
    CHECK(read_raw_entry(0u, &raw) == OM_OK && raw.seq == 5u, "seq advances past torn copy");
    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK && read.record.copyIndex == 0u &&
              read.record.seq == 5u,
          "new record in copy 0");
}

static void test_erase_failure(void)
{
    OmBootMetaEntry older = make_raw_entry(1u, OM_BOOT_META_STATE_VALID, 0u, 0u,
                                           OM_BOOT_META_REASON_NONE);
    OmBootMetaData data = make_data(1u, OM_BOOT_META_STATE_PENDING, 0u, 0u, OM_FALSE,
                                    OM_BOOT_META_REASON_NONE);
    OmBootMetaReadResult read;

    printf("[T8] erase failure keeps latest valid record\n");
    CHECK(om_boot_meta_cleanup(&g_meta) == OM_OK, "cleanup before erase-failure case");
    older.seq = 1u;
    write_raw_entry(1u, &older); /* 让目标副本非空白，迫使写入前擦除 */
    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK && read.record.seq == 5u,
          "latest valid record present before failure");

    flash_sim_set_fail(&g_sim, true);
    CHECK(om_boot_meta_write(&g_meta, &data) == OM_ERR_FLASH_IO, "erase failure propagates");
    flash_sim_set_fail(&g_sim, false);

    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK && read.record.copyIndex == 0u &&
              read.record.seq == 5u,
          "latest valid record survived");
}

static void test_write_failure(void)
{
    OmBootMetaData data = make_data(0u, OM_BOOT_META_STATE_VALID, 0u, 0u, OM_FALSE,
                                    OM_BOOT_META_REASON_NONE);
    OmBootMetaReadResult read;

    printf("[T9] program failure keeps latest valid record\n");
    CHECK(om_boot_meta_cleanup(&g_meta) == OM_OK, "cleanup before program-failure case");
    CHECK(copy_is_erased(1u) == 1, "copy 1 is erased");

    flash_sim_set_fail(&g_sim, true);
    CHECK(om_boot_meta_write(&g_meta, &data) == OM_ERR_FLASH_IO, "program failure propagates");
    flash_sim_set_fail(&g_sim, false);

    CHECK(om_boot_meta_read(&g_meta, &read) == OM_OK && read.record.copyIndex == 0u &&
              read.record.seq == 5u,
          "latest valid record survived");
}

static void test_invalid_args(void)
{
    OmBootMetaData data = make_data(0u, (OmBootMetaState)99u, 0u, 0u, OM_FALSE,
                                    OM_BOOT_META_REASON_NONE);
    OmBootMetaReadResult read;

    printf("[T10] invalid arguments rejected\n");
    CHECK(om_boot_meta_write(&g_meta, &data) == OM_ERR_INVALID_ARG, "invalid state rejected");
    data.state = OM_BOOT_META_STATE_VALID;
    data.activeSlot = 2u;
    CHECK(om_boot_meta_write(&g_meta, &data) == OM_ERR_INVALID_ARG, "invalid slot rejected");
    CHECK(om_boot_meta_read(NULL, &read) == OM_ERR_INVALID_ARG, "NULL context rejected");
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
        printf("FAIL: init meta object\n");
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
