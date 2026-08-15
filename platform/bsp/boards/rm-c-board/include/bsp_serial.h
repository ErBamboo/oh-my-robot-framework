/**
 * @file bsp_serial.h
 * @brief rm-c-board 串口板配置 shim（板瘦身：类型/契约已上移 serial/bsp_serial_f4.h）
 * @details 本文件只留板配置（实例裁剪 + DMA 参数）；全部类型见共享适配层头。
 */

#ifndef __OM_BSP_SERIAL_H__
#define __OM_BSP_SERIAL_H__

#include "drivers/peripheral/serial/pal_serial_dev.h"

#include "stm32f4xx_hal.h"

#define USE_SERIAL_1
#ifdef USE_SERIAL_1
#define SERIAL_1_REG_PARAMS (SERIAL_REG_DMA_RX | SERIAL_REG_DMA_TX) /* 注册参数 */
#define USE_SERIAL1_DMA_TX                                          /* 使用DMA发送 */
#define USE_SERIAL1_DMA_RX                                          /* 使用DMA接收 */
#ifdef USE_SERIAL1_DMA_TX
#define SERIAL_1_DMA_TX_DMA_STREAM DMA2_Stream7
#define SERIAL_1_DMA_TX_DMA_CHANNEL DMA_CHANNEL_4
#define SERIAL_1_DMA_TX_IRQn DMA2_Stream7_IRQn
#define SERIAL_1_DMA_TX_IRQ_Handler DMA2_Stream7_IRQHandler
#endif
#ifdef USE_SERIAL1_DMA_RX
#define SERIAL_1_RX_MULTIBUF_SIZE (256U) /* 多缓冲区接收长度 */
#define USE_SERIAL1_CONTAINER1           /* 使用多缓冲区接收 */
#define SERIAL_1_DMA_RX_DMA_STREAM DMA2_Stream2
#define SERIAL_1_DMA_RX_DMA_CHANNEL DMA_CHANNEL_4
#define SERIAL_1_DMA_RX_IRQn DMA2_Stream2_IRQn
#define SERIAL_1_DMA_RX_IRQ_Handler DMA2_Stream2_IRQHandler
#endif
#endif

// #define USE_SERIAL_3
#ifdef USE_SERIAL_3
#define SERIAL_3_REG_PARAMS (SERIAL_REG_DMA_RX | SERIAL_REG_DMA_TX) /* 注册参数 */
#define USE_SERIAL3_DMA_TX                                          /* 使用DMA发送 */
#define USE_SERIAL3_DMA_RX                                          /* 使用DMA接收 */
#ifdef USE_SERIAL3_DMA_TX
#define SERIAL_3_DMA_TX_DMA_STREAM DMA1_Stream3
#define SERIAL_3_DMA_TX_DMA_CHANNEL DMA_CHANNEL_4
#define SERIAL_3_DMA_TX_IRQn DMA1_Stream3_IRQn
#define SERIAL_3_DMA_TX_IRQ_Handler DMA1_Stream3_IRQHandler
#endif
#ifdef USE_SERIAL3_DMA_RX
#define SERIAL_3_RX_MULTIBUF_SIZE (256U) /* 多缓冲区接收长度 */
#define USE_SERIAL3_CONTAINER1           /* 使用多缓冲区接收 */
#define SERIAL_3_DMA_RX_DMA_STREAM DMA1_Stream1
#define SERIAL_3_DMA_RX_DMA_CHANNEL DMA_CHANNEL_4
#define SERIAL_3_DMA_RX_IRQn DMA1_Stream1_IRQn
#define SERIAL_3_DMA_RX_IRQ_Handler DMA1_Stream1_IRQHandler
#endif
#endif

/* 串口配置宏 */
#define USE_SERIAL_6
#ifdef USE_SERIAL_6
#define SERIAL_6_REG_PARAMS (SERIAL_REG_DMA_RX | SERIAL_REG_DMA_TX) /* 注册参数 */
#define USE_SERIAL6_DMA_TX                                          /* 使用DMA发送 */
#define USE_SERIAL6_DMA_RX                                          /* 使用DMA接收 */
#ifdef USE_SERIAL6_DMA_TX
#define SERIAL_6_DMA_TX_DMA_STREAM DMA2_Stream6
#define SERIAL_6_DMA_TX_DMA_CHANNEL DMA_CHANNEL_5
#define SERIAL_6_DMA_TX_IRQn DMA2_Stream6_IRQn
#define SERIAL_6_DMA_TX_IRQ_Handler DMA2_Stream6_IRQHandler
#endif
#ifdef USE_SERIAL6_DMA_RX
#define SERIAL_6_RX_MULTIBUF_SIZE (256U) /* 多缓冲区接收长度 */
#define USE_SERIAL6_CONTAINER1           /* 使用多缓冲区接收 */
#define SERIAL_6_DMA_RX_DMA_STREAM DMA2_Stream1
#define SERIAL_6_DMA_RX_DMA_CHANNEL DMA_CHANNEL_5
#define SERIAL_6_DMA_RX_IRQn DMA2_Stream1_IRQn
#define SERIAL_6_DMA_RX_IRQ_Handler DMA2_Stream1_IRQHandler
#endif
#endif

/* 实例数：必须等于 bsp_serial_data.c 中 g_bsp_serial 条目数（数据文件内编译期校验） */
#define BSP_SERIAL_COUNT (2U)

#include "serial/bsp_serial_f4.h"

#endif
