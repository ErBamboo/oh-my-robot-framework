/**
 * @file   meta.c
 * @brief  启动决策数据 entry 的读写与轮转实现
 */

#include "boot/meta.h"

#include <string.h>

#include "algorithm/checksum/crc32.h"

/** 单个副本的读取分类。 */
typedef enum OmBootMetaCopyState
{
    OM_BOOT_META_COPY_ERASED = 0,  /**< 全 0xFF，可直接写 */
    OM_BOOT_META_COPY_VALID = 1,   /**< CRC 与字段校验通过 */
    OM_BOOT_META_COPY_INVALID = 2, /**< 半写或损坏，写前须擦除 */
} OmBootMetaCopyState;

static uint32_t meta_crc32(const OmBootMetaEntry *entry)
{
    return om_crc32_iso_hdlc(OM_CRC32_INIT, entry, offsetof(OmBootMetaEntry, crc32));
}

static OmBool entry_is_erased(const OmBootMetaEntry *entry)
{
    const uint8_t *bytes = (const uint8_t *)entry;
    size_t i;

    for (i = 0u; i < sizeof(*entry); i++)
    {
        if (bytes[i] != 0xFFu)
        {
            return OM_FALSE;
        }
    }
    return OM_TRUE;
}

OmBool om_boot_meta_entry_valid(const OmBootMetaEntry *entry)
{
    if (entry == NULL)
    {
        return OM_FALSE;
    }
    if (entry->seq == OM_BOOT_META_SEQ_INVALID)
    {
        return OM_FALSE;
    }
    if (entry->state > (uint32_t)OM_BOOT_META_STATE_ABORTED)
    {
        return OM_FALSE;
    }
    if (entry->activeSlot > 1u)
    {
        return OM_FALSE;
    }
    if (entry->crc32 != meta_crc32(entry))
    {
        return OM_FALSE;
    }
    return OM_TRUE;
}

static OmRet read_copy(const OmBootMeta *meta, uint32_t index, OmBootMetaEntry *entry,
                       OmBootMetaCopyState *state)
{
    OmRet ret;

    ret = om_partition_read(&meta->partition, meta->copyOffset[index], entry, sizeof(*entry));
    if (ret != OM_OK)
    {
        return ret;
    }

    if (entry_is_erased(entry) == OM_TRUE)
    {
        *state = OM_BOOT_META_COPY_ERASED;
    }
    else if (om_boot_meta_entry_valid(entry) == OM_TRUE)
    {
        *state = OM_BOOT_META_COPY_VALID;
    }
    else
    {
        *state = OM_BOOT_META_COPY_INVALID;
    }
    return OM_OK;
}

OmRet om_boot_meta_init(OmBootMeta *meta, const OmPartitionHandle *partition)
{
    uint32_t count;
    uint32_t i;

    if (meta == NULL || partition == NULL)
    {
        return OM_ERR_INVALID_ARG;
    }

    count = om_partition_erase_unit_count(partition);
    if (count < OM_BOOT_META_COPIES)
    {
        return OM_ERR_INVALID_ARG;
    }

    memset(meta, 0, sizeof(*meta));
    meta->partition = *partition;
    /* 分区可以大于副本集合；模块只使用前 N 个独立的擦除单位。 */
    meta->copyCount = OM_BOOT_META_COPIES;

    for (i = 0u; i < OM_BOOT_META_COPIES; i++)
    {
        OmPartitionEraseUnit unit;
        OmRet ret = om_partition_erase_unit_at(partition, i, &unit);

        if (ret != OM_OK)
        {
            return ret;
        }
        if (unit.size < sizeof(OmBootMetaEntry))
        {
            return OM_ERR_INVALID_ARG;
        }
        meta->copyOffset[i] = unit.offset;
        meta->copySize[i] = unit.size;
    }

    return OM_OK;
}

OmRet om_boot_meta_read(const OmBootMeta *meta, OmBootMetaEntry *entry, uint32_t *copy_index)
{
    OmBootMetaEntry best = {0};
    OmBool found = OM_FALSE;
    uint32_t best_index = 0u;
    uint32_t i;

    if (meta == NULL || entry == NULL || meta->copyCount != OM_BOOT_META_COPIES)
    {
        return OM_ERR_INVALID_ARG;
    }

    for (i = 0u; i < meta->copyCount; i++)
    {
        OmBootMetaEntry candidate;
        OmBootMetaCopyState state;
        OmRet ret = read_copy(meta, i, &candidate, &state);

        if (ret != OM_OK)
        {
            return ret;
        }
        if (state == OM_BOOT_META_COPY_VALID)
        {
            if (found == OM_FALSE || candidate.seq > best.seq)
            {
                best = candidate;
                best_index = i;
                found = OM_TRUE;
            }
        }
    }

    if (found == OM_FALSE)
    {
        memset(entry, 0xFF, sizeof(*entry));
        return OM_ERR_NOT_FOUND;
    }

    *entry = best;
    if (copy_index != NULL)
    {
        *copy_index = best_index;
    }
    return OM_OK;
}

static uint32_t choose_target(const OmBootMeta *meta, const OmBootMetaCopyState *states,
                              uint32_t latest_index)
{
    uint32_t i;

    for (i = 0u; i < meta->copyCount; i++)
    {
        if (states[i] == OM_BOOT_META_COPY_ERASED)
        {
            return i;
        }
    }
    for (i = 0u; i < meta->copyCount; i++)
    {
        if (states[i] == OM_BOOT_META_COPY_INVALID)
        {
            return i;
        }
    }

    /* 所有副本都有效：选一个不是最新的副本；双副本时即另一个。 */
    return (latest_index + 1u) % meta->copyCount;
}

OmRet om_boot_meta_write(OmBootMeta *meta, OmBootMetaEntry *entry)
{
    OmBootMetaEntry copies[OM_BOOT_META_COPIES];
    OmBootMetaCopyState states[OM_BOOT_META_COPIES];
    OmBool has_latest = OM_FALSE;
    uint32_t latest_index = 0u;
    uint32_t latest_seq = 0u;
    uint32_t target;
    uint32_t next_seq;
    uint32_t i;
    OmRet ret;

    if (meta == NULL || entry == NULL || meta->copyCount != OM_BOOT_META_COPIES)
    {
        return OM_ERR_INVALID_ARG;
    }
    if (entry->state > (uint32_t)OM_BOOT_META_STATE_ABORTED || entry->activeSlot > 1u)
    {
        return OM_ERR_INVALID_ARG;
    }

    for (i = 0u; i < meta->copyCount; i++)
    {
        ret = read_copy(meta, i, &copies[i], &states[i]);
        if (ret != OM_OK)
        {
            return ret;
        }
        if (states[i] == OM_BOOT_META_COPY_VALID &&
            (has_latest == OM_FALSE || copies[i].seq > latest_seq))
        {
            latest_index = i;
            latest_seq = copies[i].seq;
            has_latest = OM_TRUE;
        }
    }

    next_seq = (has_latest == OM_TRUE) ? (latest_seq + 1u) : 1u;
    if (next_seq == OM_BOOT_META_SEQ_INVALID)
    {
        return OM_ERR_OVERFLOW;
    }

    target = choose_target(meta, states, latest_index);
    if (target >= meta->copyCount)
    {
        return OM_ERR_INVALID_ARG;
    }

    if (states[target] != OM_BOOT_META_COPY_ERASED)
    {
        ret = om_partition_erase_range(&meta->partition, meta->copyOffset[target],
                                       meta->copySize[target]);
        if (ret != OM_OK)
        {
            return ret;
        }
    }

    entry->seq = next_seq;
    entry->crc32 = meta_crc32(entry);

    ret = om_partition_write(&meta->partition, meta->copyOffset[target], entry,
                             offsetof(OmBootMetaEntry, crc32));
    if (ret != OM_OK)
    {
        return ret;
    }
    ret = om_partition_write(&meta->partition,
                             meta->copyOffset[target] + (uint32_t)offsetof(OmBootMetaEntry, crc32),
                             &entry->crc32, sizeof(entry->crc32));
    if (ret != OM_OK)
    {
        return ret;
    }

    return OM_OK;
}

OmRet om_boot_meta_cleanup(OmBootMeta *meta)
{
    OmBootMetaCopyState states[OM_BOOT_META_COPIES];
    OmBool found = OM_FALSE;
    uint32_t latest_index = 0u;
    uint32_t latest_seq = 0u;
    uint32_t i;

    if (meta == NULL || meta->copyCount != OM_BOOT_META_COPIES)
    {
        return OM_ERR_INVALID_ARG;
    }

    for (i = 0u; i < meta->copyCount; i++)
    {
        OmBootMetaEntry candidate;
        OmRet ret = read_copy(meta, i, &candidate, &states[i]);

        if (ret != OM_OK)
        {
            return ret;
        }
        if (states[i] == OM_BOOT_META_COPY_VALID &&
            (found == OM_FALSE || candidate.seq > latest_seq))
        {
            latest_index = i;
            latest_seq = candidate.seq;
            found = OM_TRUE;
        }
    }

    for (i = 0u; i < meta->copyCount; i++)
    {
        if (found == OM_TRUE && i == latest_index)
        {
            continue;
        }
        if (states[i] != OM_BOOT_META_COPY_ERASED)
        {
            OmRet ret = om_partition_erase_range(&meta->partition, meta->copyOffset[i],
                                                 meta->copySize[i]);
            if (ret != OM_OK)
            {
                return ret;
            }
        }
    }

    return OM_OK;
}
