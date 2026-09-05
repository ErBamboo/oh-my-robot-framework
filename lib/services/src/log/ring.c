/**
 * @file ring.c
 * @brief log 统一消息环：生产恒入环（临界区串行）+ 消费抽环 + 门铃（异步）+ 丢弃后验告警
 * @details 容器 = data_struct/Ringbuf（SPSC 定长元素；纯原子无 OSAL——早期/最小配置可用；
 *          惰性初始化：静态缓冲零 init 生命周期——printk 同款）。
 *          生产（log_ring_produce）：临界区串行（多生产者收敛单写者角色——成本 = 消息拷贝
 *          级）ringbuf_in；满 = 丢新 + 计数。
 *          消费触发（OM_LOG_ASYNC 选择）：1 = 日志线程（门铃 OsalSem 二值，环 空→非空 才
 *          post——pipe 模式；门铃/线程/等待循环本文件单一属主——原 log_async.c 已并入）；
 *          0 = 现场触发——生产后判定 any_accepts(本消息)
 *          为真 → drain 全量（保生产序，含本消息与滞留段）→ 现场 emit；无后端 → 滞留环中
 *          （"deferred"回归形态）；服务就绪点（OM_INIT_SERVICE，仅 !ASYNC）→ 回放滞留段。
 *          消费（log_ring_drain）：循环 ringbuf_out → log_emit_args（单消费者 SPSC 读侧）；
 *          尾部丢弃后验告警（log_drop_warn——节流 + 增量；直接 emit 不走环防递归）。
 *          超限丢弃（参数包）不告警——只计数（msg.c）。
 */

#include "core/om_config.h"

#if OM_USE_LOG

#include "core/om_def.h"
#include "core/om_init.h" /* OM_INIT_SERVICE——消费调度器/服务就绪点自注册 */
#include "core/om_interrupt.h"
#include "data_struct/ringbuffer.h"
#include "log_internal.h"
#include "osal/osal.h" /* 门铃（osal_sem）+ 线程 + 时间戳（host 经本地桩头 shadow osal_time） */

#include <stdbool.h>
#include <stdint.h>

/** @brief 消息环（静态缓冲 + 惰性 init：ringbuf_init 一次性设置参数——零 init 生命周期） */
static Ringbuf s_rb;
static uint8_t s_buf[OM_LOG_RING_LEN * sizeof(OmLogMsg)];
static bool s_inited;

/** @brief 环满丢弃计数（om_log_stats 汇总） */
static uint32_t s_dropped_ring;

/** @brief 环满丢弃告警状态（本丢弃点独享；last_warn_ms 哨兵 = 从未告警——首次放行） */
static LogDropWarnState s_warn_ring = {0U, UINT32_MAX};

/** @brief 框架内部告警模块实例（丢弃告警的消息头 module 标注；不占用模块注册表） */
static OmLogModule s_log_module = {"log", OM_LOG_LEVEL_DEBUG, -1};

#if OM_LOG_ASYNC
/** @brief 门铃（环 空→非空 才 post——二值；NULL = 未建（创建前/早期——尚无可唤醒的消费者））
 *  @note 门铃/线程/等待循环同属本文件——消费调度器与环的单一属主；log_async.c 已并入 */
static OsalSem *s_doorbell;
#endif

/** @brief 惰性初始化（首次生产/消费触发；幂等——并发初化写入同参，无害） */
static void ring_ensure(void)
{
    if (!s_inited)
    {
        (void)ringbuf_init(&s_rb, s_buf, sizeof(OmLogMsg), OM_LOG_RING_LEN);
        s_inited = true;
    }
}

void log_ring_produce(const OmLogMsg *msg)
{
    port_critical_key_t key = om_hw_disable_interrupt(); /* 生产串行化（多生产者→单写者角色） */
    ring_ensure();
    bool was_empty = ringbuf_is_empty(&s_rb);
    if (ringbuf_in(&s_rb, msg, 1) == 0)
    {
        s_dropped_ring++; /* 满 = 丢新 + 计数（后验告警由消费侧补发——不阻塞调用方） */
        om_hw_restore_interrupt(key);
        return;
    }
#if OM_LOG_ASYNC
    if (was_empty && s_doorbell != NULL) /* 空→非空才 post（pipe 门铃模式——连续写不重复唤醒） */
    {
        (void)osal_sem_post_auto(s_doorbell); /* 线程/ISR 自动分流（post_from_isr 语义） */
    }
#else
    bool fire = log_backend_any_accepts(msg->level); /* 现场判定（临界区内读表——v1 同构） */
#endif
    om_hw_restore_interrupt(key);
#if !OM_LOG_ASYNC
    if (fire)
    {
        log_ring_drain(); /* 现场发射：drain 全量（保生产序——含本消息与滞留段） */
    }
#endif
}

void log_ring_drain(void)
{
    OmLogMsg msg;
    /* 环初始化仅生产侧执行（首次生产必然先于任何消费；若系统零日志，未初始化环
     * mask=0 → cap=1 → 空语义——消费侧安全空转） */
    while (ringbuf_out(&s_rb, &msg, 1))
    {
        log_emit_args(&msg); /* 消费时刻格式化+扇出（单消费者——SPSC 读侧原子） */
    }
    if (s_dropped_ring > 0)
    {
        (void)log_drop_warn(&s_warn_ring, log_service_module(), "ring-full", s_dropped_ring,
                            osal_time_now_monotonic());
    }
}

void log_ring_flush(void)
{
#if !OM_LOG_ASYNC
    log_ring_drain(); /* 服务就绪点：回放滞留段（当下后端表过滤——printk 语义） */
#endif
}

uint32_t log_dropped_ring(void)
{
    return s_dropped_ring;
}

/** @brief 丢弃后验告警（丢弃点共同）：节流 + 增量报告——消息经 log_emit_args 直接 emit
 *  （不走环——告警自身引发的环满不再递归；WRN 级，per-backend 过滤正常生效）
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
    msg.ts = now_ms;
    msg.argBuf[0] = (uintptr_t)site;
    msg.argBuf[1] = dropped - st->warned_upto; /* 增量 */
    msg.argBuf[2] = dropped;                   /* 累计 */
    msg.argCount = 3;
    log_emit_args(&msg);
    st->warned_upto = dropped;
    st->last_warn_ms = now_ms;
    return true;
}

const OmLogModule *log_service_module(void)
{
    return &s_log_module;
}

#if OM_LOG_ASYNC
/** @brief 日志线程入口：先行 drain（启动期滞留——门铃未建时生产无信号可等）→ take 循环
 *  @param arg 未使用 */
static void log_async_thread(void *arg)
{
    (void)arg;
    log_ring_drain(); /* 启动期滞留段（生产者早期在门铃未建时入环——无信号可等） */
    for (;;)
    {
        (void)osal_sem_wait(s_doorbell, OSAL_WAIT_FOREVER);
        log_ring_drain();
    }
}

/** @brief 异步模式初始化：建门铃（二值 max=1 init=0）+ 日志线程（LOW 带；名 "log_thread"）
 *  @return OM_OK 成功；OM_ERR_NO_MEM 门铃/线程创建失败 */
static OmRet log_async_init(void)
{
    if (osal_sem_create(&s_doorbell, 1U, 0U) != OSAL_OK)
    {
        return OM_ERR_NO_MEM;
    }
    OsalThreadAttr attr = {"log_thread", 0U, OSAL_PRIO_LOW_BASE};
    OsalThread *thread = NULL;
    if (osal_thread_create(&thread, &attr, log_async_thread, NULL) != OSAL_OK)
    {
        return OM_ERR_NO_MEM;
    }
    return OM_OK;
}
OM_INIT_SERVICE(log_async_init); /* SERVICE 级：调度器后建门铃+线程（可阻塞）；
                                  * 之前生产流入环滞留——线程首轮 drain 接住 */
#else
/** @brief 服务就绪点（OM_INIT_SERVICE）：回放滞留段——调度器后、后端已注册（DRIVER 级）
 *  @return OM_OK */
static OmRet log_ring_init(void)
{
    log_ring_flush();
    return OM_OK;
}
OM_INIT_SERVICE(log_ring_init); /* 同步模式：现场触发 + 服务就绪点回放 */
#endif

#endif /* OM_USE_LOG */
