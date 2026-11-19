/**
 * @file   meta_contract.c
 * @brief  启动状态记录（meta）契约的公共校验
 */

#include "boot/contract/meta.h"

OmBool om_boot_meta_entry_schema_valid(const OmBootMetaEntry *entry)
{
    if (entry == NULL)
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
    return OM_TRUE;
}
