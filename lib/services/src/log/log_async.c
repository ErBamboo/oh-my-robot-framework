/**
 * @file log_async.c
 * @brief log 异步模式：参数包入队（调用侧 ~1-2µs）→ 日志线程格式化+扇出
 * @details OM_LOG_MODE_ASYNC 专属（同文件 #ifdef OM_LOG_MODE_ASYNC 包裹）。
 *          生产者（线程上下文）只经 log_async_send 入队（非阻塞，满即丢+计数；
 *          ISR 生产需 send_from_isr 分流——当前未接线，见 ADR-0015 后续）；
 *          日志线程（LOW 带）循环 recv → log_format_args → 复用 emit 链路。
 *          队列满丢弃+计数（printk 语义，T6 查询 API 读取）。
 */

#include "core/om_config.h"

#ifdef OM_LOG_MODE_ASYNC

#include "core/om_def.h"
#include "core/om_init.h"
#include "log_internal.h"
#include "osal/osal.h"

#include <stdbool.h>
#include <string.h>

/** @brief 异步队列（定长元素值拷贝——参数包整体入队；满丢弃 printk 语义） */
static OsalQueue *s_log_queue;

/** @brief 丢计数（队列满——T6 查询 API 汇总） */
static uint32_t s_dropped_queue;

bool log_async_send(const OmLogMsg *msg)
{
    if (msg == NULL)
    {
        return false;
    }
    if (osal_queue_send(s_log_queue, msg, 0) != OSAL_OK) /* 非阻塞：满即 WOULD_BLOCK */
    {
        s_dropped_queue++;
        return false;
    }
    return true;
}

/** @brief 异步 emit：与同步 log_emit 同构——头部（含时间戳 T5 后）+ format_args + 扇出 + \n
 *  @param msg 参数包（fmt/level/module/args——%s 指针生命周期调用方保证，Zephyr 同款文档约束） */
static void log_async_emit(const OmLogMsg *msg)
{
    /* 复用 core.c 导出的 emit 链路（log_emit_args 声明见 log_internal.h） */
    log_emit_args(msg->module, msg->level, msg->fmt, msg->argBuf, msg->argCount);
}

/** @brief 日志线程入口：recv（阻塞, FOREVER）→ format_args → 复用 emit 输出链
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
        log_async_emit(&msg);
    }
}

/** @brief 异步模式初始化：建队列（定长值拷贝）+ 日志线程（LOW 带；名 "log_thread"）
 *  @return OM_OK 成功；OM_ERR_NO_MEM 队列/线程创建失败 */
OmRet log_async_init(void)
{
    if (osal_queue_create(&s_log_queue, OM_LOG_QUEUE_LEN, sizeof(OmLogMsg)) != OSAL_OK)
    {
        return OM_ERR_NO_MEM;
    }
    OsalThreadAttr attr = {"log_thread", 0U, OSAL_PRIO_LOW_BASE};
    OsalThread *attr_thread = NULL;
    if (osal_thread_create(&attr_thread, &attr, log_async_thread, NULL) != OSAL_OK)
    {
        return OM_ERR_NO_MEM;
    }
    return OM_OK;
}
OM_INIT_SERVICE(log_async_init); /* SERVICE 级：调度器后，可阻塞/建线程（ADR-0015 异步引入 init） */

#endif /* OM_LOG_MODE_ASYNC */
