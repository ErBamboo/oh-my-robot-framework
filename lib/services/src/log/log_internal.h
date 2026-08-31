/**
 * @file log_internal.h
 * @brief log 服务私有共享接口（formatter/core/backend 三源文件内部互调，不进公共头）
 * @details 段缓冲写器（LogBufWriter）与流式格式化归 formatter.c；后端接受判定与扇出
 *          归 backend.c；core.c 在临界区内经本接口编排。公共契约见 services/log/log.h。
 */

#ifndef __LOG_INTERNAL_H__
#define __LOG_INTERNAL_H__

#include "core/om_def.h"
#include "core/om_config.h"   /* OM_LOG_MAX_ARGS（OmLogMsg 参数包宽度，log_internal.h 专属） */
#include "services/log/log.h" /* OmLogLevel（级别类型，公共头已含 core/om_def.h） */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 段输出回调（每段一次调用；log 服务实现：扇出到通过过滤的后端） */
typedef void (*LogOutFn)(void *ctx, const char *seg, size_t len);

/** @brief 段缓冲写器：格式化/头部/尾部共用；满段自动 flush 回调，任意长不截断 */
typedef struct
{
    LogOutFn out;
    void *outCtx;
    char *seg;
    size_t segSize;
    size_t segLen;
} LogBufWriter;

/** @brief 初始化段缓冲写器（调用方提供段缓冲；满段自动 flush 回调）
 *  @param w 写器
 *  @param out 段输出回调
 *  @param out_ctx 回调上下文
 *  @param seg 段缓冲（大小即分段粒度）
 *  @param seg_size 段缓冲字节数（须 > 0） */
void log_buf_writer_init(LogBufWriter *w, LogOutFn out, void *out_ctx, char *seg, size_t seg_size);

/** @brief 写入 n 字节（超段自动 flush 回调，任意长不截断）
 *  @param w 写器
 *  @param s 数据源
 *  @param n 字节数 */
void log_buf_write(LogBufWriter *w, const char *s, size_t n);

/** @brief 写入单字符（log_buf_write 特例）
 *  @param w 写器
 *  @param c 字符 */
void log_buf_putc(LogBufWriter *w, char c);

/** @brief 强制刷出残留段（一条日志的尾部调用）
 *  @param w 写器 */
void log_buf_flush(LogBufWriter *w);

/** @brief printf 风格子集：标志 -/0、宽度、长度 l；转换符 d i u x X p c s %
 *  @param w 写器（经其段缓冲与 out 回调输出）
 *  @param fmt 格式串
 *  @param ap 可变参数
 *  @note 未知/不完整转换符降级为整段规格字面输出；LONG_MIN 取负未处理（文档约束） */
void log_format(LogBufWriter *w, const char *fmt, va_list ap);

/** @brief 参数包（异步投递的消息载体——Zephyr log_msg 同款；fmt+args 延后到日志线程格式化）
 *  @note argBuf 为 uintptr_t 宽参数数组（8 个 = 64B）：整型/指针直接存；
 *        %s 只存指针——字符串生命周期由调用方保证（doxygen 文档约束，Zephyr 同款） */
typedef struct
{
    const char *fmt;
    OmLogLevel level;
    const OmLogModule *module;
    uintptr_t argBuf[OM_LOG_MAX_ARGS];
    uint32_t argCount;
} OmLogMsg;

/** @brief 参数数组版格式化（与 log_format(va_list) 并存——日志线程/同步模式均可） */
void log_format_args(LogBufWriter *w, const char *fmt, const uintptr_t *args, size_t n);

/** @brief 打包：按 fmt 解析参数数 → va_list 逐参抓取进 argBuf（超限丢弃）
 *  @return true = 打包成功（<= OM_LOG_MAX_ARGS 参）；false = 超限丢弃（已计数） */
bool log_msg_build(OmLogMsg *msg, const OmLogModule *module, OmLogLevel level, const char *fmt, va_list ap);

/** @brief emit（日志线程/同步共用）：后端接受判定 → 头部 + 流式格式化 + 尾部 \n + 扇出
 *  @param module 模块实例
 *  @param level 消息级别
 *  @param fmt 格式串
 *  @param args 参数数组（参数包）
 *  @param n 参数个数
 *  @note 异步模式 = 日志线程调此函数（v1 结构预留兑现）；同步模式 = 调用侧（va_list 版） */
void log_emit_args(const OmLogModule *module, OmLogLevel level, const char *fmt, const uintptr_t *args, size_t n);

#ifdef OM_LOG_MODE_ASYNC
/** @brief 异步模式初始化：建队列 + 日志线程（经 OM_INIT_SERVICE 调用；仅异步模式存在） */
OmRet log_async_init(void);

/** @brief 异步模式入队（build 成功后调用；队列满 → 丢弃+计数）
 *  @return true = 已入队 */
bool log_async_send(const OmLogMsg *msg);
#endif

/** @brief 是否有后端接受该级别（过滤流水线第②步，临界区内调用）
 *  @param level 消息级别
 *  @return true = 至少一个已注册后端满足 level >= 其后端级别 */
bool log_backend_any_accepts(OmLogLevel level);

/** @brief 扇出：对每个接受该级别的后端依次 push（临界区内调用）
 *  @param level 消息级别（per-backend 过滤依据）
 *  @param seg 段数据
 *  @param len 段字节数 */
void log_backend_push_all(OmLogLevel level, const char *seg, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __LOG_INTERNAL_H__ */
