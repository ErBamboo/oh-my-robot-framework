/**
 * @file main.c
 * @brief 串口日志后端接线示范（组合层：drivers 工厂 + 板级日志口 + log 服务）
 * @details OM_INIT_DRIVER 注册后端（调度器前，早于 SERVICE 级业务日志——DRIVER 级
 *          之前（EARLIEST/BOARD）的日志 v1 静默丢弃，契约见 services/log/README.md）；
 *          OM_INIT_APPLICATION 打样例日志。烧录后日志口串口输出
 *          "[INF][log_serial] ..." 格式文本。
 */

#include "core/om_init.h" /* OM_INIT_DRIVER / OM_INIT_APPLICATION */
#include "drivers/peripheral/serial/log_serial_backend.h"
#include "osal/osal.h"
#include "services/log/log.h"

#include "bsp_serial.h" /* BSP_LOG_SERIAL_NAME：板级日志口选择 */

static LogSerialBackend g_log_serial_backend;

OM_LOG_MODULE(log_serial, OM_LOG_LEVEL_INFO);

/** @brief 接线：device_find 板级日志口 → 注册后端（DRIVER 级，调度器前）
 *  @return OM_OK 成功；失败传播（open/注册错误码） */
static OmRet log_port_init(void)
{
    return om_log_serial_backend_register(&g_log_serial_backend,
                                          device_find((char *)BSP_LOG_SERIAL_NAME), "serial", OM_LOG_LEVEL_INFO);
}
OM_INIT_DRIVER(log_port_init);

/** @brief 心跳日志线程：持续输出验证（串口观察用）
 *  @param arg 未使用 */
static void log_heartbeat(void *arg)
{
    int i = 0;
    (void)arg;
    for (;;)
    {
        OM_LOG_INFO("log heartbeat %d", i++);
        osal_sleep_ms(500);
    }
}

/** @brief 样例日志（APPLICATION 级：业务就绪后打样 + 建心跳线程）
 *  @return OM_OK */
static OmRet log_demo(void)
{
    OsalThread *thread = NULL;
    OsalThreadAttr attr = {"log_hb", 1024U, OSAL_PRIO_LOW_BASE};
    OM_LOG_INFO("log serial backend demo: %d", 42);
    OM_LOG_ERROR("error path demo");
    (void)osal_thread_create(&thread, &attr, log_heartbeat, NULL);
    return OM_OK;
}
OM_INIT_APPLICATION(log_demo);
