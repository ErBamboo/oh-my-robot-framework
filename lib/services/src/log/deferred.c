/**
 * @file deferred.c
 * @brief log 早期缓冲（deferred）：未就绪窗口（调度器前/SERVICE init 前）日志入固定环形，
 *        异步就绪后按条回放 + 丢弃后验告警
 * @details 窗口 = 程序启动 → log_async_init（OM_INIT_SERVICE）末尾：此窗口内后端可能尚未
 *          注册（EARLIEST/BOARD 级）、队列/日志线程不存在——日志不进扇出也不静默丢，
 *          入本缓冲（发起时间戳/发起顺序保留——回放按条 per-backend 过滤后扇出）。
 *          缓冲满 = 整条回滚 + 计数（线性保持最早——启动序列窗口；半条记录不入环）。
 *          丢弃后验告警（log_drop_warn）：队列满/缓冲满的丢弃在排空或回放后由日志侧补发
 *          WRN（节流 OM_LOG_DROP_WARN_INTERVAL_MS——增量 + 累计自证）。
 *          OM_LOG_ASYNC=0（同步模式恒直出）或 OM_LOG_DEFERRED=0 时本文件编译为空——deferred.c
 *          相关符号随 #if 消失，调用点同门控（core.c/log_async.c/stats.c）。
 */

#include "core/om_config.h"

#if OM_USE_LOG && OM_LOG_ASYNC && OM_LOG_DEFERRED

#include "core/om_def.h"
#include "core/om_interrupt.h"
#include "log_internal.h"
#include "osal/osal_time.h" /* 回放告警时间戳 osal_time_now_monotonic（host 测试经本地桩头） */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/** @brief 早期环形缓冲（线性保持最早——整条记录存储：满即丢新）
 *  记录布局 = [级别 1B][消息字节数 2B 小端][消息字节（含尾部 \n）]；
 *  整条不可拆（放不下 = 整条回滚 + 计数——回放不再需要帧同步） */
static uint8_t s_ring[OM_LOG_DEFERRED_BUF_SIZE];
static size_t s_ring_len;

/** @brief 早期窗口开启？（入环 → 回放关闭；幂等 flush 后恒 false） */
static bool s_active = true;

/** @brief 早期缓冲满丢弃计数（整条回滚——om_log_stats 汇总） */
static uint32_t s_dropped_deferred;

/** @brief 早期缓冲丢弃告警状态（本丢弃点独享；last_warn_ms 哨兵 = 从未告警——首次放行） */
static LogDropWarnState s_warn_deferred = {0U, UINT32_MAX};

/** @brief 框架内部告警模块实例（丢弃告警的消息头 module 标注；不占用模块注册表） */
static OmLogModule s_log_module = {"log", OM_LOG_LEVEL_DEBUG, -1};

bool log_deferred_active(void)
{
    return s_active;
}

/** @brief 环形写入上下文（当前记录的数据区偏移——ring_out 恢复用） */
typedef struct
{
    size_t base; /* 数据区起点（环内绝对偏移 = 记录起点 + 3） */
    size_t len;  /* 已写入字节数 */
    bool overflow;
} DeferredRingWriter;

/** @brief 段写入：放不下置 overflow（整条回滚由 log_deferred_emit 收尾）
 *  @param ctx DeferredRingWriter
 *  @param seg 段数据
 *  @param len 段字节数 */
static void ring_out(void *ctx, const char *seg, size_t len)
{
    DeferredRingWriter *w = (DeferredRingWriter *)ctx;
    if (w->overflow)
    {
        return;
    }
    if (w->base + w->len + len > OM_LOG_DEFERRED_BUF_SIZE)
    {
        w->overflow = true; /* 放不下：本记录后续段全弃（终态=整条回滚） */
        return;
    }
    memcpy(&s_ring[w->base + w->len], seg, len);
    w->len += len;
}

void log_deferred_emit(const OmLogModule *module, OmLogLevel level, const char *fmt, va_list ap)
{
    size_t start = s_ring_len;
    if (OM_LOG_DEFERRED_BUF_SIZE - start < 3)
    {
        s_dropped_deferred++; /* 记录头都放不下 = 缓冲已满（保持最早——丢弃新条目） */
        return;
    }
    s_ring[start] = (uint8_t)level;
    DeferredRingWriter w;
    w.base = start + 3;
    w.len = 0;
    w.overflow = false;
    log_emit_build(module, level, fmt, ap, ring_out, &w);
    if (w.overflow)
    {
        s_ring_len = start; /* 整条回滚（半条记录不入环——回放按整条处理） */
        s_dropped_deferred++;
    }
    else
    {
        s_ring[start + 1] = (uint8_t)(w.len & 0xFF);
        s_ring[start + 2] = (uint8_t)(w.len >> 8);
        s_ring_len = w.base + w.len;
    }
}

/** @brief 丢弃后验告警（丢弃点共同）：节流 + 增量报告——消息经 log_emit_args 直接 emit
 *  （不走队列——告警自身引发的队列满不再递归；WARN 级，per-backend 过滤正常生效）
 *  @param st 状态（每丢弃点一实例：warned_upto=已上报累计，last_warn_ms=上次上报时刻）
 *  @param module 告警模块实例（log_service_module——"log"）
 *  @param site 丢弃点描述（静态字符串；消息 %s 参数——生命周期框架内恒定）
 *  @param dropped 丢弃点累计丢弃数
 *  @param now_ms 单调毫秒（调用方取 osal_time_now_monotonic——host 测试可控）
 *  @return true = 已补发 WRN */
bool log_drop_warn(LogDropWarnState *st, const OmLogModule *module, const char *site,
                   uint32_t dropped, uint32_t now_ms)
{
    if (dropped <= st->warned_upto)
    {
        return false; /* 无新丢弃 */
    }
    if (st->last_warn_ms != UINT32_MAX &&
        now_ms - st->last_warn_ms < OM_LOG_DROP_WARN_INTERVAL_MS)
    {
        return false; /* 节流（回绕安全——无符号差） */
    }
    OmLogMsg msg;
    msg.fmt = "log drop: %s dropped %u (total %u)";
    msg.level = OM_LOG_LEVEL_WARN;
    msg.module = module;
    msg.argBuf[0] = (uintptr_t)site;
    msg.argBuf[1] = dropped - st->warned_upto; /* 增量 */
    msg.argBuf[2] = dropped;                   /* 累计 */
    msg.argCount = 3;
    log_emit_args(&msg);
    st->warned_upto = dropped;
    st->last_warn_ms = now_ms;
    return true;
}

/** @brief 回放 + 关闭早期窗口（log_async_init 末尾；幂等——已关闭则无操作）
 *  @note 回放 = 逐条读环 → per-backend 过滤扇出（发起时间戳/发起顺序保留）→
 *        丢弃告警（WRN 节流）→ 清环。临界区同构（emit 链保护——扇出表读写）；
 *        告警在内层复用同一临界区（嵌套安全原语）。 */
void log_deferred_flush(void)
{
    if (!s_active)
    {
        return;
    }
    s_active = false;
    port_critical_key_t key = om_hw_disable_interrupt();
    size_t pos = 0;
    while (pos + 3 <= s_ring_len)
    {
        size_t rlen = (size_t)s_ring[pos + 1] | ((size_t)s_ring[pos + 2] << 8);
        if (pos + 3 + rlen > s_ring_len)
        {
            break; /* 防御：坏记录（内存损坏——正常流程整条写入，不应出现） */
        }
        log_backend_push_all((OmLogLevel)s_ring[pos], (const char *)&s_ring[pos + 3], rlen);
        pos += 3 + rlen;
    }
    s_ring_len = 0;
    if (s_dropped_deferred > 0)
    {
        (void)log_drop_warn(&s_warn_deferred, log_service_module(), "early-buffer",
                            s_dropped_deferred, osal_time_now_monotonic());
    }
    om_hw_restore_interrupt(key);
}

uint32_t log_deferred_dropped(void)
{
    return s_dropped_deferred;
}

const OmLogModule *log_service_module(void)
{
    return &s_log_module;
}

#endif /* OM_USE_LOG && OM_LOG_ASYNC && OM_LOG_DEFERRED */
