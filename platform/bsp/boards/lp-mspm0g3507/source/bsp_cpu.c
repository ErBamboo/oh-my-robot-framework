/*
 * LP_MSPM0G3507 — board-level CPU callbacks (Cortex-M0+, TI DriverLib)
 *
 * 本文件仅提供板级回调函数和 om_board_init()。
 * om_cpu_register() / om_core_init() 由框架 lib/source/core/om_cpu.c 提供。
 *
 * TODO: 替换为基于 TIMG12 的实际时间测量实现
 */

#include "bsp_gpio.h"
#include "bsp_serial.h"
#include "bsp_spi.h"
#include "core/om_cpu.h"
#include "core/om_interrupt.h"

static void mspm0g3507_errhandler(void)
{
    om_hw_disable_interrupt_force();
    while (1) {}
}

static void mspm0g3507_reset(void)
{
    *(volatile uint32_t *)0xE000ED0CU = 0x05FA0004U;
}

static Cputime mspm0g3507_get_cpu_time_s(void)
{
    return 0.0f;  /* TODO: TIMG12 */
}

static CputimeMs mspm0g3507_get_cpu_time_ms(void)
{
    return 0.0f;
}

static CputimeUs mspm0g3507_get_cpu_time_us(void)
{
    return 0ULL;
}

static Cputime mspm0g3507_get_delta_cpu_time_s(CputimeCnt *last_time_cnt)
{
    (void)last_time_cnt;
    return 0.0f;
}

static void mspm0g3507_delay_ms(float ms)
{
    (void)ms;  /* TODO: HW timer */
}

void om_board_init(void)
{
    static OmBoardInterface iface = {
        .errhandler      = mspm0g3507_errhandler,
        .reset           = mspm0g3507_reset,
        .getCpuTimeS     = mspm0g3507_get_cpu_time_s,
        .getCpuTimeMs    = mspm0g3507_get_cpu_time_ms,
        .getCpuTimeUs    = mspm0g3507_get_cpu_time_us,
        .getDeltaCpuTimeS = mspm0g3507_get_delta_cpu_time_s,
        .delayMs         = mspm0g3507_delay_ms,
    };
    om_cpu_register(32U, &iface);

    /* 外设注册 */
    bsp_gpio_register();
    bsp_serial_register();
    bsp_spi_register();
}
