/*
 * rm-a-board IRQ port implementation (Cortex-M4, GNU toolchain)
 */
#include "core/port/om_port_hw.h"
#include "stm32f4xx.h"

port_critical_key_t port_critical_enter(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return (port_critical_key_t)primask;
}

void port_critical_exit(port_critical_key_t key)
{
    __set_PRIMASK((uint32_t)key);
}

void port_int_disable(void)
{
    __disable_irq();
}

void port_int_enable(void)
{
    __enable_irq();
}
