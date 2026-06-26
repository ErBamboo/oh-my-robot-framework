/*
 * LP_MSPM0G3507 — board-level CPU registration (Cortex-M0+, TI DriverLib)
 *
 * TODO: 替换为基于 TIMG12 的实际时间测量实现
 */

#include "core/om_cpu.h"
#include "core/om_interrupt.h"

/*---------------------------------------------------------------------------
 * 全局 CPU 和板级接口实例
 *---------------------------------------------------------------------------*/
static OmCpu g_om_cpu;
static OmBoardInterface g_om_board_interface;

/*---------------------------------------------------------------------------
 * 板级接口函数（当前为 stub，待外设初始化后实现）
 *---------------------------------------------------------------------------*/

static void mspm0g3507_errhandler(void)
{
    om_hw_disable_interrupt_force();
    while (1) {}
}

static void mspm0g3507_reset(void)
{
    /* SCB AIRCR: system reset */
    *(volatile uint32_t *)0xE000ED0CU = 0x05FA0004U;
}

static Cputime mspm0g3507_get_cpu_time_s(void)
{
    /* TODO: 基于 TIMG12 自由运行计数器实现 */
    return 0.0f;
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
    /* TODO: 基于 HW timer 或 SysTick 实现阻塞延时 */
    (void)ms;
}

/*---------------------------------------------------------------------------
 * CPU 注册
 *---------------------------------------------------------------------------*/

void om_cpu_register(uint32_t cpu_freq_m_hz, OmBoardInterface *interface)
{
    g_om_cpu.cpuFreqMHz = cpu_freq_m_hz;
    g_om_cpu.cpuFreqHz  = cpu_freq_m_hz * 1000000U;
    g_om_cpu.interface   = interface;
    g_om_cpu.cpuName     = "MSPM0G3507";
}

void om_core_init(void)
{
    g_om_board_interface.errhandler     = mspm0g3507_errhandler;
    g_om_board_interface.reset          = mspm0g3507_reset;
    g_om_board_interface.getCpuTimeS    = mspm0g3507_get_cpu_time_s;
    g_om_board_interface.getCpuTimeMs   = mspm0g3507_get_cpu_time_ms;
    g_om_board_interface.getCpuTimeUs   = mspm0g3507_get_cpu_time_us;
    g_om_board_interface.getDeltaCpuTimeS = mspm0g3507_get_delta_cpu_time_s;
    g_om_board_interface.delayMs        = mspm0g3507_delay_ms;

    om_cpu_register(32U, &g_om_board_interface);  /* 32 MHz */
}

void om_board_init(void)
{
    om_core_init();
}
