/**
 * @file   host_gpio_stub.c
 * @brief  GPIO 桩（spi_nor_test 用）
 *
 * SPI 的片选双路径把 gpio_pin_write 内联在公共头里，故任何 pal_spi_dev.h 的
 * 消费者在链接期都需要这两个符号。本测试的片选走控制器路径
 * （csSpec.controller == NULL ⇒ 框架回落到 ops->setCs），GPIO 分支不会被执行。
 *
 * 因此这里不做"假装成功"的实现：一旦被调用，说明夹具假设已失效、片选实际
 * 没走到仿真里——静默返回会让这种失效变得不可见，故直接终止。
 */

#include <stdlib.h>

#include "drivers/peripheral/gpio/pal_gpio_dev.h"

OmRet gpio_pin_get(const GpioPinSpec *spec, GpioPin *pin)
{
    (void)spec;
    (void)pin;
    return OM_ERR_NOT_SUPPORTED;
}

void gpio_pin_write(GpioPin pin, uint8_t value)
{
    (void)pin;
    (void)value;
    abort(); /* host 测试无 GPIO 控制器：走到这里即夹具失效 */
}
