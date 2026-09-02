#ifndef __OM_CONFIG_H__
#define __OM_CONFIG_H__

/* 功能裁剪 */
#define OM_USE_ASSERT /* 框架断言（见 core/om_assert.h）；om_appcfg.h 中 #undef 可关闭 */

#ifndef OM_USE_LOG
#define OM_USE_LOG 1 /* services: log 服务（见 services/log/log.h）；值语义 =0 或 om_appcfg.h #undef（#if 未定义=0）可裁剪 */
#endif

#ifndef OM_LOG_MAX_BACKENDS
#define OM_LOG_MAX_BACKENDS 4 /* 后端注册表上限（定长数组） */
#endif

#ifndef OM_LOG_SEGMENT_SIZE
#define OM_LOG_SEGMENT_SIZE 32 /* formatter 段缓冲（字节，栈占用 = 段 + 常量状态） */
#endif

/* log 服务异步能力（统一异步形态：就绪=打包入队+日志线程格式化；未就绪/裁剪=同步兜底）
 * 默认定义（能力常驻）；最小配置（无 RTOS/裸机）可 #undef——此时调用侧格式化（同步兜底，
 * 即 v1 语义）。详见服务 README 投递形态节。 */
#ifndef OM_LOG_ASYNC
#define OM_LOG_ASYNC 1 /* 值语义（OM_SYNC_ACCEL 同款）：=0 或 appcfg #undef（#if 未定义=0）即裁剪 */
#endif

#ifndef OM_LOG_MAX_ARGS
#define OM_LOG_MAX_ARGS 8 /* 参数包上限（uintptr_t 宽度 ×8 = 64B）；取值 1..16（va 计数表上限）—— \
                            超限编译期报错（log.h ARG_LIMIT 负数组）+ 运行期丢弃计数 */
#endif
#if OM_LOG_MAX_ARGS > 16
#error "OM_LOG_MAX_ARGS 超出 va 计数表上限（1..16）——扩展 OM_LOG_VA_COUNT 表再调大"
#endif

#ifndef OM_LOG_QUEUE_LEN
#define OM_LOG_QUEUE_LEN 8 /* 异步队列深度（消息槽数；满丢弃+计数，printk 语义） */
#endif

/* 抽象层裁剪 */
#define OM_USE_HAL_SERIALS
#define OM_USE_HAL_CAN
#define OM_USE_HAL_SPI
#define OM_USE_HAL_PWM

/*
 * sync 加速开关（统一入口）：
 *
 * - OM_SYNC_ACCEL: 全局加速开关，用于标记是否启用加速后端
 *
 * 说明：
 * - 分原语 capability 由 port 层注入（如 OM_SYNC_ACCEL_CAP_COMPLETION）。
 * - 当前 completion 无独立加速后端，默认回退 reference。
 */
#ifndef OM_SYNC_ACCEL
#define OM_SYNC_ACCEL 0
#endif

/*
 * 应用覆写入口：用户可在工程中放置 om_appcfg.h 以覆盖上述默认值。
 * 该文件不是必须的。启用方式：在包含 om_config.h 之前定义 OM_USE_APPCFG，
 * 或通过构建系统注入 -DOM_USE_APPCFG。
 */
#ifdef OM_USE_APPCFG
#include "om_appcfg.h"
#endif

#endif // __OM_CONFIG_H__
