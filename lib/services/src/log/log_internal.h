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

/** @brief 参数包（异步投递的消息载体 —— fmt+args 延后到日志线程格式化）
 *  @note argBuf 为 uintptr_t 宽参数数组（8 个 = 64B）：整型/指针直接存；
 *        %s 只存指针——字符串生命周期由调用方保证 */
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

typedef struct
{
    char conv;   /* 转换符；'\0' = 尾部不完整规格（余下全为字面） */
    int is_long; /* 长度修饰 l（类型宽判定） */
    int width;   /* 最小宽度（0 = 无） */
    char pad;    /* 填充字符（' ' 或 '0'） */
    int left;    /* 左对齐（'-' 标志） */
} LogSpec;

/** @brief 规格解析（格式化语法唯一事实源）：从 '%' 起解析标志（-/0/宽度/l）（输出聚合——最小参数形态）
 *  @param fmtp inout：指向 '%'；成功时前进到转换符之后；不完整（解析至 '\0'）时前进到 '\0'
 *  @return 聚合规格（conv='\0' = 尾部不完整，余下全为字面） */
LogSpec log_spec_next(const char **fmtp);

/** @brief 单调 ms → HH:MM:SS.mmm（十进制，"：" 分隔时分、"." 分隔毫秒）
 *  @param buf 输出缓冲（>=13B，含 NUL）
 *  @param ms 单调毫秒（osal_time_now_monotonic）
 *  @return 写入字节数（不含 NUL，恒 12） */
size_t log_time_format(char *buf, uint32_t ms);

/** @brief 打包：按 fmt 解析参数数 → va_list 逐参抓取进 argBuf（超限丢弃）
 *  @return true = 打包成功（<= OM_LOG_MAX_ARGS 参）；false = 超限丢弃（已计数） */
bool log_msg_build(OmLogMsg *msg, const OmLogModule *module, OmLogLevel level, const char *fmt, va_list ap);

/** @brief 读取丢计数（参数包超限——msg.c 维护；om_log_stats 汇总用）
 *  @return 累计超限丢弃数（自启动以来） */
uint32_t log_dropped_overflow(void);

/** @brief 惰性登记模块（首次日志调用入库——幂等；表满返回 -2（抑制重复尝试））
 *  @param module 模块实例
 *  @return >=0 = moduleId；-2 = 表满；-3 = 参数非法 */
int log_module_check_in(const OmLogModule *module);

/** @brief emit（日志线程/就绪路径）：后端接受判定 → 头部 + 流式格式化 + 尾部
 + 扇出
 *  @param msg 消息包（module/level/fmt/args/n 全内含——最小参数形态）
 *  @note 就绪路径 = 日志线程调此函数；兜底路径 = 调用侧（va_list 版 log_emit 或 log_emit_panic） */
void log_emit_args(const OmLogMsg *msg);

/** @brief 日志构建核心：头部 + 流式格式化 + 尾部 \n + flush 到指定 out 回调
 *  @param module 模块实例
 *  @param level 消息级别
 *  @param fmt 格式串
 *  @param ap 可变参数
 *  @param out 段输出回调（扇出 / 早期缓冲等——执行者唯一差异；一条日志的内容组合事实源）
 *  @param out_ctx 回调上下文
 *  @note 无条件编译（va_list 版）；执行者 = 同步兜底调用侧 / 日志线程 / 早期缓冲入环 */
void log_emit_build(const OmLogModule *module, OmLogLevel level, const char *fmt, va_list ap,
                    LogOutFn out, void *out_ctx);

#if OM_LOG_ASYNC && OM_LOG_DEFERRED
/** @brief 早期缓冲（deferred）：未就绪窗口（调度器前/SERVICE init 前）日志入固定环形，
 *         异步就绪后按条回放——发起时间戳/发起顺序保留，无后端接受时不再静默丢失
 *  @return true = 早期窗口开启（om_log_log 未就绪分支应走 log_deferred_emit） */
bool log_deferred_active(void);

/** @brief 入环一条日志（整条记录 = [级别 1B][长度 2B][消息字节]；放不下=整条回滚+计数）
 *  @param module 模块实例
 *  @param level 消息级别
 *  @param fmt 格式串
 *  @param ap 可变参数
 *  @note 仅 log_deferred_active()=true 时调用；调用方须在临界区内（与兜底路径同构——早期
 *        窗口内仅 main/init 执行期，ISR 打日志仍守 ERROR/FATAL 短消息） */
void log_deferred_emit(const OmLogModule *module, OmLogLevel level, const char *fmt, va_list ap);

/** @brief 回放 + 关闭早期窗口（log_async_init 末尾调用；幂等——已关闭则无操作）
 *  @note 回放 = 按条 per-backend 过滤扇出（发起时间戳/顺序保留）；随后丢弃告警（WRN 节流） */
void log_deferred_flush(void);

/** @brief 读取早期缓冲满丢弃计数（整条回滚——om_log_stats 汇总用）
 *  @return 累计早期缓冲丢弃数（自启动以来） */
uint32_t log_deferred_dropped(void);

/** @brief 丢弃后验告警状态（每丢弃点一实例；warned_upto=已上报累计，last_warn_ms=上次上报时刻） */
typedef struct
{
    uint32_t warned_upto;
    uint32_t last_warn_ms;
} LogDropWarnState;

/** @brief 丢弃后验告警：丢弃只计数 → 补发 WRN 自证（节流 + 增量报告）
 *  @param st 状态（每丢弃点一个）
 *  @param module 告警模块实例（log_service_module——"log"）
 *  @param site 丢弃点描述（静态字符串：如 "queue-full"）
 *  @param dropped 丢弃点累计丢弃数
 *  @param now_ms 单调毫秒（调用方取 osal_time_now_monotonic）
 *  @return true = 已补发 WRN（内部经 log_emit_args——直接 emit，不走队列，无递归）
 *  @note 节流：距上次 >= OM_LOG_DROP_WARN_INTERVAL_MS；报告 = 增量 + 累计 */
bool log_drop_warn(LogDropWarnState *st, const OmLogModule *module, const char *site,
                   uint32_t dropped, uint32_t now_ms);

/** @brief 框架内部告警模块实例（"log"——弃点告警的消息头 module 标注；不进模块注册表） */
const OmLogModule *log_service_module(void);
#endif /* OM_LOG_ASYNC && OM_LOG_DEFERRED */

#if OM_LOG_ASYNC
/** @brief 异步路径初始化：建队列 + 日志线程（经 OM_INIT_SERVICE 调用；最小配置无此声明） */
OmRet log_async_init(void);

/** @brief 异步路径就绪？队列已建（调度器后、线程创建前窗口内为 false——调用方走同步兜底）
 *  @return true = 就绪（log_async_send 可用） */
bool log_async_can_enqueue(void);

/** @brief 异步路径入队（build 成功后调用；队列满 → 丢弃+计数）
 *  @return true = 已入队 */
bool log_async_send(const OmLogMsg *msg);

/** @brief 读取丢计数（异步队列满——log_async.c 维护；最小配置/兜底恒 0）
 *  @note 仅在 OM_LOG_ASYNC 下编译/调用（stats.c 以同样 #ifdef 包裹调用点）
 *  @return 累计队列满丢弃数（自启动以来） */
uint32_t log_dropped_queue(void);
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

/** @brief panic 投递：无过滤遍历全部已注册后端，panic 钩子优先（NULL→push 尽力）
 *  @param seg 段数据
 *  @param len 段字节数 */
void log_backend_panic_push_all(const char *seg, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __LOG_INTERNAL_H__ */
