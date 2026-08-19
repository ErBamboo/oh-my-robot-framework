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

/** @brief 样例日志（APPLICATION 级：业务就绪后打样）
 *  @return OM_OK */
static OmRet log_demo(void)
{
    OM_LOG_INFO("log serial backend demo: %d", 42);
    OM_LOG_ERROR("error path demo");
    return OM_OK;
}
OM_INIT_APPLICATION(log_demo);
