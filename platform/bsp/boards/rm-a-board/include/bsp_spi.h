/**
 * @file    bsp_spi.h
 * @brief   A 板 SPI BSP 私有头文件
 * @details 定义 BspSpi_s 实例结构、BSP_SPI_STATIC_INIT 宏、实例索引枚举、
 *          DMA stream/channel/NVIC IRQn/IRQHandler 的板级映射宏。
 *
 *          SPI4 引脚（AF5）：
 *            PE12 — SCLK
 *            PE5  — MISO
 *            PE6  — MOSI
 *            PE4  — NSS（GPIO CS，由框架直控）
 *
 *          本头文件不入框架 PR 之外的使用范围（仅 rm-a-board SPI BSP 内部），
 *          框架侧通过 SpiBus.hwPrivate 不透明指针持有本结构。
 *
 * @note    结构布局约束（任务约束 #5）：
 *          - 第 1 字段：SpiBus parent（框架父类）
 *          - 第 2 字段：SPI_HandleTypeDef handle（HAL 句柄）
 *          由于 handle 不在首字段，HAL 回调传入 SPI_HandleTypeDef* 时不能
 *          直接强转为 BspSpi_t，须用 BSP_SPI_FROM_HANDLE(hspi) 辅助宏
 *          （基于 offsetof 反查）。
 */

#ifndef BSP_SPI_H
#define BSP_SPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

#include "drivers/peripheral/spi/pal_spi_dev.h"

/*===========================================================================
 * 实例裁剪与配置宏
 *===========================================================================*/

#define USE_SPI4
#ifdef USE_SPI4
/* SPI4 在 APB2（90MHz），DMA 必须走 DMA2。
 *
 * STM32F427 DMA2 request mapping（RM0090 §9.3.3 Table 43，硬件布线，非 LL_DMA_REQUEST）：
 *   SPI4_TX = DMA2 Stream4 Channel5（F427 唯一可用映射，Table 43 footnote 标 F42x/F43x-only）
 *   SPI4_RX = DMA2 Stream3 Channel5
 *
 * 实测教训（任务 #54）：
 *   (1) Stream5/Ch4 — F427 硬件不路由 SPI4_TX 请求到 Stream5，NDTR 永不递减、ISR 不触发
 *   (2) Stream1/Ch4 — 硬件可路由，但 IRQ 与 serial6_rx 占用的 DMA2_Stream1_IRQHandler
 *       符号冲突（STM32 IRQ 按 stream 共享，不能按 channel 复用），链接器报 L6123E
 *       symbol multiply defined
 *   (3) Stream4/Ch5 — RM0090 Table 43 中 F427 唯一合法的 SPI4_TX 映射，IRQ（DMA2_Stream4_IRQn
 *       =60）空闲，符号无冲突
 *
 * 已知被 Serial 占用的 DMA2 stream（见 bsp_serial.h）：
 *   Stream7/Ch4 = serial1_tx,   Stream2/Ch4 = serial1_rx
 *   Stream6/Ch5 = serial6_tx,   Stream1/Ch5 = serial6_rx
 *   （Stream3/4 不被任何 serial 占用，可放心给 SPI4 用）
 */
#define SPI4_DMA_TX_DMA_STREAM   (DMA2_Stream4)
#define SPI4_DMA_TX_DMA_CHANNEL  (DMA_CHANNEL_5)
#define SPI4_DMA_TX_IRQn         (DMA2_Stream4_IRQn)
#define SPI4_DMA_TX_IRQ_Handler  (DMA2_Stream4_IRQHandler)

#define SPI4_DMA_RX_DMA_STREAM   (DMA2_Stream3)
#define SPI4_DMA_RX_DMA_CHANNEL  (DMA_CHANNEL_5)
#define SPI4_DMA_RX_IRQn         (DMA2_Stream3_IRQn)
#define SPI4_DMA_RX_IRQ_Handler  (DMA2_Stream3_IRQHandler)

/* SPI4 输入时钟（APB2 = HCLK/2 = 180/2 = 90MHz），用于 maxHz 分频计算 */
#define SPI4_INPUT_CLOCK_HZ      (90000000UL)
#endif /* USE_SPI4 */

/*===========================================================================
 * 实例索引枚举
 *===========================================================================*/

typedef enum
{
#ifdef USE_SPI4
    BSP_SPI4_IDX,
#endif
    BSP_SPI_COUNT
} BspSpiIdx_e;

/*===========================================================================
 * BspSpi_s —— BSP 实例结构（含 SpiBus 父类 + HAL 句柄）
 *===========================================================================*/

typedef struct BspSpi *BspSpi_t;

/**
 * @brief A 板 SPI BSP 实例
 * @note  第 1 字段为 SpiBus parent，以便 SpiBus* 与 BspSpi_t 在向上注册时可
 *        通过首字段强转互相转换。handle 放第 2 字段，HAL 回调中反查须用
 *        BSP_SPI_FROM_HANDLE(hspi)。
 */
typedef struct BspSpi
{
    SpiBus             parent;       /* 第 1 字段：框架 SpiBus */
    SPI_HandleTypeDef  handle;       /* 第 2 字段：HAL 句柄（含 hdmatx/hdmarx 链接） */

    /* DMA dummy 缓冲：tx/rx==NULL 时 DMA 源/目标。uint16_t 兼容 8/16-bit
     * 两种数据宽度（16-bit 模式下 DMA HALFWORD 访问需 2 字节对齐）。 */
    uint16_t           dummyTx;      /* tx==NULL 时 DMA 源，固定 0xFFFF */
    uint16_t           dummyRx;      /* rx==NULL 时 DMA 目标，丢弃 */
    size_t             pendingLen;   /* 当前传输总长，abort 反算用 */
} BspSpi_s;

/**
 * @brief 由 SPI_HandleTypeDef* 反查 BspSpi_t（HAL 回调用）
 * @note  handle 在 BspSpi_s 第 2 字段，需基于偏移反算。
 */
#define BSP_SPI_FROM_HANDLE(hspi) \
    ((BspSpi_t)((char *)(hspi) - offsetof(BspSpi_s, handle)))

/**
 * @brief 静态初始化宏（用于全局 gBspSpi[] 数组）
 * @note  dummyTx = 0xFFFF（SPI 约定 dummy），dummyRx = 0（丢弃）。
 *        uint16_t 兼容 8/16-bit DMA 宽度，MINC=0 循环读，len 无上限。
 *        固定 Init 字段（Mode/Direction/NSS/TIMode/CRCCalculation/CRCPolynomial）
 *        在此静态赋初值，configure 中不再重复设置：
 *        - Mode           = MASTER
 *        - Direction      = 2LINES（全双工）
 *        - NSS            = SOFT（GPIO CS 由框架直控）
 *        - TIMode         = DISABLE
 *        - CRCCalculation = DISABLE
 *        - CRCPolynomial  = 7（HAL 默认值，CRC 禁用时无电气意义）
 */
#define BSP_SPI_STATIC_INIT(INSTANCE)                            \
    (BspSpi_s)                                                     \
    {                                                              \
        .handle.Instance = (INSTANCE),                             \
        .handle.Init     = {                                       \
            .Mode           = SPI_MODE_MASTER,                     \
            .Direction      = SPI_DIRECTION_2LINES,                \
            .NSS            = SPI_NSS_SOFT,                        \
            .TIMode         = SPI_TIMODE_DISABLE,                  \
            .CRCCalculation = SPI_CRCCALCULATION_DISABLE,          \
            .CRCPolynomial  = 7U,                                  \
        },                                                         \
        .dummyTx         = 0xFFFFU,                                \
        .dummyRx         = 0x0000U,                                \
        .pendingLen      = 0U,                                     \
    }

/*===========================================================================
 * 全局实例与注册入口
 *===========================================================================*/

extern BspSpi_s gBspSpi[];

/**
 * @brief 注册 A 板 SPI 总线到框架
 * @details 遍历 gBspSpi[]，对每个实例：
 *          (1) bsp_spi_pre_init：GPIO/DMA/NVIC 配置
 *          (2) spi_bus_register：注册 SpiBus 到框架
 *          由 om_port_hw.c 在板级初始化阶段调用。
 */
void bsp_spi_register(void);

/**
 * @brief 单实例硬件预初始化（GPIO/AF/DMA link/NVIC），bsp_spi_register 内部调用
 */
void bsp_spi_pre_init(BspSpi_t bsp_spi);

#ifdef __cplusplus
}
#endif

#endif /* BSP_SPI_H */
