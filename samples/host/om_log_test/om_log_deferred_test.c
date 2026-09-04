/**
 * @file om_log_deferred_test.c
 * @brief log 早期缓冲（deferred）+ 丢弃后验告警测试
 * @details 桩：异步桩恒"未就绪"（can_enqueue=false——本目标不编 log_async.c）→ 未就绪路径
 *          进早期缓冲（配置默认开；filter 目标显式 OM_LOG_DEFERRED=0 测同步兜底回归）。
 *          场景：① 无后端时日志入环不回显（不再静默丢）② flush 回放（发起顺序保留）
 *          ③ 回放按条 per-backend 级别过滤 ④ 缓冲满 = 整条回滚（无半条）+ 计数 + flush 时
 *          'early-buffer' WRN ⑤ flush 后未就绪 → 同步兜底（v1 回归）⑥ 告警节流
 *          （显式时刻驱动——时间桩恒 0）⑦ om_log_stats 汇总（早期缓冲项）
 * @note 本目标以 OM_LOG_DEFERRED_BUF_SIZE=128 编译（xmake.lua）——记录容量/溢出可精确构造：
 *       三条短记录（各 37B：3B 头 + 34B 消息）占 111B，第 4 条不可整条容纳 → 回滚。
 */

#include "services/log/log.h"

#include "om_log_test_common.h"

#include "log_internal.h" /* log_deferred_* / log_drop_warn / log_service_module */

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* ---- capture 后端（与 filter 测试同构——多目标独立编译） ---- */
typedef struct
{
    char buf[2048];
    size_t len;
} CaptureBackend;

static CaptureBackend g_cap_a;
static CaptureBackend g_cap_b;
static OmLogBackend g_backend_a;
static OmLogBackend g_backend_b;

static void capture_push_a(OmLogBackend *backend, const char *seg, size_t len)
{
    EXPECT(backend == &g_backend_a);
    (void)memcpy(g_cap_a.buf + g_cap_a.len, seg, len);
    g_cap_a.len += len;
}

static void capture_push_b(OmLogBackend *backend, const char *seg, size_t len)
{
    EXPECT(backend == &g_backend_b);
    (void)memcpy(g_cap_b.buf + g_cap_b.len, seg, len);
    g_cap_b.len += len;
}

static void capture_flush(OmLogBackend *backend)
{
    (void)backend;
}

static OmLogBackend g_backend_a = {"capA", capture_push_a, capture_flush, NULL};
static OmLogBackend g_backend_b = {"capB", capture_push_b, capture_flush, NULL};

/* ---- 被测模块 ---- */
OM_LOG_MODULE(deftest, OM_LOG_LEVEL_INFO);

/** @brief 尾部比对：捕获缓冲复位 len 后须先补 NUL（截断比较——先例见 filter 测试）
 *  @param cap 捕获器
 *  @param expect 期望尾串（NUL 结尾） */
static void expect_tail(CaptureBackend *cap, const char *expect)
{
    size_t n = strlen(expect);
    cap->buf[cap->len] = '\0';
    EXPECT(cap->len >= n);
    if (cap->len >= n)
    {
        EXPECT(strcmp(cap->buf + cap->len - n, expect) == 0);
    }
}

int main(void)
{
    const char *const l1 = "[INF][00:00:00.000][deftest] pre 1\n";
    const char *const l2 = "[INF][00:00:00.000][deftest] mid 2\n";
    const char *const l3 = "[ERR][00:00:00.000][deftest] err 3\n";
    char long_msg[400];
    size_t i;

    memset(&g_cap_a, 0, sizeof(g_cap_a));
    memset(&g_cap_b, 0, sizeof(g_cap_b));
    g_cap_a.buf[0] = '\0';
    g_cap_b.buf[0] = '\0';
    for (i = 0; i < sizeof(long_msg) - 1; i++)
    {
        long_msg[i] = (char)('a' + (i % 26));
    }
    long_msg[sizeof(long_msg) - 1] = '\0';

    /* ① 早期窗口 + 后端未注册：日志入环（不回显、也不静默丢） */
    OM_LOG_INFO("pre %d", 1);
    EXPECT(log_deferred_active());
    EXPECT(g_cap_a.len == 0);

    /* 后端注册（capA 全收 / capB 只收 ERROR+）——回放仍按条过滤 */
    EXPECT(om_log_backend_register(&g_backend_a, OM_LOG_LEVEL_DEBUG) == OM_OK);
    EXPECT(om_log_backend_register(&g_backend_b, OM_LOG_LEVEL_ERROR) == OM_OK);

    /* 早期窗口内（异步未就绪）：继续入环 */
    OM_LOG_INFO("mid %d", 2);
    OM_LOG_ERROR("err %d", 3);
    EXPECT(g_cap_a.len == 0);
    EXPECT(g_cap_b.len == 0);

    /* ④a 超长单条（> 缓冲容量）：整条回滚（无半条数据入环）+ 计数 */
    OM_LOG_INFO("long:%s", long_msg);
    /* ④b 缓冲已满：整条放不下 → 回滚 + 计数（保持最早——前三条完整保留） */
    OM_LOG_INFO("tail %d", 4);

    /* ②③ 回放：发起顺序保留 + per-backend 过滤（capA 全收 / capB 只收 ERR）+
     * 丢弃告警（early-buffer：增量 2 累计 2；WRN 不进 capB=ERROR 级别） */
    log_deferred_flush();
    EXPECT(!log_deferred_active());
    {
        const char *const warn =
            "[WRN][00:00:00.000][log] log drop: early-buffer dropped 2 (total 2)\n";
        char expect_a[512];
        g_cap_a.buf[g_cap_a.len] = '\0';
        g_cap_b.buf[g_cap_b.len] = '\0';
        (void)snprintf(expect_a, sizeof(expect_a), "%s%s%s%s", l1, l2, l3, warn);
        EXPECT(strcmp(g_cap_a.buf, expect_a) == 0); /* 全文比对：顺序 + 无半条 + 告警 */
        EXPECT(strcmp(g_cap_b.buf, l3) == 0);       /* 级别过滤 + 告警未进 capB */
    }

    /* ⑦ 统计汇总：早期缓冲满计数入 dropped */
    {
        OmLogStats st;
        EXPECT(om_log_stats(&st) == OM_OK);
        EXPECT(st.dropped == 2);
    }

    /* ⑤ flush 后未就绪 → 同步兜底（v1 回归：直出不回环） */
    OM_LOG_INFO("post %d", 5);
    expect_tail(&g_cap_a, "[INF][00:00:00.000][deftest] post 5\n");

    /* ⑥ 告警节流（显式时刻驱动——注入状态与时刻，纯状态机） */
    {
        LogDropWarnState st = {0U, UINT32_MAX};
        EXPECT(log_drop_warn(&st, log_service_module(), "queue-full", 1, 0));
        expect_tail(&g_cap_a,
                    "[WRN][00:00:00.000][log] log drop: queue-full dropped 1 (total 1)\n");
        EXPECT(!log_drop_warn(&st, log_service_module(), "queue-full", 5, 0));   /* 节流（间隔未到） */
        EXPECT(log_drop_warn(&st, log_service_module(), "queue-full", 5, 1000)); /* 增量 4 */
        expect_tail(&g_cap_a,
                    "[WRN][00:00:00.000][log] log drop: queue-full dropped 4 (total 5)\n");
        EXPECT(!log_drop_warn(&st, log_service_module(), "queue-full", 5, 2500)); /* 无新增 */
        EXPECT(log_drop_warn(&st, log_service_module(), "queue-full", 7, 2500));  /* 新增 + 间隔已到 */
        expect_tail(&g_cap_a,
                    "[WRN][00:00:00.000][log] log drop: queue-full dropped 2 (total 7)\n");
    }

    if (g_log_test_failed)
    {
        printf("om_log_deferred_test: FAIL\n");
        return 1;
    }
    printf("om_log_deferred_test: ALL PASS\n");
    return 0;
}
