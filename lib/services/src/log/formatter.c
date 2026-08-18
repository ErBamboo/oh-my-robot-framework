/**
 * @file formatter.c
 * @brief log 流式格式化器（printf 风格子集，逐段回调输出）
 * @details 支持：标志 -/0、宽度、长度修饰 l；转换符 d i u x X p c s %。
 *          未知/不完整转换符降级为整段规格字面输出（"%5f" 原样打印）；任意长
 *          消息分段回调，栈占用 = 段缓冲 + 常量状态，不随消息长度增长（ADR-0015）。
 *          LONG_MIN 的取负溢出未处理（嵌入式整型格式化惯例，文档约束）。
 */

#include "core/om_config.h"

#ifdef OM_USE_LOG

#include "core/om_def.h"

#include "log_internal.h"

#include <stdarg.h>
#include <string.h>

void log_buf_writer_init(LogBufWriter *w, LogOutFn out, void *out_ctx, char *seg, size_t seg_size)
{
    w->out = out;
    w->outCtx = out_ctx;
    w->seg = seg;
    w->segSize = seg_size;
    w->segLen = 0;
}

static void fmt_flush(LogBufWriter *w)
{
    if (w->segLen > 0)
    {
        w->out(w->outCtx, w->seg, w->segLen);
        w->segLen = 0;
    }
}

void log_buf_flush(LogBufWriter *w)
{
    fmt_flush(w);
}

void log_buf_write(LogBufWriter *w, const char *s, size_t n)
{
    while (n > 0)
    {
        size_t room = w->segSize - w->segLen;
        size_t m = (n < room) ? n : room;
        (void)memcpy(w->seg + w->segLen, s, m);
        w->segLen += m;
        s += m;
        n -= m;
        if (w->segLen >= w->segSize)
        {
            fmt_flush(w);
        }
    }
}

void log_buf_putc(LogBufWriter *w, char c)
{
    log_buf_write(w, &c, 1);
}

static void fmt_pad(LogBufWriter *w, char pad, size_t n)
{
    while (n > 0)
    {
        log_buf_putc(w, pad);
        n--;
    }
}

/* 无符号整数转字符串（逆序填充后反转）；返回长度 */
static size_t fmt_utoa(unsigned long v, char *tmp, int base, int upper)
{
    static const char digits[] = "0123456789abcdef";
    static const char digits_up[] = "0123456789ABCDEF";
    const char *table = upper ? digits_up : digits;
    size_t len = 0;
    if (v == 0)
    {
        tmp[len++] = '0';
    }
    else
    {
        while (v > 0)
        {
            tmp[len++] = table[v % (unsigned)base];
            v /= (unsigned)base;
        }
    }
    for (size_t i = 0; i < len / 2; i++)
    {
        char t = tmp[i];
        tmp[i] = tmp[len - 1 - i];
        tmp[len - 1 - i] = t;
    }
    return len;
}

static void fmt_num(LogBufWriter *w, unsigned long v, int sign, int base, int upper, int width, char pad, int left)
{
    char tmp[32];
    size_t len = fmt_utoa(v, tmp, base, upper);
    size_t total = len + (sign != 0 ? 1 : 0);
    size_t pad_count = (size_t)(width > (int)total ? width - (int)total : 0);
    if (sign != 0 && pad == '0')
    {
        /* 零填充：符号先出再补零（"-0001"），与 libc printf 一致 */
        log_buf_putc(w, (char)sign);
        fmt_pad(w, '0', pad_count);
        log_buf_write(w, tmp, len);
        return;
    }
    if (!left)
    {
        fmt_pad(w, pad, pad_count);
    }
    if (sign != 0)
    {
        log_buf_putc(w, (char)sign);
    }
    log_buf_write(w, tmp, len);
    if (left)
    {
        fmt_pad(w, ' ', pad_count);
    }
}

void log_format(LogBufWriter *w, const char *fmt, va_list ap)
{
    while (*fmt != '\0')
    {
        char c = *fmt++;
        if (c != '%')
        {
            log_buf_putc(w, c);
            continue;
        }
        /* 解析规格：- / 0 / 宽度 / l；spec_start 供未知/不完整规格整体字面输出 */
        const char *spec_start = fmt - 1; /* '%' 起始位 */
        int width = 0;
        char pad = ' ';
        int left = 0;
        int is_long = 0;
        c = *fmt;
        while (c == '-' || c == '0' || (c >= '1' && c <= '9') || c == 'l')
        {
            if (c == '-')
            {
                left = 1;
                pad = ' ';
            }
            else if (c == '0' && width == 0 && !left)
            {
                pad = '0';
            }
            else if (c >= '0' && c <= '9')
            {
                width = width * 10 + (c - '0');
            }
            else if (c == 'l')
            {
                is_long = 1;
            }
            fmt++;
            c = *fmt;
        }
        c = *fmt++;
        if (c == '\0')
        {
            log_buf_write(w, spec_start, (size_t)(fmt - spec_start)); /* 尾部不完整规格整体字面输出 */
            break;
        }
        switch (c)
        {
        case '%':
            log_buf_putc(w, '%');
            break;
        case 'd':
        case 'i': {
            long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            int sign = 0;
            unsigned long u;
            if (v < 0)
            {
                sign = '-';
                u = (unsigned long)(-v);
            }
            else
            {
                u = (unsigned long)v;
            }
            fmt_num(w, u, sign, 10, 0, width, pad, left);
            break;
        }
        case 'u': {
            unsigned long u = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            fmt_num(w, u, 0, 10, 0, width, pad, left);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long u = is_long ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            fmt_num(w, u, 0, 16, (c == 'X'), width, pad, left);
            break;
        }
        case 'p': {
            void *p = va_arg(ap, void *);
            fmt_num(w, (unsigned long)(uintptr_t)p, 0, 16, 0, width, pad, left);
            break;
        }
        case 'c':
            log_buf_putc(w, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL)
            {
                s = "(null)";
            }
            size_t len = strlen(s);
            if (!left)
            {
                fmt_pad(w, ' ', (size_t)(width > (int)len ? width - (int)len : 0));
            }
            log_buf_write(w, s, len);
            if (left)
            {
                fmt_pad(w, ' ', (size_t)(width > (int)len ? width - (int)len : 0));
            }
            break;
        }
        default:
            /* 未知转换符：整段规格字面输出（含已解析的宽度/标志） */
            log_buf_write(w, spec_start, (size_t)(fmt - spec_start));
            break;
        }
    }
    fmt_flush(w);
}

#endif /* OM_USE_LOG */
