/**
 * @file log_serial_backend.c
 * @brief 串口日志后端实现（工厂：open + 组装 + 注册；container_of 多实例）
 * @details push = device_write 非阻塞提交（SERIAL_O_NBLCK_TX 打开语义）；
 *          txFifo 满截断静默（快速提交契约），溢出经设备 err_cb 暴露
 *          （失败感知分层，后端自持诊断，见 services/log/README.md）。
 */

#include "drivers/peripheral/serial/log_serial_backend.h"

#include "drivers/peripheral/serial/pal_serial_dev.h"

#include <stddef.h> /* offsetof */

/* 首成员强转取实例的前提：backend 位于 offset 0（约束锁定在本文件，编译期保障，
 * 重构结构体顺序会立即编译报错） */
_Static_assert(offsetof(LogSerialBackend, backend) == 0,
               "LogSerialBackend.backend 须为首成员（直接强转取实例的前提）");

/** @brief 段推送：非阻塞提交到串口设备（log 临界区内被调；线程/中断上下文均可能）
 *  @param backend 后端实例（首成员强转反查 LogSerialBackend，offset 0 由 _Static_assert 保障）
 *  @param seg 段数据
 *  @param len 段字节数 */
static void log_serial_push(OmLogBackend *backend, const char *seg, size_t len)
{
    LogSerialBackend *inst = (LogSerialBackend *)backend;
    /* device_write：NBLCK 打开 → 非阻塞提交（BUSY_TX 时入 txFifo，满则截断）；
     * ISR 中自动走 serial_tx_nonblock（hal_serial 已处理）——快速提交契约 */
    (void)device_write(inst->dev, NULL, (void *)seg, len);
}

OmRet om_log_serial_backend_register(LogSerialBackend *inst, Device *serial_dev, const char *name, OmLogLevel level)
{
    OmRet ret;
    if (inst == NULL || serial_dev == NULL || name == NULL)
    {
        return OM_ERR_INVALID_ARG;
    }
    ret = device_open(serial_dev, SERIAL_O_NBLCK_TX); /* 非阻塞发送打开（快速提交前提） */
    if (ret != OM_OK)
    {
        return ret;
    }
    inst->dev = serial_dev;
    inst->backend.name = name;
    inst->backend.push = log_serial_push;
    inst->backend.flush = NULL; /* v1 无调用点；串口设备无 flush 语义 */
    return om_log_backend_register(&inst->backend, level);
}
