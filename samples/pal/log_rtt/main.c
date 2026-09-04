/**
 * @file main.c
 * @brief RTT 日志后端接线示范（组合层：services 后端工厂 + log 服务）
 * @details OM_INIT_DRIVER 注册后端（调度器前，早于 SERVICE 级业务日志——EARLIEST/BOARD
 *          级日志静默丢弃，契约见 services/log/README.md）；
 *          OM_INIT_APPLICATION 建心跳线程持续输出（带序号，供接收侧完整性分析）。
 *          观测：J-Link RTT Viewer / RTTClient 经调试接口读目标 RAM——零引脚、零串口。
 *
 *          压测档位（编译期宏，配合接收端分析脚本——RTT 无波特率，"速率"=接口时钟）：
 *            LOG_STRESS_LEN        1=短~40B  2=中~200B  3=长~1KB（默认 1）
 *            RTT_STRESS_PERIOD_MS  消息间隔 ms（默认 500）
 *          构建例：xmake f --cxflags="-DLOG_STRESS_LEN=2 -DRTT_STRESS_PERIOD_MS=5"
 */

#include "core/om_init.h" /* OM_INIT_DRIVER / OM_INIT_APPLICATION */
#include "osal/osal.h"
#include "services/log/log.h"
#include "services/log/rtt_backend.h"

static OmRttBackend g_rtt_backend;

OM_LOG_MODULE(log_rtt, OM_LOG_LEVEL_INFO);

/** @brief 接线：注册 RTT 后端（DRIVER 级，调度器前；固定上向通道 0）
 *  @return OM_OK 成功；失败传播（注册错误码） */
static OmRet log_port_init(void)
{
    return om_rtt_backend_register(&g_rtt_backend, "rtt", OM_LOG_LEVEL_INFO);
}
OM_INIT_DRIVER(log_port_init);

/* 压测档位（编译期宏；默认短消息 + 500ms 心跳） */
#ifndef LOG_STRESS_LEN
#define LOG_STRESS_LEN 1
#endif
#ifndef RTT_STRESS_PERIOD_MS
#define RTT_STRESS_PERIOD_MS 500
#endif

/* 长消息负载（~1KB 数据字段，%s 一次性提交） */
static const char g_long_payload[] =
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"
    "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";

/** @brief 心跳日志线程：带序号持续输出（压测档位由编译期宏选择）
 *  @param arg 未使用 */
static void log_heartbeat(void *arg)
{
    int i = 0;
    (void)arg;
    for (;;)
    {
#if (LOG_STRESS_LEN == 1)
        OM_LOG_INFO("hb %d", i++);
#elif (LOG_STRESS_LEN == 2)
        OM_LOG_INFO("hb %d payload=0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF", i++);
#else
        OM_LOG_INFO("hb %d payload=%s", i++, g_long_payload);
#endif
        osal_sleep_ms(RTT_STRESS_PERIOD_MS);
    }
}

/** @brief 样例日志（APPLICATION 级：业务就绪后打样 + 建心跳线程）
 *  @return OM_OK */
static OmRet log_demo(void)
{
    OsalThread *thread = NULL;
    OsalThreadAttr attr = {"log_hb", 1024U, OSAL_PRIO_LOW_BASE};
    OM_LOG_INFO("log rtt backend demo: %d", 42);
    OM_LOG_ERROR("error path demo");
    (void)osal_thread_create(&thread, &attr, log_heartbeat, NULL);
    return OM_OK;
}
OM_INIT_APPLICATION(log_demo);
