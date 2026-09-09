/**
 * @file om_log_module_filter_test.c
 * @brief log 后端按模块过滤测试：per-backend 默认级 + 按模块覆盖（覆盖放宽/收紧/
 *        OFF 显式拒/clear 回退默认/get 生效值——覆盖命中或回落默认级的判据）
 */

#include "services/log/log.h"

#include "om_log_test_common.h"

#include <string.h>

/* ---- capture 后端 ---- */
/** @brief 捕获后端状态：拼接接收字节 + 段计数（断言比对与零扇出验证用） */
typedef struct
{
    char buf[1024];
    size_t len;
    unsigned seg_count;
    unsigned panic_called;
} CaptureBackend;

static CaptureBackend g_cap_a;
static CaptureBackend g_cap_b;

/* 前向声明（capture_push_a/b 断言 backend==&g_backend_*，需先见实例声明） */
static OmLogBackend g_backend_a;
static OmLogBackend g_backend_b;

/** @brief capA 捕获 push
 *  @param backend 后端实例（must be &g_backend_a——校验回调收到注册的实例指针，防双重指针回归）
 *  @param seg 段数据
 *  @param len 段字节数 */
static void capture_push_a(OmLogBackend *backend, const char *seg, size_t len)
{
    EXPECT(backend == &g_backend_a);
    (void)memcpy(g_cap_a.buf + g_cap_a.len, seg, len);
    g_cap_a.len += len;
    g_cap_a.seg_count++;
}

/** @brief capB 捕获 push（同 capture_push_a，写 g_cap_b；校验 backend == &g_backend_b）
 *  @param backend 后端实例（must be &g_backend_b）
 *  @param seg 段数据
 *  @param len 段字节数 */
static void capture_push_b(OmLogBackend *backend, const char *seg, size_t len)
{
    EXPECT(backend == &g_backend_b);
    (void)memcpy(g_cap_b.buf + g_cap_b.len, seg, len);
    g_cap_b.len += len;
    g_cap_b.seg_count++;
}

/** @brief 捕获 flush（无操作占位，接口完整性；本测试无调用点） */
static void capture_flush(OmLogBackend *backend)
{
    (void)backend;
}

/* 后端实例：文件级静态（unregister 按指针，main 需访问）；name = 覆盖 API 按名解析键 */
static OmLogBackend g_backend_a = {"backend_a", capture_push_a, capture_flush, NULL};
static OmLogBackend g_backend_b = {"backend_b", capture_push_b, capture_flush, NULL};

/* ---- 被测模块（手写结构体经公开 om_log_log 调用——moduleId=-1 惰性登记，
 *      首次日志入模块表，与 OM_LOG_MODULE 宏路径一致；覆盖 API 依赖可解析 id） ---- */
static OmLogModule g_mod_a = {"mod_a", OM_LOG_LEVEL_DEBUG, -1};
static OmLogModule g_mod_b = {"mod_b", OM_LOG_LEVEL_DEBUG, -1};
static OmLogModule g_mod_c = {"mod_c", OM_LOG_LEVEL_DEBUG, -1};

/** @brief 用例 A：默认级拒绝/接受 + 覆盖放宽生效 + get 生效值（覆盖与默认回落）+
 *        OFF 显式拒 + clear 回退默认 */
static void test_override_basic(void)
{
    /* 后端 a 以默认级 WARN 注册（覆盖 API 按名解析后端） */
    EXPECT(om_log_backend_register(&g_backend_a, OM_LOG_LEVEL_WARN) == OM_OK);

    /* 模块惰性登记促发：mod_a/mod_b 各打一条（INFO 无后端接受 → 滞留环中，随后续
     * drain 以默认 WARN 逐条拒掉——零段零字节，不污染断言） */
    om_log_log(&g_mod_a, OM_LOG_LEVEL_INFO, "reg mod_a");
    om_log_log(&g_mod_b, OM_LOG_LEVEL_INFO, "reg mod_b");

    /* 默认级 WARN：INFO 拒（零格式化）；WARN 收 */
    memset(&g_cap_a, 0, sizeof(g_cap_a));
    om_log_log(&g_mod_a, OM_LOG_LEVEL_INFO, "info dropped by default");
    EXPECT(g_cap_a.seg_count == 0);
    EXPECT(g_cap_a.len == 0);
    om_log_log(&g_mod_a, OM_LOG_LEVEL_WARN, "warn accepted by default");
    EXPECT(g_cap_a.len > 0);

    /* 覆盖放宽 mod_a → INFO：get 生效值 = 覆盖级；未覆盖 mod_b 回落默认级 WARN */
    memset(&g_cap_a, 0, sizeof(g_cap_a));
    EXPECT(om_log_backend_set_module_level("backend_a", "mod_a", OM_LOG_LEVEL_INFO) == OM_OK);
    OmLogLevel eff = OM_LOG_LEVEL_OFF;
    EXPECT(om_log_backend_get_module_level("backend_a", "mod_a", &eff) == OM_OK &&
           eff == OM_LOG_LEVEL_INFO);
    EXPECT(om_log_backend_get_module_level("backend_a", "mod_b", &eff) == OM_OK &&
           eff == OM_LOG_LEVEL_WARN);
    om_log_log(&g_mod_a, OM_LOG_LEVEL_INFO, "info accepted after override");
    EXPECT(g_cap_a.len > 0);
    EXPECT(g_cap_a.seg_count > 0);
    EXPECT(strcmp(g_cap_a.buf, "[INF][00:00:00.000][mod_a] info accepted after override\n") == 0);

    /* 覆盖收紧 mod_b → OFF：WARN 也拒（显式拒优先于默认级——消息滞留环中零扇出） */
    memset(&g_cap_a, 0, sizeof(g_cap_a));
    EXPECT(om_log_backend_set_module_level("backend_a", "mod_b", OM_LOG_LEVEL_OFF) == OM_OK);
    om_log_log(&g_mod_b, OM_LOG_LEVEL_WARN, "warn rejected by off");
    EXPECT(g_cap_a.seg_count == 0);
    EXPECT(g_cap_a.len == 0);

    /* clear mod_b → 回退默认级：WARN 恢复接收（本次 drain 连同上一条滞留 WARN 一并
     * 发射——消费时刻裁判，故此处仅断言增量接收） */
    EXPECT(om_log_backend_clear_module_level("backend_a", "mod_b") == OM_OK);
    memset(&g_cap_a, 0, sizeof(g_cap_a));
    om_log_log(&g_mod_b, OM_LOG_LEVEL_WARN, "warn accepted after clear");
    EXPECT(g_cap_a.len > 0);

    EXPECT(om_log_backend_unregister(&g_backend_a) == OM_OK);
}

int main(void)
{
    test_override_basic();

    if (g_log_test_failed)
    {
        printf("om_log_module_filter_test: FAIL\n");
        return 1;
    }
    printf("om_log_module_filter_test: ALL PASS\n");
    return 0;
}
