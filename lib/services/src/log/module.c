/**
 * @file module.c
 * @brief log 模块登记：log_module_check_in 惰性登记（首次日志调用入库——幂等）
 * @details 表 = 定长数组（实例指针条目，键 = 实例指针——TU 静态唯一）；惰性：OM_LOG_MODULE
 *          生成实例 moduleId 初值 -1（未登记）→ 首次日志调用入表；抑制：表满写实例
 *          moduleId = -2（幂等短路，不重复扫描）。实例可写前提：OM_LOG_MODULE 生成非
 *          const 实例（本文件经指针写回 moduleId——契约见 log.h 宏注释）。
 */

#include "core/om_config.h"

#if OM_USE_LOG

#include "core/om_def.h"
#include "services/log/log.h" /* OmLogModule（登记条目类型） */

#include "log_internal.h"

/* 模块登记表上限（配置键落地前本文件默认值；om_appcfg.h 可覆写——同款 #ifndef 模式） */
#ifndef OM_LOG_MAX_MODULES
#define OM_LOG_MAX_MODULES 16
#endif

/* 返回码哨兵（契约见 log_internal.h：>=0 = moduleId；-2 = 表满；-3 = 参数非法） */
#define LOG_MODULE_ERR_FULL (-2)
#define LOG_MODULE_ERR_INVALID (-3)

/** @brief 模块登记表：条目 = 实例指针（幂等键；模块管理按 moduleId 索引得实例）
 *  @note 模块实例须由 OM_LOG_MODULE 生成（非 const static——登记写回 moduleId） */
static const OmLogModule *g_module_table[OM_LOG_MAX_MODULES];

int log_module_check_in(const OmLogModule *module)
{
    int i;
    if (module == NULL || module->name == NULL)
    {
        return LOG_MODULE_ERR_INVALID; /* 参数非法（无副作用） */
    }
    if (module->moduleId >= 0)
    {
        return module->moduleId; /* 已登记——幂等 */
    }
    if (module->moduleId == LOG_MODULE_ERR_FULL)
    {
        return LOG_MODULE_ERR_FULL; /* 表满已抑制——幂等（不重复查表） */
    }
    for (i = 0; i < OM_LOG_MAX_MODULES; i++)
    {
        if (g_module_table[i] == module)
        {
            ((OmLogModule *)module)->moduleId = i; /* 指针已在表（异常路径）——补登回写 */
            return i;
        }
    }
    for (i = 0; i < OM_LOG_MAX_MODULES; i++)
    {
        if (g_module_table[i] == NULL)
        {
            g_module_table[i] = module;
            ((OmLogModule *)module)->moduleId = i;
            return i;
        }
    }
    ((OmLogModule *)module)->moduleId = LOG_MODULE_ERR_FULL; /* 表满置 -2——抑制重复尝试 */
    return LOG_MODULE_ERR_FULL;
}

#endif /* OM_USE_LOG */
