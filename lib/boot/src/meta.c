/**
 * @file   meta.c
 * @brief  启动状态记录持久化对象实现
 */

#include "boot/meta.h"

#include <string.h>

#include "algorithm/checksum/crc32.h"

/** 单个副本的读取分类。 */
typedef enum OmBootMetaCopyState
{
    OM_BOOT_META_COPY_ERASED = 0,       /**< 全 0xFF，可直接写 */
    OM_BOOT_META_COPY_VALID = 1,        /**< CRC 与字段校验通过，指纹兼容 */
    OM_BOOT_META_COPY_INVALID = 2,      /**< 半写或损坏，写前须擦除 */
    OM_BOOT_META_COPY_INCOMPATIBLE = 3, /**< 记录完整，但布局指纹与当前构建不符 */
} OmBootMetaCopyState;

/** 计算记录的 CRC；crc32 字段自身不参与。 */
static uint32_t meta_crc32(const OmBootMetaEntry *entry)
{
    return om_crc32_iso_hdlc(OM_CRC32_INIT, entry, offsetof(OmBootMetaEntry, crc32));
}

/** 判断记录是否保持擦除态（32 字节全 0xFF）。 */
static OmBool entry_is_erased(const OmBootMetaEntry *entry)
{
    const uint8_t *bytes = (const uint8_t *)entry; /* 按字节检查擦除值 */
    size_t i;                                      /* 记录字节序号 */

    for (i = 0u; i < sizeof(*entry); i++)
    {
        if (bytes[i] != 0xFFu)
        {
            return OM_FALSE;
        }
    }
    return OM_TRUE;
}

/** 判断记录能否作为有效副本参与比较。 */
static OmBool entry_is_valid(const OmBootMetaEntry *entry)
{
    if (om_boot_meta_entry_schema_valid(entry) == OM_FALSE)
    {
        return OM_FALSE;
    }
    if (entry->seq == OM_BOOT_META_SEQ_INVALID)
    {
        return OM_FALSE;
    }
    if (entry->crc32 != meta_crc32(entry))
    {
        return OM_FALSE;
    }
    return OM_TRUE;
}

/** 只读原始 32 字节，不做字段或 CRC 判定。 */
static OmRet read_copy_raw(const OmBootMeta *meta, uint32_t index, OmBootMetaEntry *entry)
{
    return om_partition_read(&meta->partition, meta->copyOffset[index], entry, sizeof(*entry));
}

/** 从记录中取出布局指纹。 */
static uint32_t entry_fingerprint(const OmBootMetaEntry *entry)
{
    return (entry->flags & OM_BOOT_META_FLAG_FINGERPRINT_MASK) >>
           OM_BOOT_META_FLAG_FINGERPRINT_SHIFT;
}

/** 对已读出的记录做空白/兼容/损坏分类。 */
static OmBootMetaCopyState classify_copy(const OmBootMetaEntry *entry,
                                         uint32_t expectedFingerprint)
{
    if (entry_is_erased(entry) == OM_TRUE)
    {
        return OM_BOOT_META_COPY_ERASED;
    }
    if (entry_is_valid(entry) == OM_FALSE)
    {
        return OM_BOOT_META_COPY_INVALID;
    }
    if (expectedFingerprint != OM_BOOT_META_FINGERPRINT_NONE &&
        entry_fingerprint(entry) != expectedFingerprint)
    {
        return OM_BOOT_META_COPY_INCOMPATIBLE;
    }
    return OM_BOOT_META_COPY_VALID;
}

/**
 * @brief 读取全部副本，分类并定位最新有效记录
 *
 * 读、写、清理共用本次扫描，避免重复实现分类与最新项选择。
 */
static OmRet scan_copies(const OmBootMeta *meta, OmBootMetaEntry *copies,
                         OmBootMetaCopyState *states, uint32_t *latest_index,
                         uint32_t *latest_seq, OmBool *has_latest)
{
    uint32_t i; /* 副本序号 */

    *latest_index = 0u;
    *latest_seq = 0u;
    *has_latest = OM_FALSE;

    for (i = 0u; i < meta->copyCount; i++)
    {
        OmRet ret = read_copy_raw(meta, i, &copies[i]); /* 当前副本读取结果 */

        if (ret != OM_OK)
        {
            return ret;
        }
        states[i] = classify_copy(&copies[i], meta->layoutFingerprint);
        if (states[i] == OM_BOOT_META_COPY_VALID &&
            (*has_latest == OM_FALSE || copies[i].seq > *latest_seq))
        {
            *latest_index = i;
            *latest_seq = copies[i].seq;
            *has_latest = OM_TRUE;
        }
    }

    return OM_OK;
}

/** 选择写入目标：优先空白副本，其次损坏副本，最后选最旧的非最新有效副本。 */
static uint32_t choose_target(const OmBootMeta *meta, const OmBootMetaEntry *copies,
                              const OmBootMetaCopyState *states, uint32_t latest_index)
{
    uint32_t i;      /* 副本序号 */
    uint32_t oldest; /* 非最新有效副本中 seq 最小者 */

    if (latest_index >= meta->copyCount)
    {
        return meta->copyCount;
    }

    for (i = 0u; i < meta->copyCount; i++)
    {
        if (states[i] == OM_BOOT_META_COPY_ERASED)
        {
            return i;
        }
    }
    for (i = 0u; i < meta->copyCount; i++)
    {
        if (states[i] == OM_BOOT_META_COPY_INVALID ||
            states[i] == OM_BOOT_META_COPY_INCOMPATIBLE)
        {
            return i;
        }
    }

    /* 所有副本都有效：排除最新项，按 seq 选最旧；两份时就是另一份。 */
    oldest = meta->copyCount;
    for (i = 0u; i < meta->copyCount; i++)
    {
        if (i == latest_index || states[i] != OM_BOOT_META_COPY_VALID)
        {
            continue;
        }
        if (oldest == meta->copyCount || copies[i].seq < copies[oldest].seq)
        {
            oldest = i;
        }
    }

    return oldest;
}

/** 把记录内容编码为 32 字节记录；seq 与 CRC 在写入时填写。 */
static void encode_entry(const OmBootMetaData *data, uint32_t fingerprint,
                         OmBootMetaEntry *entry)
{
    uint32_t flags; /* 指纹与状态迁移位 */

    memset(entry, 0, sizeof(*entry));
    entry->activeSlot = data->activeSlot;
    entry->state = (uint32_t)data->state;
    entry->bootCount = data->bootCount;
    entry->imageVersion = data->imageVersion;
    entry->reason = (uint32_t)data->reason;

    flags = (fingerprint & 0xFFFFu) << OM_BOOT_META_FLAG_FINGERPRINT_SHIFT;
    if (data->confirmed[0] == OM_TRUE)
    {
        flags |= OM_BOOT_META_FLAG_SLOT_A_CONFIRMED;
    }
    if (data->confirmed[1] == OM_TRUE)
    {
        flags |= OM_BOOT_META_FLAG_SLOT_B_CONFIRMED;
    }
    if (data->trialFromRollback == OM_TRUE)
    {
        flags |= OM_BOOT_META_FLAG_TRIAL_FROM_ROLLBACK;
    }
    if (data->confirmedWrite == OM_TRUE)
    {
        flags |= OM_BOOT_META_FLAG_CONFIRMED;
    }
    entry->flags = flags;
}

/** 把 32 字节记录解码为记录内容。 */
static void decode_entry(const OmBootMetaEntry *entry, OmBootMetaData *data)
{
    memset(data, 0, sizeof(*data));
    data->state = (OmBootMetaState)entry->state;
    data->activeSlot = entry->activeSlot;
    data->bootCount = entry->bootCount;
    data->imageVersion = entry->imageVersion;
    data->confirmed[0] =
        ((entry->flags & OM_BOOT_META_FLAG_SLOT_A_CONFIRMED) != 0u) ? OM_TRUE : OM_FALSE;
    data->confirmed[1] =
        ((entry->flags & OM_BOOT_META_FLAG_SLOT_B_CONFIRMED) != 0u) ? OM_TRUE : OM_FALSE;
    data->trialFromRollback =
        ((entry->flags & OM_BOOT_META_FLAG_TRIAL_FROM_ROLLBACK) != 0u) ? OM_TRUE : OM_FALSE;
    data->confirmedWrite =
        ((entry->flags & OM_BOOT_META_FLAG_CONFIRMED) != 0u) ? OM_TRUE : OM_FALSE;
    data->layoutFingerprint =
        (entry->flags & OM_BOOT_META_FLAG_FINGERPRINT_MASK) >> OM_BOOT_META_FLAG_FINGERPRINT_SHIFT;
    data->reason = (OmBootMetaReason)entry->reason;
}

/** 写一条已编码的记录；必要时擦除目标副本，CRC 最后写。 */
static OmRet write_entry(OmBootMeta *meta, OmBootMetaEntry *entry)
{
    OmBootMetaEntry copies[OM_BOOT_META_COPIES];     /* 各副本读出的原始记录 */
    OmBootMetaCopyState states[OM_BOOT_META_COPIES]; /* 各副本的分类结果 */
    OmBool has_latest;                               /* 是否存在有效记录 */
    uint32_t latest_index;                           /* 最新有效记录的副本序号 */
    uint32_t latest_seq;                             /* 最新有效记录的 seq */
    uint32_t target;                                 /* 本次写入选中的副本序号 */
    uint32_t next_seq;                               /* 新记录的序号 */
    OmRet ret;                                       /* 分区读写结果 */

    ret = scan_copies(meta, copies, states, &latest_index, &latest_seq, &has_latest);
    if (ret != OM_OK)
    {
        return ret;
    }

    /* seq 从 1 起步；0xFFFFFFFF 是无效哨兵，耗尽后报溢出。 */
    next_seq = (has_latest == OM_TRUE) ? (latest_seq + 1u) : 1u;
    if (next_seq == OM_BOOT_META_SEQ_INVALID)
    {
        return OM_ERR_OVERFLOW;
    }

    target = choose_target(meta, copies, states, latest_index);
    if (target >= meta->copyCount)
    {
        return OM_ERR_INVALID_ARG;
    }

    /* 只有目标不是空白副本时才擦除；最新有效副本永远不擦。 */
    if (states[target] != OM_BOOT_META_COPY_ERASED)
    {
        ret = om_partition_erase_range(&meta->partition, meta->copyOffset[target],
                                       meta->copySize[target]);
        if (ret != OM_OK)
        {
            return ret;
        }
    }

    /* 先写决策字段，最后写 CRC；CRC 未写完的副本会被读侧忽略。 */
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

OmRet om_boot_meta_init(OmBootMeta *meta, const OmPartitionHandle *partition)
{
    uint32_t count; /* 分区内可用擦除单位数 */
    uint32_t i;     /* 副本序号 */

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
    /* 分区可以大于副本集合；模块只使用前 N 个独立单位，多余单位不读、不擦、不校验。 */
    meta->copyCount = OM_BOOT_META_COPIES;
#ifdef OM_BOOT_LAYOUT_FINGERPRINT
    meta->layoutFingerprint = OM_BOOT_LAYOUT_FINGERPRINT;
#else
    meta->layoutFingerprint = OM_BOOT_META_FINGERPRINT_NONE;
#endif

    for (i = 0u; i < OM_BOOT_META_COPIES; i++)
    {
        OmPartitionEraseUnit unit; /* 第 i 个擦除单位的偏移与大小 */
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

OmRet om_boot_meta_read(const OmBootMeta *meta, OmBootMetaReadResult *out)
{
    OmBootMetaEntry copies[OM_BOOT_META_COPIES];     /* 各副本读出的原始记录 */
    OmBootMetaCopyState states[OM_BOOT_META_COPIES]; /* 各副本的分类结果 */
    OmBool has_latest;                               /* 是否存在有效记录 */
    uint32_t latest_index;                           /* 最新有效记录的副本序号 */
    uint32_t latest_seq;                             /* 最新有效记录的 seq */
    uint32_t i;                                      /* 副本序号 */
    OmRet ret;                                       /* 副本读取结果 */

    if (meta == NULL || out == NULL || meta->copyCount != OM_BOOT_META_COPIES)
    {
        return OM_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->status = OM_BOOT_META_READ_IO_ERROR;

    ret = scan_copies(meta, copies, states, &latest_index, &latest_seq, &has_latest);
    if (ret != OM_OK)
    {
        return OM_OK; /* 读取失败已折入 status */
    }

    if (has_latest == OM_FALSE)
    {
        out->status = OM_BOOT_META_READ_NO_RECORD;
        for (i = 0u; i < meta->copyCount; i++)
        {
            if (states[i] == OM_BOOT_META_COPY_INCOMPATIBLE)
            {
                out->status = OM_BOOT_META_READ_INCOMPATIBLE;
                break;
            }
            if (states[i] == OM_BOOT_META_COPY_INVALID)
            {
                out->status = OM_BOOT_META_READ_DAMAGED;
            }
        }
        return OM_OK;
    }

    out->status = OM_BOOT_META_READ_OK;
    out->record.copyIndex = latest_index;
    out->record.seq = latest_seq;
    decode_entry(&copies[latest_index], &out->record.data);
    return OM_OK;
}

OmRet om_boot_meta_write(OmBootMeta *meta, const OmBootMetaData *data)
{
    OmBootMetaEntry entry; /* 编码后的记录 */
    uint32_t fingerprint;  /* 写入记录的布局指纹 */

    if (meta == NULL || data == NULL || meta->copyCount != OM_BOOT_META_COPIES)
    {
        return OM_ERR_INVALID_ARG;
    }

    /* 有当前构建指纹时由框架盖章，避免调用方写入与构建不符的值。 */
    fingerprint = (meta->layoutFingerprint != OM_BOOT_META_FINGERPRINT_NONE)
                      ? meta->layoutFingerprint
                      : data->layoutFingerprint;
    encode_entry(data, fingerprint, &entry);
    if (om_boot_meta_entry_schema_valid(&entry) == OM_FALSE)
    {
        return OM_ERR_INVALID_ARG;
    }
    return write_entry(meta, &entry);
}

OmRet om_boot_meta_cleanup(OmBootMeta *meta)
{
    OmBootMetaEntry copies[OM_BOOT_META_COPIES];     /* 各副本读出的原始记录 */
    OmBootMetaCopyState states[OM_BOOT_META_COPIES]; /* 各副本的分类结果 */
    OmBool has_latest;                               /* 是否存在有效记录 */
    uint32_t latest_index;                           /* 最新有效记录的副本序号 */
    uint32_t latest_seq;                             /* 最新有效记录的 seq */
    uint32_t i;                                      /* 副本序号 */
    OmRet ret;                                       /* 分区擦除结果 */

    if (meta == NULL || meta->copyCount != OM_BOOT_META_COPIES)
    {
        return OM_ERR_INVALID_ARG;
    }

    /* 保留最新有效副本；没有有效副本时清空全部副本。 */
    ret = scan_copies(meta, copies, states, &latest_index, &latest_seq, &has_latest);
    if (ret != OM_OK)
    {
        return ret;
    }

    /* 只擦非最新副本；已是空白态的副本跳过。 */
    for (i = 0u; i < meta->copyCount; i++)
    {
        if (has_latest == OM_TRUE && i == latest_index)
        {
            continue;
        }
        if (states[i] != OM_BOOT_META_COPY_ERASED)
        {
            ret = om_partition_erase_range(&meta->partition, meta->copyOffset[i],
                                           meta->copySize[i]);
            if (ret != OM_OK)
            {
                return ret;
            }
        }
    }

    return OM_OK;
}
