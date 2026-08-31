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
#include "osal/osal_time.h" /* 时间戳来源 osal_time_now_monotonic（ADR-0015；host 测试经本地桩头） */

#include "log_internal.h"

#include <stdarg.h>
#include <string.h>

/* 级别名表（3 字符）：尺寸绑定 OM_LOG_LEVEL_OFF——级别枚举扩展必须同步本表，
 * 否则下方编译期断言拦截（运行时无需再校验：emit 入口已保证 level < OFF） */
static const char *const log_level_name[OM_LOG_LEVEL_OFF] = {
    [OM_LOG_LEVEL_DEBUG] = "DBG",
    [OM_LOG_LEVEL_INFO] = "INF",
    [OM_LOG_LEVEL_WARN] = "WRN",
    [OM_LOG_LEVEL_ERROR] = "ERR",
    [OM_LOG_LEVEL_FATAL] = "FTL",
};

_Static_assert(OM_LOG_LEVEL_FATAL + 1 == OM_LOG_LEVEL_OFF,
               "log 级别枚举扩展需同步 log_level_name 级别名表");

/** @brief 扇出回调：把格式化段推给所有接受该级别的后端（临界区内调用）
 *  @param ctx 编码的日志级别（(uintptr_t)OmLogLevel，由 log_emit 传入）
 *  @param seg 段数据
 *  @param len 段字节数 */
static void emit_fanout(void *ctx, const char *seg, size_t len)
{
    OmLogLevel level = (OmLogLevel)(uintptr_t)ctx;
    log_backend_push_all(level, seg, len);
}

/** @brief 头部生成：输出 "[LVL][HH:MM:SS.mmm][module] "（v2 时间戳字段在此统一加——同步/异步共用）
 *  @param w 写器
 *  @param module 模块实例（name 已由入口校验非 NULL）
 *  @param level 消息级别
 *  @note 时间戳 = osal_time_now_monotonic（32 位单调 ms，线程/ISR 双安全，ADR-0015）；
 *        格式换算 log_time_format（纯函数，逐字符填数——不经过 log_format，避免依赖格式化器）
 *  @note log_level_name[level] 下标安全：入口 OFF 检查保证 level < OM_LOG_LEVEL_OFF，
 *        表尺寸 = OM_LOG_LEVEL_OFF（编译期断言保证枚举与表同步） */
static void emit_header(LogBufWriter *w, const OmLogModule *module, OmLogLevel level)
{
    char ts[13];
    size_t n = log_time_format(ts, osal_time_now_monotonic()); /* 恒 12 */
    log_buf_putc(w, '[');
    log_buf_write(w, log_level_name[level], 3);
    log_buf_putc(w, ']');
    log_buf_putc(w, '[');
    log_buf_write(w, ts, n);
    log_buf_putc(w, ']');
    log_buf_putc(w, '[');
    log_buf_write(w, module->name, strlen(module->name));
    log_buf_write(w, "] ", 2);
}

/** @brief emit：后端接受判定 → 头部 + 流式格式化 + 尾部 \n + 扇出（临界区内调用）
 *  @param module 模块实例
 *  @param level 消息级别
 *  @param fmt 格式串
 *  @param ap 可变参数
 *  @note 结构预留：v2 异步 = 日志线程调同一函数，只换执行者；栈占用 = 段缓冲
 *        （OM_LOG_SEGMENT_SIZE）+ 写器状态，不随消息长度增长
 *  @note 仅同步模式编译（ASYNC 由 log_async_emit → log_emit_args 承接） */
#ifndef OM_LOG_MODE_ASYNC
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
    log_buf_write(&w, "\n", 1); /* 统一结束符：框架侧一处，广播一致（后端可映射，见 README） */
    log_buf_flush(&w);
}
#endif /* OM_LOG_MODE_ASYNC */

/** @brief emit（日志线程/同步共用）：后端接受判定 → 头部 + 流式格式化 + 尾部 \n + 扇出
 *  @param module 模块实例
 *  @param level 消息级别
 *  @param fmt 格式串
 *  @param args 参数数组（参数包）
 *  @param n 参数个数
 *  @note 异步模式 = 日志线程调此函数（v1 结构预留兑现）；同步模式 = 调用侧（va_list 版） */
void log_emit_args(const OmLogModule *module, OmLogLevel level, const char *fmt, const uintptr_t *args, size_t n)
{
    if (!log_backend_any_accepts(level))
    {
        return; /* ② 无后端接受 → 零格式化开销 */
    }
    LogBufWriter w;
    char seg[OM_LOG_SEGMENT_SIZE];
    log_buf_writer_init(&w, emit_fanout, (void *)(uintptr_t)level, seg, sizeof(seg));
    emit_header(&w, module, level);
    log_format_args(&w, fmt, args, n);
    log_buf_write(&w, "\n", 1); /* 统一结束符：框架侧一处，广播一致（后端可映射，见 README） */
    log_buf_flush(&w);
}

void om_log_log(const OmLogModule *module, OmLogLevel level, const char *fmt, ...)
{
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
#ifdef OM_LOG_MODE_ASYNC
    {
        OmLogMsg msg;
        if (log_msg_build(&msg, module, level, fmt, ap))
        {
            (void)log_async_send(&msg); /* 队列满内部计数丢弃 */
        }
    }
    va_end(ap);
    return; /* 异步：调用侧到此（~1-2µs，无临界区——打包本地 + 非阻塞入队） */
#else
    {
        port_critical_key_t key;
        key = om_hw_disable_interrupt(); /* ③ 临界区（线程/中断上下文均安全，嵌套安全） */
        log_emit(module, level, fmt, ap);
        om_hw_restore_interrupt(key); /* ⑤ */
    }
    va_end(ap);
#endif
}

#endif /* OM_USE_LOG */
