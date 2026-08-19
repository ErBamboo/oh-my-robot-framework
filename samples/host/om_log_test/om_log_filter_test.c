/**
 * @file om_log_filter_test.c
 * @brief log 管线测试：om_log_log 全链（编译期门控/后端扇出/per-backend 过滤/
 *        OFF/注册注销错误码/段切分拼接/头部格式）
 */

#include "services/log/log.h"

#include "om_log_test_common.h"

#include <string.h>

/* ---- capture 后端 ---- */
/** @brief 捕获后端状态：拼接接收字节 + 段计数（断言比对与段切分验证用） */
typedef struct
{
    char buf[1024];
    size_t len;
    unsigned seg_count;
} CaptureBackend;

static CaptureBackend g_cap_a;
static CaptureBackend g_cap_b;

/** @brief capA 捕获 push（push 契约无 ctx——README 同款：一后端一 push + 静态状态）
 *  @param seg 段数据
 *  @param len 段字节数 */
static void capture_push_a(const char *seg, size_t len)
{
    (void)memcpy(g_cap_a.buf + g_cap_a.len, seg, len);
    g_cap_a.len += len;
    g_cap_a.seg_count++;
}

/** @brief capB 捕获 push（同 capture_push_a，写 g_cap_b）
 *  @param seg 段数据
 *  @param len 段字节数 */
static void capture_push_b(const char *seg, size_t len)
{
    (void)memcpy(g_cap_b.buf + g_cap_b.len, seg, len);
    g_cap_b.len += len;
    g_cap_b.seg_count++;
}

/** @brief 捕获 flush（无操作占位，接口完整性；v1 无调用点） */
static void capture_flush(void)
{
}

/* 后端实例：文件级静态（unregister 按指针，main 需访问） */
static OmLogBackend g_backend_a = {"capA", capture_push_a, capture_flush};
static OmLogBackend g_backend_b = {"capB", capture_push_b, capture_flush};

/** @brief 注册 capA/capB 并验证管理 API 错误码
 *  @note capA 默认 DEBUG（全收）；capB set_level 收紧到 ERROR 以上；本函数即
 *        管理 API 的负向用例（重复注册/NULL/未找到/级别越界） */
static void setup_backends(void)
{
    memset(&g_cap_a, 0, sizeof(g_cap_a));
    memset(&g_cap_b, 0, sizeof(g_cap_b));
    EXPECT(om_log_backend_register(&g_backend_a) == OM_OK);
    EXPECT(om_log_backend_register(&g_backend_b) == OM_OK);
    EXPECT(om_log_backend_register(&g_backend_a) == OM_ERR_ALREADY); /* 重复注册 */
    EXPECT(om_log_backend_register(NULL) == OM_ERR_INVALID_ARG);
    EXPECT(om_log_backend_set_level("capB", OM_LOG_LEVEL_ERROR) == OM_OK);
    EXPECT(om_log_backend_set_level("no_such", OM_LOG_LEVEL_ERROR) == OM_ERR_NOT_FOUND);
    EXPECT(om_log_backend_set_level("capA", OM_LOG_LEVEL_MAX) == OM_ERR_INVALID_ARG);
}

/* ---- 被测模块（注册宏 + 调用宏冒烟） ---- */
OM_LOG_MODULE(testmod, OM_LOG_LEVEL_INFO);

int main(void)
{
    setup_backends();

    /* INFO：capA（DEBUG 全收）收，capB（ERROR）不收 */
    OM_LOG_INFO("hello %d", 42);
    EXPECT(strcmp(g_cap_a.buf, "[INF][testmod] hello 42\r\n") == 0);
    EXPECT(g_cap_b.len == 0);

    /* DEBUG < 编译期 INFO → 全静默（编译期门控） */
    OM_LOG_DEBUG("below compile level");
    EXPECT(g_cap_a.len == strlen("[INF][testmod] hello 42\r\n"));

    /* ERROR：capA + capB 都收 */
    OM_LOG_ERROR("oops %s", "x");
    EXPECT(strcmp(g_cap_b.buf, "[ERR][testmod] oops x\r\n") == 0);
    EXPECT(strcmp(g_cap_a.buf + strlen("[INF][testmod] hello 42\r\n"),
                  "[ERR][testmod] oops x\r\n") == 0);

    /* 段切分：>32B 消息多段回调，拼接一致 */
    {
        char long_msg[200];
        size_t i;
        for (i = 0; i < sizeof(long_msg) - 1; i++)
        {
            long_msg[i] = (char)('a' + (i % 26));
        }
        long_msg[sizeof(long_msg) - 1] = '\0';
        g_cap_a.seg_count = 0;
        OM_LOG_INFO("long:%s", long_msg);
        char expect[240];
        (void)snprintf(expect, sizeof(expect), "[INF][testmod] long:%s\r\n", long_msg);
        EXPECT(strcmp(g_cap_a.buf + g_cap_a.len - strlen(expect), expect) == 0);
        EXPECT(g_cap_a.seg_count > 1);
    }

    /* 级别调节：capA 调 OFF → ERROR 只进 capB */
    EXPECT(om_log_backend_set_level("capA", OM_LOG_LEVEL_OFF) == OM_OK);
    size_t len_a_off = g_cap_a.len;
    size_t len_b_before = g_cap_b.len;
    OM_LOG_ERROR("after off");
    EXPECT(g_cap_a.len == len_a_off);
    EXPECT(g_cap_b.len > len_b_before);

    /* capB 也 OFF → 全静默（无后端接受 → 零格式化） */
    EXPECT(om_log_backend_set_level("capB", OM_LOG_LEVEL_OFF) == OM_OK);
    size_t len_b_off = g_cap_b.len;
    OM_LOG_ERROR("all off");
    EXPECT(g_cap_b.len == len_b_off);

    /* 注销：OK → 重复 NOT_FOUND → NULL INVALID_ARG */
    EXPECT(om_log_backend_unregister(&g_backend_a) == OM_OK);
    EXPECT(om_log_backend_unregister(&g_backend_a) == OM_ERR_NOT_FOUND);
    EXPECT(om_log_backend_unregister(NULL) == OM_ERR_INVALID_ARG);
    EXPECT(om_log_backend_unregister(&g_backend_b) == OM_OK);

    if (g_log_test_failed)
    {
        printf("om_log_filter_test: FAIL\n");
        return 1;
    }
    printf("om_log_filter_test: ALL PASS\n");
    return 0;
}
