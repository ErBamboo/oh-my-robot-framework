/**
 * @file om_log_formatter_test.c
 * @brief 流式格式化器测试：格式符/宽度标志/l 修饰/段切分/任意长/未知降级
 */

#include "log_internal.h"

#include "om_log_test_common.h"

#include <stdarg.h>
#include <string.h>

static char g_buf[1024];     /* 捕获拼接缓冲（整条比对用） */
static size_t g_len;         /* 已捕获字节数 */
static unsigned g_seg_count; /* 段回调次数（段切分验证用） */

/** @brief 段捕获回调：拼接进 g_buf 并计数（模拟 log 服务的扇出目标）
 *  @param ctx 未使用（LogOutFn 签名要求）
 *  @param seg 段数据
 *  @param len 段字节数 */
static void capture(void *ctx, const char *seg, size_t len)
{
    (void)ctx;
    (void)memcpy(g_buf + g_len, seg, len);
    g_len += len;
    g_seg_count++;
}

/** @brief 格式化并捕获：每次清零缓冲后执行一次 log_format，供断言比对
 *  @param fmt 格式串（printf 风格子集）
 *  @note 必须经 varargs 入口包装——log_format 形参是 va_list（x86_64 下为数组
 *        类型），直接传普通实参是 UB（T3 实测段错误后修复） */
static void format_check(const char *fmt, ...)
{
    LogBufWriter w;
    char seg[32];
    va_list ap;
    g_len = 0;
    g_seg_count = 0;
    log_buf_writer_init(&w, capture, NULL, seg, sizeof(seg));
    va_start(ap, fmt);
    log_format(&w, fmt, ap);
    va_end(ap);
    log_buf_flush(&w);
    g_buf[g_len] = '\0';
}

int main(void)
{
    format_check("%d", -42);
    EXPECT(strcmp(g_buf, "-42") == 0);
    format_check("x=%d", 7);
    EXPECT(strcmp(g_buf, "x=7") == 0);
    format_check("%u", 4294967295U);
    EXPECT(strcmp(g_buf, "4294967295") == 0);
    format_check("%x", 0x1A2B);
    EXPECT(strcmp(g_buf, "1a2b") == 0);
    format_check("%X", 0x1A2B);
    EXPECT(strcmp(g_buf, "1A2B") == 0);
    format_check("%lx", 0x1A2B3C4DUL);
    EXPECT(strcmp(g_buf, "1a2b3c4d") == 0);
    format_check("%p", (void *)0x1234);
    EXPECT(strcmp(g_buf, "1234") == 0);
    format_check("%c%c", 'A', 'B');
    EXPECT(strcmp(g_buf, "AB") == 0);
    format_check("%s", "hello");
    EXPECT(strcmp(g_buf, "hello") == 0);
    format_check("%s", (const char *)NULL);
    EXPECT(strcmp(g_buf, "(null)") == 0);
    format_check("%5d", -1);
    EXPECT(strcmp(g_buf, "   -1") == 0);
    format_check("%05d", -1);
    EXPECT(strcmp(g_buf, "-0001") == 0);
    format_check("%-5d", -1);
    EXPECT(strcmp(g_buf, "-1   ") == 0);
    format_check("%10s", "abc");
    EXPECT(strcmp(g_buf, "       abc") == 0);
    format_check("%-10s", "abc");
    EXPECT(strcmp(g_buf, "abc       ") == 0);
    format_check("100%%");
    EXPECT(strcmp(g_buf, "100%") == 0);
    format_check("%d", 0);
    EXPECT(strcmp(g_buf, "0") == 0);
    format_check(""); /* 空格式串 */
    EXPECT(g_len == 0);
    format_check("%.2f", 1.5); /* 未知转换符：整段规格字面输出 */
    EXPECT(strcmp(g_buf, "%.2f") == 0);
    format_check("%5f", 1.5); /* 宽度被解析仍整段字面输出（不丢宽度前缀） */
    EXPECT(strcmp(g_buf, "%5f") == 0);
    format_check("%", 1); /* 尾部不完整规格：整体字面输出 */
    EXPECT(strcmp(g_buf, "%") == 0);

    /* 段边界：>32B 消息多段回调，拼接后与原文一致 */
    {
        char long_msg[300];
        size_t i;
        for (i = 0; i < sizeof(long_msg) - 1; i++)
            long_msg[i] = (char)('a' + (i % 26));
        long_msg[sizeof(long_msg) - 1] = '\0';
        /* log_format 形参为 va_list（x86_64 下为数组类型），必须经 varargs 包装调用 */
        format_check("%s", long_msg);
        EXPECT(strcmp(g_buf, long_msg) == 0);
        EXPECT(g_seg_count > 1);
    }

    if (g_log_test_failed)
    {
        printf("om_log_formatter_test: FAIL\n");
        return 1;
    }
    printf("om_log_formatter_test: ALL PASS\n");
    return 0;
}
