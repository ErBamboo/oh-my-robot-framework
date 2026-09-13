/**
 * @file   partition.c
 * @brief  分区表抽象实现 v2（注册表 + 句柄，模块零状态）
 *
 * 实现要点：
 * - 零状态：无私有全局、无 init 依赖；全部 API 天然可重入；
 * - 注册表由调用方持有，模块只读其 table/count；
 * - 句柄域校验：index < reg->count，offset/size 恒从表取；
 * - 便捷层一律句柄入口，内部对权威条目重解析，杜绝外部描述符伪造；
 * - 器件访问经底层器件 API（flash_* 同步面）——对齐/容量由该层强制。
 */

#include <string.h>

#include "core/om_def.h"
#include "drivers/peripheral/flash/pal_flash_dev.h"
#include "drivers/storage/partition.h"

/* ===================================================================
 * 内部：注册表遍历
 * =================================================================== */

/** @brief 注册表可用性（表非空 + count 非零） */
static bool partition_reg_usable(const OmPartitionRegistry *reg)
{
    return reg != NULL && reg->table != NULL && reg->count > 0u;
}

/** @brief 按索引取条目指针（调用方保证 index < count） */
static const OmPartitionEntry *partition_entry_at(const OmPartitionRegistry *reg, uint32_t index)
{
    return &reg->table[index];
}

/** @brief 句柄域校验：注册表可用 + index 在域内（伪造/损坏句柄的公共判定） */
static bool partition_handle_valid(const OmPartitionHandle *h)
{
    return h != NULL && h->reg != NULL && partition_reg_usable(h->reg) &&
           h->index < h->reg->count;
}

/** @brief 权威表线性查找（表小；name 主键唯一）
 *  NULL name 条目视为畸形配置（registry_validate 可跳过、RAM 来源表可能半填）
 *  ——不可匹配，跳过：未经校验的表不得在 strcmp 上硬故障 */
static const OmPartitionEntry *partition_lookup(const OmPartitionRegistry *reg, const char *name)
{
    if (!partition_reg_usable(reg) || !name)
    {
        return NULL;
    }
    for (uint32_t i = 0; i < reg->count; i++)
    {
        const OmPartitionEntry *e = &reg->table[i];
        if (!e->name)
        {
            continue;
        }
        if (strcmp(e->name, name) == 0)
        {
            return e;
        }
    }
    return NULL;
}

/* ===================================================================
 * 内部：几何校验（v1 逻辑原样保留）
 * =================================================================== */

/** @brief 扇区友好判定：分区擦除闭包恰好等于自身——start 为扇区起点，
 *  size 为自 start 起整扇区数（跨 region 时逐段验证，均匀几何为退化情形） */
static bool is_partition_sector_aligned(const FlashGeometry *g, uint32_t off, uint32_t size)
{
    if (g->sectorSize > 0u)
    {
        /* 均匀几何：一个扇区大小贯穿全器件 */
        return off % g->sectorSize == 0u && size % g->sectorSize == 0u;
    }
    /* region 表几何：逐 region 消费，每段边界须为扇区边界 */
    uint32_t remaining = size;
    uint32_t cur = off;
    const FlashSectorRegion *r = g->sectorRegions;
    for (; r && remaining > 0u; r++)
    {
        uint32_t rStart = r->offset;
        uint32_t rLen = r->size * r->count;
        if (cur >= rStart + rLen)
        {
            continue; /* 分区整体在更靠后的 region */
        }
        if (cur < rStart || (cur - rStart) % r->size != 0u)
        {
            return false; /* 起点落在 region 间隙或非本 region 扇区边界 */
        }
        uint32_t avail = rStart + rLen - cur;
        uint32_t take = (remaining < avail) ? remaining : avail;
        if (take % r->size != 0u)
        {
            return false; /* 跨界点不是扇区边界（终点非整扇区） */
        }
        remaining -= take;
        cur += take;
    }
    return remaining == 0u; /* region 表耗尽仍未消费完 → 越界/非法 */
}

/** @brief 条目几何校验：器件解析 + 容量 + 扇区友好
 *  require_dev=false（全表校验）：器件未注册跳过——顺序解耦，几何由 open 期兜底；
 *  require_dev=true（open/操作期）：器件不存在即 NOT_FOUND（幽灵器件条目不阻断
 *  其它条目）。outDev 可空 = 调用方不需要器件指针 */
static OmRet partition_geom_check(const OmPartitionEntry *e, bool require_dev, FlashDev **outDev)
{
    FlashDev *dev = flash_find(e->devName);
    if (!dev)
    {
        return require_dev ? OM_ERR_NOT_FOUND : OM_OK;
    }
    const FlashGeometry *g = flash_geometry(dev);
    if (!g || e->offset >= g->capacity || e->size > g->capacity - e->offset)
    {
        return OM_ERR_INVALID_ARG; /* 越器件容量（或几何不可得） */
    }
    if (!is_partition_sector_aligned(g, e->offset, e->size))
    {
        return OM_ERR_INVALID_ARG; /* 非扇区友好：erase 闭包 != 自身 */
    }
    if (outDev)
    {
        *outDev = dev;
    }
    return OM_OK;
}

/** @brief 全表校验用条目校验（器件未注册 → 跳过，返回 OM_OK） */
static OmRet partition_entry_validate(const OmPartitionEntry *e)
{
    return partition_geom_check(e, false, NULL);
}

/* ===================================================================
 * 注册表面
 * =================================================================== */

OmRet om_partition_registry_validate(const OmPartitionRegistry *reg)
{
    if (!partition_reg_usable(reg))
    {
        return OM_ERR_INVALID_ARG;
    }
    /* 结构校验：字段非空、size 非零、offset+size 无溢出、重名 */
    for (uint32_t i = 0; i < reg->count; i++)
    {
        const OmPartitionEntry *e = partition_entry_at(reg, i);
        if (!e->name || !e->devName || e->size == 0u || e->offset > UINT32_MAX - e->size)
        {
            return OM_ERR_INVALID_ARG;
        }
        for (uint32_t j = 0; j < i; j++)
        {
            if (strcmp(reg->table[j].name, e->name) == 0)
            {
                return OM_ERR_INVALID_ARG;
            }
        }
    }
    /* 几何校验（器件未注册的条目跳过） */
    for (uint32_t i = 0; i < reg->count; i++)
    {
        OmRet ret = partition_entry_validate(partition_entry_at(reg, i));
        if (ret != OM_OK)
        {
            return ret;
        }
    }
    return OM_OK;
}

uint32_t om_partition_registry_count(const OmPartitionRegistry *reg)
{
    return partition_reg_usable(reg) ? reg->count : 0u;
}

OmRet om_partition_registry_at(const OmPartitionRegistry *reg, uint32_t index,
                               OmPartitionEntry *out)
{
    if (!partition_reg_usable(reg) || !out)
    {
        return OM_ERR_INVALID_ARG;
    }
    if (index >= reg->count)
    {
        return OM_ERR_NOT_FOUND;
    }
    *out = *partition_entry_at(reg, index);
    return OM_OK;
}

OmRet om_partition_query(const OmPartitionRegistry *reg, const char *name, OmPartitionEntry *out)
{
    if (!name || !out)
    {
        return OM_ERR_INVALID_ARG;
    }
    const OmPartitionEntry *e = partition_lookup(reg, name);
    if (!e)
    {
        return OM_ERR_NOT_FOUND; /* 含空表语义；*out 不动 */
    }
    *out = *e;
    return OM_OK;
}

/* ===================================================================
 * 解析面
 * =================================================================== */

OmRet om_partition_open(const OmPartitionRegistry *reg, const char *name,
                        OmPartitionHandle *h)
{
    if (!name || !h)
    {
        return OM_ERR_INVALID_ARG;
    }
    if (!partition_reg_usable(reg))
    {
        return OM_ERR_NOT_FOUND; /* 空表语义 */
    }
    bool has_malformed = false;
    for (uint32_t i = 0; i < reg->count; i++)
    {
        const OmPartitionEntry *e = partition_entry_at(reg, i);
        if (!e->name)
        {
            /* 畸形条目：未经 registry_validate 的 RAM 来源表可能半填。不可
             * strcmp（硬故障）；不阻断其它条目，仅在最终未命中时以 INVALID_ARG
             * 报出配置错（而非查找未命中） */
            has_malformed = true;
            continue;
        }
        if (strcmp(e->name, name) != 0)
        {
            continue;
        }
        OmRet ret = partition_geom_check(e, true, NULL);
        if (ret != OM_OK)
        {
            return ret;
        }
        h->reg = reg;
        h->index = i;
        return OM_OK;
    }
    return has_malformed ? OM_ERR_INVALID_ARG : OM_ERR_NOT_FOUND;
}

/* ===================================================================
 * 数据通路（句柄入口）
 * =================================================================== */

/** @brief 句柄域校验 + 权威条目/器件解析 + 分区内范围断言 */
static OmRet partition_range(const OmPartitionHandle *h, uint32_t off, size_t len,
                             const OmPartitionEntry **outE, FlashDev **outDev)
{
    if (!partition_handle_valid(h))
    {
        return OM_ERR_INVALID_ARG; /* 伪造/损坏句柄 */
    }
    const OmPartitionEntry *e = partition_entry_at(h->reg, h->index);
    if (off >= e->size || len > e->size - off)
    {
        return OM_ERR_INVALID_ARG; /* 双端越界（off < size 先行，杜绝 off+len 溢出） */
    }
    /* 器件按名重解析是每个操作的刻意代价：句柄若缓存 FlashDev*，器件指针即成
     * 可伪造/可悬垂的字段，"offset/size 恒从表取"的信任模型随之失效 */
    OmRet ret = partition_geom_check(e, true, outDev);
    if (ret != OM_OK)
    {
        return ret;
    }
    *outE = e;
    return OM_OK;
}

OmRet om_partition_read(const OmPartitionHandle *h, uint32_t off, void *buf, size_t len)
{
    if (!buf)
    {
        return OM_ERR_INVALID_ARG;
    }
    const OmPartitionEntry *e;
    FlashDev *dev;
    OmRet ret = partition_range(h, off, len, &e, &dev);
    if (ret != OM_OK)
    {
        return ret;
    }
    return flash_read(dev, e->offset + off, buf, len);
}

OmRet om_partition_write(const OmPartitionHandle *h, uint32_t off, const void *data, size_t len)
{
    if (!data)
    {
        return OM_ERR_INVALID_ARG;
    }
    const OmPartitionEntry *e;
    FlashDev *dev;
    OmRet ret = partition_range(h, off, len, &e, &dev);
    if (ret != OM_OK)
    {
        return ret;
    }
    return flash_write(dev, e->offset + off, data, len);
}

OmRet om_partition_erase(const OmPartitionHandle *h)
{
    if (!partition_handle_valid(h))
    {
        return OM_ERR_INVALID_ARG;
    }
    const OmPartitionEntry *e = partition_entry_at(h->reg, h->index);
    FlashDev *dev = flash_find(e->devName);
    if (!dev)
    {
        return OM_ERR_NOT_FOUND;
    }
    /* 整分区擦：扇区对齐由器件层强制（配置错误显式报 INVALID_ARG，不静默扩擦） */
    return flash_erase(dev, e->offset, e->size);
}

OmRet om_partition_erase_range(const OmPartitionHandle *h, uint32_t off, size_t len)
{
    if (!partition_handle_valid(h))
    {
        return OM_ERR_INVALID_ARG; /* 句柄有效性先行——与家族其它入口一致 */
    }
    if (len == 0u)
    {
        return OM_OK; /* 无操作（句柄已确认有效） */
    }
    const OmPartitionEntry *e;
    FlashDev *dev;
    OmRet ret = partition_range(h, off, len, &e, &dev);
    if (ret != OM_OK)
    {
        return ret;
    }
    /* 扇区对齐由器件层强制（flash_erase_validate）——非对齐显式报错，不扩擦 */
    return flash_erase(dev, e->offset + off, len);
}
