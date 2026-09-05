/**
 * @file log_async.c
 * @brief log 异步路径：参数包入队（调用侧 ~1-2µs）→ 日志线程格式化+扇出
 * @details OM_LOG_ASYNC 专属（同文件 #if OM_USE_LOG && OM_LOG_ASYNC 包裹；最小配置 #undef
 *          OM_LOG_ASYNC 时本文件编译为空——调用侧走向上的同步兜底）。
 *          就绪判定 log_async_can_enqueue（队列已建）：调度器前/SERVICE init 前为
 *          false——调用方（core.c om_log_log）走早期缓冲（deferred 开）或同步兜底。
 *          生产者经 log_async_send 入队：线程（osal_queue_send 非阻塞）/ ISR 自动分流
 *          （osal_is_in_isr 判定 → osal_queue_send_from_isr）——ISR 安全入队；
 *          日志线程（LOW 带）循环 recv → log_format_args → 复用 emit 链路。
 *          队列满丢弃+计数（printk 语义，T6 查询 API 读取）。
 */

#include "core/om_config.h"

#if OM_USE_LOG && OM_LOG_ASYNC

#include "core/om_def.h"
#include "core/om_init.h"
#include "core/om_interrupt.h"
#include "log_internal.h"
#include "osal/osal.h"

#include <stdbool.h>
#include <string.h>

/** @brief 异步队列（定长元素值拷贝——参数包整体入队；满丢弃 printk 语义） */
static OsalQueue *s_log_queue;

/** @brief 丢计数（队列满——T6 查询 API 汇总） */
static uint32_t s_dropped_queue;

/** @brief 异步路径就绪？队列已建（调度器后、线程创建前窗口内为 false——调用方走同步兜底）
 *  @return true = 就绪（log_async_send 可用） */
bool log_async_can_enqueue(void)
{
    return s_log_queue != NULL;
}

bool log_async_send(const OmLogMsg *msg)
{
    if (msg == NULL)
    {
        return false;
    }
    OsalStatus st;
    if (osal_is_in_isr())
    {
        st = osal_queue_send_from_isr(s_log_queue, msg); /* ISR：非阻塞，无断言 */
    }
    else
    {
        st = osal_queue_send(s_log_queue, msg, 0); /* 线程：非阻塞，满即 WOULD_BLOCK */
    }
    if (st != OSAL_OK)
    {
        /* 计数临界区保护（ISR 与线程并发增量的竞态） */
        port_critical_key_t key = om_hw_disable_interrupt();
        s_dropped_queue++;
        om_hw_restore_interrupt(key);
        return false;
    }
    return true;
}

/** @brief 读取队列满丢弃计数（static s_dropped_queue 的跨文件访问器——om_log_stats 汇总）
 *  @return 累计队列满丢弃数 */
uint32_t log_dropped_queue(void)
{
    return s_dropped_queue;
}

#if OM_LOG_DEFERRED
/** @brief 队列满丢弃告警状态（本丢弃点独享——与 deferred 互不串扰；
 *  哨兵 = 从未告警——首个事件放行，静态零初始化会让哨兵失效） */
static LogDropWarnState s_warn_queue = {0U, UINT32_MAX};

/** @brief 队列满丢弃后验告警：排水后检查（节流——OM_LOG_DROP_WARN_INTERVAL_MS）
 *  @param now_ms 单调毫秒（线程循环取 osal_time_now_monotonic） */
static void log_async_warn_queue(uint32_t now_ms)
{
    if (s_dropped_queue > 0)
    {
        (void)log_drop_warn(&s_warn_queue, log_service_module(), "queue-full", s_dropped_queue,
                            now_ms);
    }
}
#endif /* OM_LOG_DEFERRED */

/** @brief 日志线程入口：recv（阻塞, FOREVER）→ emit（复用 core 链路——全参随 msg 传递）
 *  @param arg 未使用 */
static void log_async_thread(void *arg)
{
    OmLogMsg msg;
    (void)arg;
    for (;;)
    {
        if (osal_queue_recv(s_log_queue, &msg, OSAL_WAIT_FOREVER) != OSAL_OK)
        {
            continue;
        }
        log_emit_args(&msg); /* 复用 core 链路（全参随 msg 一次传递） */
#if OM_LOG_DEFERRED
        log_async_warn_queue(osal_time_now_monotonic()); /* 排水后查丢弃（后验告警） */
#endif
    }
}

/** @brief 异步模式初始化：建队列（定长值拷贝）+ 日志线程（LOW 带；名 "log_thread"）+
 *         早期缓冲回放（deferred——不论成败，窗口在此关闭）
 *  @return OM_OK 成功；OM_ERR_NO_MEM 队列/线程创建失败 */
OmRet log_async_init(void)
{
    OmRet ret = OM_OK;
    if (osal_queue_create(&s_log_queue, OM_LOG_QUEUE_LEN, sizeof(OmLogMsg)) != OSAL_OK)
    {
        ret = OM_ERR_NO_MEM;
    }
    else
    {
        OsalThreadAttr attr = {"log_thread", 0U, OSAL_PRIO_LOW_BASE};
        OsalThread *thread = NULL;
        if (osal_thread_create(&thread, &attr, log_async_thread, NULL) != OSAL_OK)
        {
            ret = OM_ERR_NO_MEM;
        }
    }
#if OM_LOG_DEFERRED
    /* 关闭早期窗口：回放（发起时间戳/顺序；按条过滤）。队列创建失败时后续日志走
     * 同步兜底（not 早期缓冲——窗口已关，v1 语义兜底） */
    log_deferred_flush();
#endif
    return ret;
}
OM_INIT_SERVICE(log_async_init); /* SERVICE 级：调度器后，可阻塞/建线程；
                                  * 之前日志走同步兜底（未就绪判定） */

#endif /* OM_LOG_ASYNC */
