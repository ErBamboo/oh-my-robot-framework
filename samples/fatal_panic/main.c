/**
 * @file main.c
 * @brief fatal 现场输出组合示范：handler 覆盖（om_log_panic 记录现场）→ 恢复策略（不返回）；
 *        OM_INIT_APPLICATION 触发断言（fatal 演示）。
 * @details 记录不属于 fatal 设施（实现位于 services/log 的 om_log_panic——故障直出）；
 *          组合点 = handler 覆盖（上层）；fatal 设施零改动。接线见 verify_fatal_panic 目标。
 */

#include "core/om_assert.h"
#include "core/om_fatal.h"
#include "core/om_init.h"
#include "services/log/log.h"

OM_LOG_MODULE(fatal_panic, OM_LOG_LEVEL_INFO);

/** @brief handler 覆盖：记录现场（log 服务故障直出）→ 不返回（程序死亡）
 *  @param reason 触发源类别
 *  @param cause 具体错误码（OmRet）
 *  @param ctx 触发点上下文（file/line/pc/detail） */
void om_fatal_handler(OmFatalReason reason, OmRet cause, const OmFatalContext *ctx)
{
    om_log_panic(&_om_log_module, OM_LOG_LEVEL_FATAL,
                 "fatal reason=%d cause=%d at %s:%d pc=0x%lx detail=%s",
                 (int)reason, (int)cause,
                 (ctx->file == NULL) ? "?" : ctx->file,
                 ctx->line,
                 (unsigned long)ctx->pc,
                 (ctx->detail == NULL) ? "?" : ctx->detail);
    /* 恢复策略（亮灯/软复位/跳 bootloader）在此展开——示范从略，不得返回 */
    for (;;)
    {
    }
}

/** @brief 触发点（APPLICATION 级）：故意断言失败 → om_fatal_error → handler */
static OmRet trigger_fatal(void)
{
    OM_ASSERT(0 && "fatal panic demo: intentional assert");
    return OM_OK;
}
OM_INIT_APPLICATION(trigger_fatal);
