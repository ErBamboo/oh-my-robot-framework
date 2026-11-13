/**
 * @file main.c
 * @brief 引导器最小启动入口（os=none）
 * @details 强符号 main 覆盖框架默认 main；只执行调度器前的 EARLIEST/BOARD/DRIVER
 *          初始化，不建 init 线程、不启动调度器。后续决策与下载逻辑在此接入。
 */
#include "core/om_init.h"

int main(void)
{
    (void)om_startup_pre_scheduler();
    while (1)
    {
    }
}
