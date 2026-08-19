/**
 * @file log_serial_backend.h
 * @brief 串口日志后端工厂（drivers 层）
 * @details 依赖 services/log/log.h——ADR-0016 (drivers_services_one_way_dependency)
 *          开放的首个 drivers→services 跨层依赖（按服务逐个开放，当前仅 log）。
 *          实例由调用者静态分配（Workqueue 先例），container_of 模式取实例
 *          （backend 为首成员，多实例支持）。
 *          接线（组合层，3 行）：
 *            static LogSerialBackend g_log_serial;
 *            OM_INIT_DRIVER(log_port_init);  // device_find(BSP_LOG_SERIAL_NAME) → register
 */

#ifndef __OM_LOG_SERIAL_BACKEND_H__
#define __OM_LOG_SERIAL_BACKEND_H__

#include "drivers/model/device.h"
#include "services/log/log.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 串口日志后端实例（调用者静态分配；backend 为首成员——container_of 取实例）
 *  @note 多实例支持：每实例一个变量，各自持有 Device；实例生命周期由调用者管理 */
typedef struct {
    OmLogBackend backend; /* 首成员（log.h 契约：内嵌须为首成员） */
    Device *dev;          /* 串口设备（register 内部 open 后持有） */
} LogSerialBackend;

/** @brief 注册串口日志后端：内部 open(SERIAL_O_NBLCK_TX) 并注册到 log 服务
 *  @param inst 实例（调用者静态分配，零初始化即可）
 *  @param serial_dev 串口设备（device_find 获得；NULL → OM_ERR_INVALID_ARG）
 *  @param name 后端名（log 服务按名查找用）
 *  @param level 初始级别（过滤语义同 om_log_backend_register）
 *  @return OM_OK 成功；OM_ERR_INVALID_ARG 参数非法；open 失败或 log 注册失败传播其错误码
 *  @note 快速提交契约：push = device_write 非阻塞提交（NBLCK 打开语义；txFifo 满截断
 *        静默，溢出经设备 err_cb 暴露——失败感知分层，后端自持诊断）；线程上下文调用
 *        （内部 device_open 禁 ISR） */
OmRet om_log_serial_backend_register(LogSerialBackend *inst, Device *serial_dev, const char *name, OmLogLevel level);

#ifdef __cplusplus
}
#endif

#endif /* __OM_LOG_SERIAL_BACKEND_H__ */
