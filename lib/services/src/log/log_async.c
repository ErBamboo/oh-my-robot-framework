/**
 * @file log_async.c
 * @brief log 异步消费：门铃（环 空→非空 post）→ 日志线程抽取环中消息 → emit（格式化+扇出）
 * @details OM_LOG_ASYNC 专属（同文件 #if OM_USE_LOG && OM_LOG_ASYNC 包裹；最小配置 #undef
 *          OM_LOG_ASYNC 时本文件编译为空——消费由现场触发承担，见 ring.c）。
 *          门铃（log_async_init 建，OsalSem 二值 max=1）：生产者（ring.c）仅在环 空→非空
 *          时 post——连续写不重复唤醒（pipe 模式）；日志线程（LOW 带）循环
 *          take（阻塞）→ log_ring_drain（含丢弃后验告警检查）。
 *          线程入口先 drain 一次：接住启动期（门铃未建、无信号可等）生产的滞留消息。
 *          失败退化：门铃/线程创建失败 → OM_ERR_NO_MEM（生产仍入环——无消费方则满丢+计数，
 *          后验告警在异常时无消费不发出——可接受）。
 */

#include "core/om_config.h"

#if OM_USE_LOG && OM_LOG_ASYNC

#include "core/om_def.h"
#include "core/om_init.h"
#include "log_internal.h"
#include "osal/osal.h"

/** @brief 门铃（环 空→非空 才 post——二值；pipe 模式） */
static OsalSem *s_doorbell;

/** @brief 日志线程入口：先行 drain（启动期滞留——门铃未建时无信号可等）→ take 循环
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

/** @brief 异步模式初始化：建门铃（二值信号量 max=1 init=0）+ 日志线程（LOW 带；名 "log_thread"）
 *  @return OM_OK 成功；OM_ERR_NO_MEM 门铃/线程创建失败 */
OmRet log_async_init(void)
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
OM_INIT_SERVICE(log_async_init); /* SERVICE 级：调度器后建门铃+线程（可阻塞/建线程）；
                                  * 之前生产流入环滞留——线程首轮 drain 接住 */

#endif /* OM_LOG_ASYNC */
