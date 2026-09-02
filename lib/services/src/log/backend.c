/**
 * @file backend.c
 * @brief log 输出后端注册表（定长数组，临界区保护；多生产者广播的扇出端）
 * @details 注册/注销/级别调节为管理类 API（线程上下文）；any_accepts/push_all
 *          在 log 临界区内被 core 调用（线程或中断上下文）。注册携带初始级别
 *          （过滤阈值），om_log_backend_set_level 为运行时动态调整。
 */

#include "core/om_config.h"

#if OM_USE_LOG

#include "core/om_def.h"
#include "core/om_interrupt.h"

#include "log_internal.h"

#include <string.h>

/** @brief 后端表项：level = 该后端运行时过滤阈值（msg.level >= level 才投递） */
typedef struct
{
    OmLogBackend *backend;
    OmLogLevel level;
    uint8_t used;
} LogBackendEntry;

/** @brief 后端注册表（定长数组，静态零初始化；所有读写均在临界区内，无初始化生命周期） */
static LogBackendEntry g_backends[OM_LOG_MAX_BACKENDS];

bool log_backend_any_accepts(OmLogLevel level)
{
    size_t i;
    for (i = 0; i < OM_LOG_MAX_BACKENDS; i++)
    {
        if (g_backends[i].used && level >= g_backends[i].level)
        {
            return true;
        }
    }
    return false;
}

void log_backend_push_all(OmLogLevel level, const char *seg, size_t len)
{
    size_t i;
    for (i = 0; i < OM_LOG_MAX_BACKENDS; i++)
    {
        if (g_backends[i].used && level >= g_backends[i].level)
        {
            g_backends[i].backend->push(g_backends[i].backend, seg, len);
        }
    }
}

/** @brief panic 投递：无 per-backend 级别过滤（提满全出——崩溃证据保全）；
 *  panic 钩子优先（最可靠通道）；NULL 钩子 → 退回 push（尽力而为）
 *  @param seg 段数据
 *  @param len 段字节数
 *  @note 与 log_backend_push_all 的差异 = 无过滤判定 + 提交通道选择（panic 优先/push 兜底）；
 *        调用者须在禁中断/故障上下文（om_log_panic 内已禁中断）——无表锁（故障上下文） */
void log_backend_panic_push_all(const char *seg, size_t len)
{
    size_t i;
    for (i = 0; i < OM_LOG_MAX_BACKENDS; i++)
    {
        if (g_backends[i].used)
        {
            if (g_backends[i].backend->panic != NULL)
            {
                g_backends[i].backend->panic(g_backends[i].backend, seg, len);
            }
            else
            {
                g_backends[i].backend->push(g_backends[i].backend, seg, len);
            }
        }
    }
}

OmRet om_log_backend_register(OmLogBackend *backend, OmLogLevel level)
{
    size_t i;
    port_critical_key_t key;
    if (backend == NULL || backend->name == NULL || backend->push == NULL)
    {
        return OM_ERR_INVALID_ARG;
    }
    if (level >= OM_LOG_LEVEL_MAX)
    {
        return OM_ERR_INVALID_ARG;
    }
    key = om_hw_disable_interrupt();
    for (i = 0; i < OM_LOG_MAX_BACKENDS; i++)
    {
        if (g_backends[i].used && g_backends[i].backend == backend)
        {
            om_hw_restore_interrupt(key);
            return OM_ERR_ALREADY;
        }
    }
    for (i = 0; i < OM_LOG_MAX_BACKENDS; i++)
    {
        if (!g_backends[i].used)
        {
            g_backends[i].backend = backend;
            g_backends[i].level = level;
            g_backends[i].used = 1;
            om_hw_restore_interrupt(key);
            return OM_OK;
        }
    }
    om_hw_restore_interrupt(key);
    return OM_ERR_FULL;
}

OmRet om_log_backend_unregister(OmLogBackend *backend)
{
    size_t i;
    port_critical_key_t key;
    if (backend == NULL)
    {
        return OM_ERR_INVALID_ARG;
    }
    key = om_hw_disable_interrupt();
    for (i = 0; i < OM_LOG_MAX_BACKENDS; i++)
    {
        if (g_backends[i].used && g_backends[i].backend == backend)
        {
            g_backends[i].used = 0;
            om_hw_restore_interrupt(key);
            return OM_OK;
        }
    }
    om_hw_restore_interrupt(key);
    return OM_ERR_NOT_FOUND;
}

OmRet om_log_backend_set_level(const char *backend_name, OmLogLevel level)
{
    size_t i;
    port_critical_key_t key;
    if (backend_name == NULL || level >= OM_LOG_LEVEL_MAX)
    {
        return OM_ERR_INVALID_ARG;
    }
    key = om_hw_disable_interrupt();
    for (i = 0; i < OM_LOG_MAX_BACKENDS; i++)
    {
        if (g_backends[i].used && strcmp(g_backends[i].backend->name, backend_name) == 0)
        {
            g_backends[i].level = level;
            om_hw_restore_interrupt(key);
            return OM_OK;
        }
    }
    om_hw_restore_interrupt(key);
    return OM_ERR_NOT_FOUND;
}

#endif /* OM_USE_LOG */
