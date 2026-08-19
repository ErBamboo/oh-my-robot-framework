/**
 * @file core.c
 * @brief log 核心：om_log_log 入口 + 过滤流水线 + emit（头部/格式化/扇出）+ 临界区
 * @details 过滤：① 编译期模块级别（常量折叠）→ ② 有无后端接受（零格式化）→
 *          ③ 临界区（kernel 层临界区原语，中断上下文嵌套安全）→ ④ emit →
 *          ⑤ 退临界区。打日志无失败路径；未就绪（无后端/级别不达）静默返回。
 *          结构预留：emit 独立（v2 异步 = 日志线程调同一 emit）；头部生成独立
 *          （v2 时间戳字段扩展）；丢弃点独立（v3 deferred 挂接）。
 */

#include "core/om_config.h"

#ifdef OM_USE_LOG

#include "core/om_def.h"
#include "core/om_interrupt.h"

#include "log_internal.h"

#include <stdarg.h>
#include <string.h>

/* 级别名（3 字符；索引 = 级别数值 0..4，过滤已保证不越界） */
static const char *const log_level_name[] = {"DBG", "INF", "WRN", "ERR", "FTL"};

/** @brief 扇出回调：把格式化段推给所有接受该级别的后端（临界区内调用）
 *  @param ctx 编码的日志级别（(uintptr_t)OmLogLevel，由 log_emit 传入）
 *  @param seg 段数据
 *  @param len 段字节数 */
static void emit_fanout(void *ctx, const char *seg, size_t len)
{
    OmLogLevel level = (OmLogLevel)(uintptr_t)ctx;
    log_backend_push_all(level, seg, len);
}

/** @brief 头部生成：输出 "[LVL][module] "（独立函数——结构预留：v2 统一时间戳在此加字段）
 *  @param w 写器
 *  @param module 模块实例（name 已由入口校验非 NULL）
 *  @param level 消息级别
 *  @note log_level_name[level] 下标安全：om_log_log 的 OFF 检查已保证 level ∈ 0..4 */
static void emit_header(LogBufWriter *w, const OmLogModule *module, OmLogLevel level)
{
    log_buf_putc(w, '[');
    log_buf_write(w, log_level_name[level], 3);
    log_buf_putc(w, ']');
    log_buf_putc(w, '[');
    log_buf_write(w, module->name, strlen(module->name));
    log_buf_write(w, "] ", 2);
}

/** @brief emit：后端接受判定 → 头部 + 流式格式化 + 尾部 \r\n + 扇出（临界区内调用）
 *  @param module 模块实例
 *  @param level 消息级别
 *  @param fmt 格式串
 *  @param ap 可变参数
 *  @note 结构预留：v2 异步 = 日志线程调同一函数，只换执行者；栈占用 = 段缓冲
 *        （OM_LOG_SEGMENT_SIZE）+ 写器状态，不随消息长度增长 */
static void log_emit(const OmLogModule *module, OmLogLevel level, const char *fmt, va_list ap)
{
    if (!log_backend_any_accepts(level))
    {
        return; /* ② 无后端接受 → 零格式化开销 */
    }
    LogBufWriter w;
    char seg[OM_LOG_SEGMENT_SIZE];
    log_buf_writer_init(&w, emit_fanout, (void *)(uintptr_t)level, seg, sizeof(seg));
    emit_header(&w, module, level);
    log_format(&w, fmt, ap);
    log_buf_write(&w, "\r\n", 2);
    log_buf_flush(&w);
}

void om_log_log(const OmLogModule *module, OmLogLevel level, const char *fmt, ...)
{
    port_critical_key_t key;
    va_list ap;
    if (module == NULL || module->name == NULL || fmt == NULL)
    {
        return; /* 无失败路径：非法参数静默 */
    }
    if (level >= OM_LOG_LEVEL_OFF)
    {
        return;
    }
    if (level < module->compileLevel)
    {
        return; /* ① 编译期裁剪（compileLevel 为编译期常量 → 折叠零成本） */
    }
    va_start(ap, fmt);
    key = om_hw_disable_interrupt(); /* ③ 临界区（线程/中断上下文均安全，嵌套安全） */
    log_emit(module, level, fmt, ap);
    om_hw_restore_interrupt(key); /* ⑤ */
    va_end(ap);
}

#endif /* OM_USE_LOG */
