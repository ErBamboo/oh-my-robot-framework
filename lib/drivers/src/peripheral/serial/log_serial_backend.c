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

/** @brief 段推送：\n→\r\n 表示层映射 + 非阻塞提交到串口设备（log 临界区内被调）
 *  @param backend 后端实例（首成员强转反查 LogSerialBackend，offset 0 由 _Static_assert 保障）
 *  @param seg 段数据
 *  @param len 段字节数
 *  @note 映射语义（README 契约第 9 条）：框架统一 \n，串口后端映射为 \r\n（终端规范行结束）；
 *        已带 \r 的 \r\n 不重复插入（避免 \r\r\n）；行边界语义不变（表示层映射）。
 *        device_write：NBLCK 打开 → 非阻塞提交（BUSY_TX 时入 txFifo，满则截断）；
 *        ISR 中自动走 serial_tx_nonblock（hal_serial 已处理）——快速提交契约 */
static void log_serial_push(OmLogBackend *backend, const char *seg, size_t len)
{
    LogSerialBackend *inst = (LogSerialBackend *)backend;
    static const char crlf[2] = {'\r', '\n'};
    static const char lf[1] = {'\n'};
    size_t start = 0;
    size_t i;
    for (i = 0; i < len; i++)
    {
        if (seg[i] == '\n')
        {
            if (i > start)
            {
                (void)device_write(inst->dev, NULL, (void *)(seg + start), i - start);
            }
            if (i == 0 || seg[i - 1] != '\r')
            {
                (void)device_write(inst->dev, NULL, (void *)crlf, sizeof(crlf)); /* \n → \r\n */
            }
            else
            {
                (void)device_write(inst->dev, NULL, (void *)lf, sizeof(lf)); /* 已是 \r\n，只写 \n */
            }
            start = i + 1;
        }
    }
    if (start < len)
    {
        (void)device_write(inst->dev, NULL, (void *)(seg + start), len - start);
    }
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

OmRet om_log_serial_backend_unregister(LogSerialBackend *inst)
{
    OmRet ret;
    if (inst == NULL || inst->dev == NULL)
    {
        return OM_ERR_INVALID_ARG;
    }
    ret = om_log_backend_unregister(&inst->backend);
    if (ret != OM_OK)
    {
        return ret; /* 未注册：NOT_FOUND，设备保持打开（无副作用） */
    }
    (void)device_close(inst->dev); /* 注销后端后设备不再被日志引用，安全关闭 */
    inst->dev = NULL;
    return OM_OK;
}
