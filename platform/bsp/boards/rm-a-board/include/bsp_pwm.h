/**
 * @file  bsp_pwm.h
 * @brief rm-a-board PWM 板配置 shim（板瘦身：类型/宏/契约已上移 pwm/bsp_pwm_f4.h）
 * @details 本文件只留板配置；全部类型与板数据契约见共享适配层头。
 *          引脚映射：pwm1(TIM2)=PA0-3、pwm2(TIM8)=PI5/6/7+PI2、pwm3(TIM4)=PD12-15、
 *          pwm4(TIM5)=PH10/11/12+PI0。
 */
#ifndef __BSP_PWM_H__
#define __BSP_PWM_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 实例数：必须等于 bsp_pwm_data.c 中 gBspPwm 条目数（数据文件内编译期校验） */
#define BSP_PWM_COUNT (4U)

#include "pwm/bsp_pwm_f4.h"

#ifdef __cplusplus
}
#endif

#endif /* __BSP_PWM_H__ */
