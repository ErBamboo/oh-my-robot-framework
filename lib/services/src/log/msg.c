/**
 * @file msg.c
 * @brief log 参数包打包器（异步投递的消息载体：fmt + 参数数组，日志线程延后格式化）
 * @details log_msg_build：预扫描参数数（只数不取，快速上限判定）→ 超限丢弃（g_dropped_overflow
 *          计数，T6 接入查询 API）→ 单遍解析规格逐参抓取进 argBuf。
 *          规格解析唯一事实源是 log_spec_next（count 循环/抓取循环共用，防解析漂移）；
 *          取值类型谱与 formatter.c log_format_args 逐型对应（T2 定义，等价性 host 测试兜底）。
 */

#include "core/om_config.h"

#ifdef OM_USE_LOG

#include "core/om_def.h"

#include "log_internal.h"

#include <stdarg.h>

/** @brief 超限丢弃计数（static——T6 接入查询 API；本任务只计数不查询） */
static uint32_t g_dropped_overflow;

/** @brief 解析一个规格：从 '%' 起跳过标志（-/0/宽度/l 循环，记录 is_long）
 *  @param fmtp inout：指向 '%'；成功时前进到转换符之后；不完整（解析至 '\0'）时**不前进**
 *  @param is_long 输出：是否出现长度修饰 l
 *  @return 转换符字符；'\0' = 尾部不完整规格（其余内容全为字面，无参可取） */
static char log_spec_next(const char **fmtp, int *is_long)
{
    const char *p = *fmtp;
    int flag = 0;
    p++; /* 越过 '%' */
    for (;;)
    {
        char c = *p;
        if (c == '-' || c == '0' || (c >= '1' && c <= '9') || c == 'l')
        {
            if (c == 'l')
            {
                flag = 1;
            }
            p++;
            continue;
        }
        break;
    }
    if (*p == '\0')
    {
        return '\0'; /* 不完整：不前进，调用方按"余下全字面"处理 */
    }
    *fmtp = p + 1;
    *is_long = flag;
    return *p;
}

/** @brief 参数数扫描：只数转换符个数（不取参；与 log_format_args 的 spec 解析同构——共用
 *          log_spec_next；未知 spec 按 1 参处理（防御，与抓取循环一致））
 *  @param fmt 格式串
 *  @return 参数个数 */
static uint32_t log_msg_count_args(const char *fmt)
{
    uint32_t count = 0;
    while (*fmt != '\0')
    {
        if (*fmt != '%')
        {
            fmt++;
            continue;
        }
        int is_long = 0;
        char spec = log_spec_next(&fmt, &is_long);
        if (spec == '\0')
        {
            break; /* 尾部不完整规格：无参 */
        }
        if (spec != '%')
        {
            count++;
        }
    }
    return count;
}

bool log_msg_build(OmLogMsg *msg, const OmLogModule *module, OmLogLevel level, const char *fmt, va_list ap)
{
    uint32_t n = log_msg_count_args(fmt);
    if (n > OM_LOG_MAX_ARGS)
    {
        g_dropped_overflow++; /* 超限丢弃（计数已增；T6 接入查询 API） */
        return false;
    }
    msg->fmt = fmt;
    msg->level = level;
    msg->module = module;
    msg->argCount = 0;
    while (*fmt != '\0')
    {
        if (*fmt != '%')
        {
            fmt++;
            continue;
        }
        int is_long = 0;
        char spec = log_spec_next(&fmt, &is_long);
        if (spec == '\0')
        {
            break; /* 尾部不完整规格：余下字面（format_args 原样输出），无参可取 */
        }
        switch (spec)
        {
        case '%':
            break; /* 字面 '%'，不取参 */
        case 'd':
        case 'i':
            msg->argBuf[msg->argCount] = is_long ? (uintptr_t)va_arg(ap, long) : (uintptr_t)va_arg(ap, int);
            msg->argCount++;
            break;
        case 'u':
        case 'x':
        case 'X':
            msg->argBuf[msg->argCount] =
                is_long ? (uintptr_t)va_arg(ap, unsigned long) : (uintptr_t)(unsigned int)va_arg(ap, unsigned int);
            msg->argCount++;
            break;
        case 'p':
            msg->argBuf[msg->argCount] = (uintptr_t)va_arg(ap, void *);
            msg->argCount++;
            break;
        case 'c':
            msg->argBuf[msg->argCount] = (uintptr_t)(char)va_arg(ap, int);
            msg->argCount++;
            break;
        case 's':
            msg->argBuf[msg->argCount] = (uintptr_t)va_arg(ap, const char *);
            msg->argCount++;
            break;
        default:
            /* 未知转换符：按 1 参防御取参（与 count 一致）——调用方已超出文档化格式子集 */
            msg->argBuf[msg->argCount] = (uintptr_t)va_arg(ap, int);
            msg->argCount++;
            break;
        }
    }
    return true;
}

/** @brief 读取超限丢弃计数（static g_dropped_overflow 的跨文件访问器——om_log_stats 汇总）
 *  @return 累计超限丢弃数 */
uint32_t log_dropped_overflow(void)
{
    return g_dropped_overflow;
}

#endif /* OM_USE_LOG */
