#ifndef __LOG_INTERNAL_H__
#define __LOG_INTERNAL_H__

#include "core/om_def.h"
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

void log_buf_writer_init(LogBufWriter *w, LogOutFn out, void *outCtx, char *seg, size_t segSize);
void log_buf_write(LogBufWriter *w, const char *s, size_t n);
void log_buf_putc(LogBufWriter *w, char c);
void log_buf_flush(LogBufWriter *w);

/** @brief printf 风格子集：标志 -/0、宽度、长度 l；转换符 d i u x X p c s %；
 *          未知转换符降级字面输出；LONG_MIN 取负未处理（文档约束） */
void log_format(LogBufWriter *w, const char *fmt, va_list ap);

/** @brief 是否有后端接受该级别（过滤流水线第②步，临界区内调用） */
bool log_backend_any_accepts(OmLogLevel level);

/** @brief 扇出：对每个接受该级别的后端依次 push（临界区内调用） */
void log_backend_push_all(OmLogLevel level, const char *seg, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __LOG_INTERNAL_H__ */
