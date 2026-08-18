/* host 端口桩：临界区 no-op（host 单线程，无需真实关中断语义） */
#include "core/port/om_port_hw.h"

port_critical_key_t port_critical_enter(void)
{
    return 0;
}

void port_critical_exit(port_critical_key_t key)
{
    (void)key;
}
