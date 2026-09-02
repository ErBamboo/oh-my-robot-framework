/**
 * @file core.c
 * @brief log 核心：om_log_log 入口 + om_log_panic 直出 + 过滤流水线 + emit（头部/格式化/扇出）+ 临界区
 * @details 过滤：① 编译期模块级别（常量折叠）→ ② 有无后端接受（零格式化）→
 *          ③ 临界区（kernel 层临界区原语，中断上下文嵌套安全）→ ④ emit →
 *          ⑤ 退临界区。打日志无失败路径；未就绪（无后端/级别不达）静默返回。
 *          结构预留：emit 独立（日志线程调同一 emit_args 链）；头部生成独立
 *          （时间戳字段扩展）；丢弃点独立（v3 deferred 挂接）。
 *          投递形态（统一异步）：OM_LOG_ASYNC 定义时——就绪（队列已建）走打包入队
 *          （log_async_send，见 log_async.c）；未就绪/裁剪走本文件同步兜底（log_emit）。
 */

#include "core/om_config.h"

#if OM_USE_LOG

#include "core/om_def.h"
#include "core/om_interrupt.h"
#include "osal/osal_time.h" /* 时间戳来源 osal_time_now_monotonic（host 测试经本地桩头） */

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

/** @brief panic 扇出回调：无过滤提满投递（故障上下文——证据保全）
 *  @param ctx 未使用（与 emit_fanout 同形）
 *  @param seg 段数据
 *  @param len 段字节数 */
static void emit_fanout_panic(void *ctx, const char *seg, size_t len)
{
    (void)ctx;
    log_backend_panic_push_all(seg, len);
}

/** @brief 头部生成：输出 "[LVL][HH:MM:SS.mmm][module] "（v2 时间戳字段在此统一加——同步/异步共用）
 *  @param w 写器
 *  @param module 模块实例（name 已由入口校验非 NULL）
 *  @param level 消息级别
 *  @note 时间戳 = osal_time_now_monotonic（32 位单调 ms，线程/ISR 双安全）；
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
 *  @note 兜底路径（未就绪/最小配置）唯一执行者，无条件编译（va_list 版，v1 语义原样）；
 *        就绪路径执行者 = 日志线程（log_async_emit → log_emit_args，同构换入口）；
 *        栈占用 = 段缓冲（OM_LOG_SEGMENT_SIZE）+ 写器状态，不随消息长度增长 */
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

/** @brief panic emit：与 log_emit 唯一差异 = 无 any_accepts 过滤 + panic 投递（提满全出）
 *  @param module 模块实例
 *  @param level 消息级别
 *  @param fmt 格式串
 *  @param ap 可变参数
 *  @note 同构互见（log_emit 注释）；emit_header 复用——时间戳 + 级别标注自动保留 */
static void log_emit_panic(const OmLogModule *module, OmLogLevel level, const char *fmt, va_list ap)
{
    LogBufWriter w;
    char seg[OM_LOG_SEGMENT_SIZE];
    log_buf_writer_init(&w, emit_fanout_panic, NULL, seg, sizeof(seg));
    emit_header(&w, module, level);
    log_format(&w, fmt, ap);
    log_buf_write(&w, "\n", 1); /* 统一结束符（框架侧一处——与正常路径一致） */
    log_buf_flush(&w);
}

/** @brief emit（日志线程）：后端接受判定 → 头部 + 流式格式化 + 尾部 \n + 扇出
 *  @param module 模块实例
 *  @param level 消息级别
 *  @param fmt 格式串
 *  @param args 参数数组（参数包）
 *  @param n 参数个数
 *  @note 就绪路径 = 日志线程（log_async_emit）调此函数（v1 结构预留兑现）；
 *        兜底路径 = 调用侧（log_emit，va_list 版——同构执行位置不同） */
void log_emit_args(const OmLogMsg *msg)
{
    if (!log_backend_any_accepts(msg->level))
    {
        return; /* ② 无后端接受 → 零格式化开销 */
    }
    LogBufWriter w;
    char seg[OM_LOG_SEGMENT_SIZE];
    log_buf_writer_init(&w, emit_fanout, (void *)(uintptr_t)msg->level, seg, sizeof(seg));
    emit_header(&w, msg->module, msg->level);
    log_format_args(&w, msg->fmt, msg->argBuf, msg->argCount);
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
#if OM_LOG_ASYNC
    if (log_async_can_enqueue()) /* 队列已建=就绪（未就绪落入下方同步兜底——含调度器前） */
    {
        OmLogMsg msg;
        if (log_msg_build(&msg, module, level, fmt, ap))
        {
            (void)log_async_send(&msg); /* 队列满内部计数丢弃 */
        }
        va_end(ap);
        return; /* 就绪：调用侧到此（~1-2µs，无临界区——打包本地 + 非阻塞入队） */
    }
#endif
    /* 同步兜底：最小配置（OM_LOG_ASYNC 未定义）或未就绪（调度器前/SERVICE init 前） */
    {
        port_critical_key_t key;
        key = om_hw_disable_interrupt();  /* ③ 临界区（线程/中断上下文均安全，嵌套安全） */
        log_emit(module, level, fmt, ap); /* va_list 版（SYNC 路径原样保留，无条件编译） */
        om_hw_restore_interrupt(key);     /* ⑤ */
    }
    va_end(ap);
}

/** @brief 故障直出（契约见 services/log/log.h）
 *  @param module 模块实例
 *  @param level 消息级别（标注保留；提满——无过滤）
 *  @param fmt 格式串
 *  @note 自身禁中断（嵌套安全——fatal handler 调用时中断可能仍开；嵌套安全原语）→
 *        调用侧同步格式化 + panic 投递（钩子优先/push 兜底）→ 恢复中断 */
void om_log_panic(const OmLogModule *module, OmLogLevel level, const char *fmt, ...)
{
    va_list ap;
    port_critical_key_t key;
    if (module == NULL || module->name == NULL || fmt == NULL || level >= OM_LOG_LEVEL_OFF)
    {
        return; /* 无失败路径：非法参数静默 */
    }
    va_start(ap, fmt);
    key = om_hw_disable_interrupt(); /* panic 自身禁中断（嵌套安全——与正常路径临界区同款原语） */
    log_emit_panic(module, level, fmt, ap);
    om_hw_restore_interrupt(key);
    va_end(ap);
}

#endif /* OM_USE_LOG */
