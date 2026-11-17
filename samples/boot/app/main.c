/**
 * @file main.c
 * @brief Boot/OTA 槽应用测试样例（app_a / app_b 共用）
 *
 * 用途：
 * - 验证 app 从各自槽基址启动、VTOR 与链接基址一致；
 * - 通过 USART6 日志输出槽身份、链接基址、VTOR、布局指纹与构建时间；
 * - 创建低优先级心跳线程，证明调度器与业务线程已运行。
 *
 * 同一份源码由构建变体 OM_BOOT_VARIANT_APP_A / APP_B 选择槽身份；
 * 日志口、波特率与槽宏来自板级配置和生成的 om_bootcfg.h。
 */

#include <stdint.h>

#include "core/om_def.h"
#include "core/om_init.h"
#include "drivers/model/device.h"
#include "drivers/peripheral/serial/log_serial_backend.h"
#include "osal/osal.h"
#include "services/log/log.h"
#include "stm32f4xx.h"

#include "bsp_serial.h"
#include "om_bootcfg.h"

#if !defined(OM_BOOT_VARIANT_APP_A) && !defined(OM_BOOT_VARIANT_APP_B)
#error "boot app sample requires OM_BOOT_VARIANT_APP_A or OM_BOOT_VARIANT_APP_B"
#endif

#if defined(OM_BOOT_VARIANT_APP_A)
#define APP_SLOT_LETTER "A"
#define APP_LINK_BASE (OM_BOOT_SLOT_A_LINK_BASE)
#define APP_EXPECTED_VTOR (OM_BOOT_FLASH_BASE + OM_BOOT_SLOT_A_VTOR_OFFSET)
#else
#define APP_SLOT_LETTER "B"
#define APP_LINK_BASE (OM_BOOT_SLOT_B_LINK_BASE)
#define APP_EXPECTED_VTOR (OM_BOOT_FLASH_BASE + OM_BOOT_SLOT_B_VTOR_OFFSET)
#endif

OM_LOG_MODULE(boot_app, OM_LOG_LEVEL_INFO);

#if OM_USE_LOG
static LogSerialBackend g_log_serial;

/** @brief 日志口接线（DRIVER 级，调度器前） */
static OmRet log_port_init(void)
{
    return om_log_serial_backend_register(&g_log_serial,
                                          device_find((char *)BSP_LOG_SERIAL_NAME),
                                          "serial", OM_LOG_LEVEL_INFO);
}
OM_INIT_DRIVER(log_port_init);
#endif /* OM_USE_LOG */

/** @brief 心跳线程：确认 init 完成后的业务线程可运行 */
static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t count = 0;
    for (;;)
    {
        OM_LOG_INFO("APP_%s heartbeat %u", APP_SLOT_LETTER, (unsigned)count);
        count++;
        (void)osal_sleep_ms(1000);
    }
}

/** @brief 应用启动设置（APPLICATION 级，init 线程上下文） */
static OmRet app_setup(void)
{
    uint32_t vtor = SCB->VTOR;

    OM_LOG_INFO("APP_%s start link=0x%08x vtor=0x%08x expected=0x%08x "
                "layout=0x%04x build=%s %s",
                APP_SLOT_LETTER, (unsigned)APP_LINK_BASE, (unsigned)vtor,
                (unsigned)APP_EXPECTED_VTOR, (unsigned)OM_BOOT_LAYOUT_FINGERPRINT,
                __DATE__, __TIME__);
    if (vtor != (uint32_t)APP_EXPECTED_VTOR)
    {
        OM_LOG_ERROR("APP_%s VTOR mismatch: expected 0x%08x",
                     APP_SLOT_LETTER, (unsigned)APP_EXPECTED_VTOR);
    }

    OsalThreadAttr attr = {
        .name = "boot_hb",
        .priority = OSAL_PRIO_LOW_BASE,
        .stackSize = 512u * OSAL_STACK_WORD_BYTES,
    };
    OsalThread *thread = NULL;
    OsalStatus status = osal_thread_create(&thread, &attr, heartbeat_task, NULL);
    if (status != OSAL_OK)
    {
        OM_LOG_ERROR("APP_%s heartbeat thread failed: %d",
                     APP_SLOT_LETTER, (int)status);
        return OM_ERR_NO_MEM;
    }
    return OM_OK;
}
OM_INIT_APPLICATION(app_setup);
