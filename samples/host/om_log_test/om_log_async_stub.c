/**
 * @file om_log_async_stub.c
 * @brief host 端异步入队链接桩（默认配置 OM_LOG_ASYNC 下仅 core.c 引用；本目标不编译 log_async.c）
 * @details host 无 OSAL 队列/线程端口（本地桩只满足 osal_time）→ 桩恒返回"未就绪"
 *          （can_enqueue = false）：om_log_log 全链走同步兜底（v1 语义），与目标设备
 *          调度器前/SERVICE init 前窗口行为同构——断言不因异步引入漂移。
 *          入队桩仅供链接（就绪判定为 false，实际不会被调用）；丢计数桩恒 0
 *          （队列未建无满丢弃，等价"队列项恒 0"）。
 */

#include "log_internal.h"

#include <stdbool.h>

bool log_async_can_enqueue(void)
{
    return false;
}

bool log_async_send(const OmLogMsg *msg)
{
    (void)msg;
    return false;
}

uint32_t log_dropped_queue(void)
{
    return 0U;
}
