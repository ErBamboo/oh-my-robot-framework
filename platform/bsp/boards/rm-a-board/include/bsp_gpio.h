/**
 * @file  bsp_gpio.h
 * @brief rm-a-board GPIO 板配置 shim（板瘦身：类型/宏/契约已上移 gpio/bsp_gpio_f4.h）
 */
#ifndef __BSP_GPIO_H__
#define __BSP_GPIO_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 端口数：必须等于 bsp_gpio_data.c 中 gBspGpio 条目数（数据文件内编译期校验） */
#define BSP_GPIO_PORT_COUNT (10U)

#include "gpio/bsp_gpio_f4.h"

#ifdef __cplusplus
}
#endif

#endif /* __BSP_GPIO_H__ */
