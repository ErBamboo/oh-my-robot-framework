/**
 * @file log.h
 * @brief log 服务公共 API（services 层第一个真实服务，ADR-0015 (log_service)）
 * @details 同步模式 + 流式格式化 + 模块注册制 + 后端抽象广播（per-backend 级别）。
 *          设计文档：services/log/README.md；决策：docs/adr/0015-log_service.md。
 *          用法：
 *            OM_LOG_MODULE(supercap, OM_LOG_LEVEL_INFO);   // 每个 .c 顶部一次
 *            OM_LOG_INFO("电压 %d.%02d V", mv / 1000, mv % 1000);
 *          后端：om_log_backend_register + om_log_backend_set_level。
 *          契约：调用宏引用本 TU 的 _om_log_module——未注册模块就调用 = 编译错误；
 *               每 TU 一次注册；中断上下文可调用（只打 ERROR/FATAL 短消息）；
 *               后端 push 必须快速提交（不得阻塞轮询）、不得假设一次 push = 一条日志。
 */

#ifndef __OM_LOG_H__
#define __OM_LOG_H__

#include "core/om_def.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 日志级别：升序严重度（spdlog/log.c 同款方向）
 *  @note OFF 置顶——设置为 OFF = 全关（msg >= OFF 永不成立）；MAX 为计数哨兵；
 *        枚举只增不改（后补 TRACE 安全） */
typedef enum {
    OM_LOG_LEVEL_DEBUG = 0,
    OM_LOG_LEVEL_INFO,
    OM_LOG_LEVEL_WARN,
    OM_LOG_LEVEL_ERROR,
    OM_LOG_LEVEL_FATAL,
    OM_LOG_LEVEL_OFF,
    OM_LOG_LEVEL_MAX,
} OmLogLevel;

/** @brief 模块实例（OM_LOG_MODULE 生成，静态常量；仅编译期级别，运行时过滤表见 v3） */
typedef struct OmLogModule {
    const char *name;
    OmLogLevel compileLevel;
} OmLogModule;

/** @brief 输出后端：分段友好（一条日志多段回调）+ 快速提交（绝不阻塞轮询）
 *  @note push/flush 携带 backend 指针——实现者经 container_of 取实例状态（支持多实例，
 *        Zephyr/Linux/RT-Thread 同构）；后端结构体内嵌进实例结构体，位置任意
 *        （container_of 经 offsetof 定位，无需首成员） */
typedef struct OmLogBackend {
    const char *name;                                                            /* 查找/调试用 */
    void (*push)(struct OmLogBackend *backend, const char *segment, size_t len); /* 流式段推送（线程/中断上下文均可能） */
    void (*flush)(struct OmLogBackend *backend);                                 /* 可选：强制刷出，可为 NULL（v1 无调用点） */
} OmLogBackend;

/**
 * @brief 模块注册：每个 TU 顶部一次，生成静态 _om_log_module
 * @param name 模块名（诊断/查找用）
 * @param level 编译期级别（其下整条编出去，常量折叠零成本）
 * @note 同一 TU 重复调用 = 重复定义；未注册就使用调用宏 = 编译错误（特性）
 */
#define OM_LOG_MODULE(name, level) OM_USED static const OmLogModule _om_log_module = {(#name), (level)}

/** @brief 调用宏（引用本 TU 的 _om_log_module；fmt 为 printf 风格子集） */
#define OM_LOG_DEBUG(...) om_log_log(&_om_log_module, OM_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define OM_LOG_INFO(...)  om_log_log(&_om_log_module, OM_LOG_LEVEL_INFO, __VA_ARGS__)
#define OM_LOG_WARN(...)  om_log_log(&_om_log_module, OM_LOG_LEVEL_WARN, __VA_ARGS__)
#define OM_LOG_ERROR(...) om_log_log(&_om_log_module, OM_LOG_LEVEL_ERROR, __VA_ARGS__)
#define OM_LOG_FATAL(...) om_log_log(&_om_log_module, OM_LOG_LEVEL_FATAL, __VA_ARGS__)

/** @brief 日志入口：过滤（编译期+后端接受）→ 临界区 → emit（头部+格式化+广播）→ 退临界区
 *  @param module 模块实例（OM_LOG_MODULE 生成；NULL 或 name 为 NULL 时静默返回）
 *  @param level 消息级别（>= OM_LOG_LEVEL_OFF 时静默返回）
 *  @param fmt printf 风格子集格式串（NULL 时静默返回）
 *  @note 无失败路径（打日志不打扰调用方）；未就绪（无后端接受）走过滤流水线返回；
 *        线程/中断上下文均可调 */
void om_log_log(const OmLogModule *module, OmLogLevel level, const char *fmt, ...);

/** @brief 注册输出后端（携带初始级别；运行时用 om_log_backend_set_level 动态调整）
 *  @param backend 后端实例（name 与 push 不得为 NULL）
 *  @param level 初始级别（过滤语义同 set_level：只接收 level >= 目标级别的消息）
 *  @return OM_OK 成功；OM_ERR_ALREADY 重复注册；OM_ERR_FULL 表满；
 *          OM_ERR_INVALID_ARG 参数非法或级别越界（>= OM_LOG_LEVEL_MAX） */
OmRet om_log_backend_register(OmLogBackend *backend, OmLogLevel level);

/** @brief 注销输出后端
 *  @param backend 已注册的后端实例指针
 *  @return OM_OK 成功；OM_ERR_NOT_FOUND 指针不在表内；OM_ERR_INVALID_ARG 参数非法 */
OmRet om_log_backend_unregister(OmLogBackend *backend);

/** @brief 设置后端级别：只接收 level >= 目标级别的消息；OM_LOG_LEVEL_OFF = 全关
 *  @param backend_name 后端名称（strcmp 按名查找）
 *  @param level 目标级别（>= OM_LOG_LEVEL_MAX 为越界）
 *  @return OM_OK 成功；OM_ERR_NOT_FOUND 名称未找到；OM_ERR_INVALID_ARG 参数非法或级别越界 */
OmRet om_log_backend_set_level(const char *backend_name, OmLogLevel level);

#ifdef __cplusplus
}
#endif

#endif /* __OM_LOG_H__ */
