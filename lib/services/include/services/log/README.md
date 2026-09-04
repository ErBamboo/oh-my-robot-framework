# Log 服务设计文档

## 定位

services 层日志服务：为全体消费者提供统一的日志记录通道。模块注册制（每模块级别——编译期初始=宏参数 + 运行时按模块调节）、流式格式化、后端抽象（广播 + per-backend 级别）、统一异步投递（就绪路径恒低调用侧成本）、故障直出（fatal/崩溃场景的可靠输出路径）。fatal 设施为下游消费方（handler 组合，见"示例·故障组合"）。

## 机制

### 投递（统一异步）

- **就绪（队列已建）**：调用侧打包参数包（约 1-2µs）→ 非阻塞入队 → 日志线程（LOW 带）recv → 格式化 + 扇出 → 后端
- **未就绪（调度器前/SERVICE init 前）**：同步兜底——调用侧格式化 + 扇出（36µs 级；早期低频可接受）
- **故障直出（`om_log_panic`）**：禁中断、绕过队列/线程/锁，调用侧同步格式化；**无 per-backend 过滤（提满全出——崩溃现场证据保全）**；级别标注保留；后端提交 = panic 钩子优先（最可靠通道，如串口轮询写——DMA/中断死仍有效）→ NULL 退回 push 尽力而为

### 过滤与分发

- 三级：模块级（初始=宏参数；运行时 `om_log_module_set_level` 按名调节——惰性登记后生效）→ 后端级（`msg.level >= backend->level`）→ 队列级（满丢弃+计数）
- 广播：一条消息格式化一次，逐段推送给所有通过过滤的后端（无每后端独立格式化、无消息复制）
- FIFO 边界：logger 保证"单生产者 FIFO + 消息原子 + 全后端广播一致"——事务完成点 = push 返回；push 之后的时序/排队/送达归后端自持

### 消息形态

- 流式分段：32B 段缓冲（可配），任意长消息分多段回调（栈占用恒定为段缓冲+状态）
- 统一结束符 `\n`（后端可做表示层映射，不得改变行边界语义）
- 时间戳 `[HH:MM:SS.mmm]`（单调 ms，十进制；线程/ISR 双安全）
- 消息头：`[LVL][HH:MM:SS.mmm][module] msg\n`

### 失败感知（分层）

- 打日志无失败路径：日志失败绝不打扰调用方（错误码仅出现在管理类 API）
- 后端提交被拒 → 后端自持诊断；队列满/参数超限 → log 服务丢计数（`om_log_stats()` 可查）

## 架构

```
lib/services/include/services/log/log.h     ← 公共 API 事实源
lib/services/src/log/log_internal.h          ← 组件间私有接口
lib/services/src/log/formatter.c             ← 流式格式化 + 规格解析（log_spec_next 唯一事实源）+ 时间换算
lib/services/src/log/msg.c                   ← 参数包打包（计数/抓取/上限丢弃）
lib/services/src/log/core.c                  ← 过滤编排/emit/兜底/panic 直出
lib/services/src/log/backend.c               ← 后端表（注册/注销/级别/广播/panic 投递）
lib/services/src/log/log_async.c             ← 队列 + 日志线程（OM_LOG_ASYNC）
lib/services/src/log/stats.c                 ← om_log_stats 汇总
lib/services/src/log/backends/               ← 后端实现（零驱动依赖的随服务家族存放——RTT 在此）
lib/services/src/log/backends/rtt_backend.c  ← RTT 后端（调试通道高带宽；零驱动依赖）
lib/drivers/src/peripheral/serial/           ← 串口后端（drivers→services 单向依赖，随驱动家族存放）
third_party/segger-rtt/                      ← RTT 库本体（外部库——BSD 许可；经 selfreg 注入 binary）
samples/host/om_log_test/                    ← 宿主测试（注入源 + osal 桩）
```

- 依赖：services → kernel（接口/原语）；drivers → services（开放清单：log.h——单向）；platform 只依赖 kernel/third_party 与 drivers 抽象头（PAL）
- 自注册：log 服务经 `OM_INIT_SERVICE` 建队列+线程（异步模式）；backend 注册经组合层 `OM_INIT_DRIVER` 等分散加载

## 接口

```c
/* 级别 */
typedef enum { OM_LOG_LEVEL_DEBUG=0, OM_LOG_LEVEL_INFO, OM_LOG_LEVEL_WARN,
               OM_LOG_LEVEL_ERROR, OM_LOG_LEVEL_FATAL, OM_LOG_LEVEL_OFF, OM_LOG_LEVEL_MAX } OmLogLevel;

/* 模块注册 + 调用宏（每 TU 一次；未注册调用 = 编译错误；上限定 8，可配） */
#define OM_LOG_MODULE(name, level) ...
#define OM_LOG_DEBUG(fmt, ...) / OM_LOG_INFO / OM_LOG_WARN / OM_LOG_ERROR / OM_LOG_FATAL
/*（编译期双重防线：format 属性 + __VA_ARGS__ 计数负数组）*/

/* 后端 */
typedef struct OmLogBackend {
    const char *name;
    void (*push)(OmLogBackend *backend, const char *segment, size_t len);  /* 快速提交 */
    void (*flush)(OmLogBackend *backend);                                  /* 可选 */
    void (*panic)(OmLogBackend *backend, const char *segment, size_t len); /* 可选：故障提交通道 */
} OmLogBackend;
OmRet om_log_backend_register(OmLogBackend *backend, OmLogLevel level);
OmRet om_log_backend_unregister(OmLogBackend *backend);
OmRet om_log_backend_set_level(const char *backend_name, OmLogLevel level);

/* 模块级运行时调节 */
OmRet om_log_module_set_level(const char *module_name, OmLogLevel level);
OmRet om_log_module_get_level(const char *module_name, OmLogLevel *level);

/* 故障直出（fatal/崩溃上下文）——无锁/提满全出/panic 钩子优先 */
void om_log_panic(const OmLogModule *module, OmLogLevel level, const char *fmt, ...);

/* 统计 */
typedef struct { uint32_t dropped; } OmLogStats;
OmRet om_log_stats(OmLogStats *stats);
```

## 配置

| 宏 | 默认 | 说明 |
|---|---|---|
| `OM_USE_LOG` | 1 | 服务级裁剪（值语义：`=0` 或 appcfg `#undef`） |
| `OM_LOG_ASYNC` | 1 | 异步能力（队列+线程）；关闭 = 同步兜底形态（v1 语义）；`=0` 或 appcfg `#undef` |
| `OM_LOG_MAX_ARGS` | 8 | 参数包上限（1..16，超出 `#error`） |
| `OM_LOG_QUEUE_LEN` | 8 | 异步队列深度（满丢弃+计数） |
| `OM_LOG_MAX_BACKENDS` | 4 | 后端表上限 |
| `OM_LOG_MAX_MODULES` | 16 | 模块注册表上限（惰性登记；按名调节） |
| `OM_LOG_SEGMENT_SIZE` | 32 | 段缓冲（字节；栈占用锚） |
| `OM_LOG_RTT` | 0 | 内置默认 RTT 后端开关（1=零接线隐藏注册，0=仅显式 API 注册） |
| `OM_LOG_RTT_NAME` | `"rtt"` | 默认 RTT 后端名（按名调节用） |
| `OM_LOG_RTT_LEVEL` | `OM_LOG_LEVEL_INFO` | 默认 RTT 后端初始级别（越界编译期 `_Static_assert` 报错） |

全宏在 `om_config.h`，均可经 `om_appcfg.h` 覆写。

## 示例

**模块使用**：

```c
#include "services/log/log.h"

OM_LOG_MODULE(supercap, OM_LOG_LEVEL_INFO);

void supercap_update(Supercap *cap)
{
    OM_LOG_DEBUG("低于模块级别，编译期裁掉");
    OM_LOG_INFO("电压 %d.%02d V", mv / 1000, mv % 1000);
    OM_LOG_ERROR("电压跌落");
}
```

/** 调试现场调节：om_log_module_set_level("supercap", OM_LOG_LEVEL_WARN); */

**接后端**（组合层 3 行接线）：

```c
static void serial_push(OmLogBackend *backend, const char *seg, size_t len) { /* 推发送缓冲，不阻塞 */ }
static void serial_panic(OmLogBackend *backend, const char *seg, size_t len) { /* 寄存器级轮询写（DMA 死可用） */ }
static OmLogBackend g_serial_backend = { "serial", serial_push, NULL, serial_panic };

static OmRet serial_log_init(void)
{
    return om_log_backend_register(&g_serial_backend, OM_LOG_LEVEL_INFO);
}
OM_INIT_DRIVER(serial_log_init);  /* 早于任何打日志的业务代码 */
```

（RTT 后端零接线版——宏配置即自动注册，组合层无代码；通道 0 固定；观测经调试接口 J-Link RTT Viewer / RTTClient）：

```c
/* om_appcfg.h：OM_LOG_RTT=1（默认后端）；OM_LOG_RTT_NAME/OM_LOG_RTT_LEVEL 可调节 */
/* 注册由后端自行完成（OM_INIT_DRIVER 隐藏注册，早于业务日志）；多后端广播语义不变 */
```

显式 API 版（`om_rtt_backend_register`）保留：多实例/自定义名场景；与 `OM_LOG_RTT=1` 的默认后端同名时注册表查重返回 `OM_ERR_ALREADY`（二选一使用）。

**故障组合**（fatal handler 覆盖——记录现场 + 恢复）：

```c
#include "core/om_fatal.h"
#include "services/log/log.h"

OM_LOG_MODULE(main_app, OM_LOG_LEVEL_INFO);

void om_fatal_handler(OmFatalReason reason, OmRet cause, const OmFatalContext *ctx)
{
    om_log_panic(&_om_log_module, OM_LOG_LEVEL_FATAL,
                 "fatal reason=%d cause=%d at %s:%d pc=0x%lx detail=%s",
                 (int)reason, (int)cause,
                 ctx->file ? ctx->file : "?", ctx->line,
                 (unsigned long)ctx->pc,
                 ctx->detail ? ctx->detail : "?");
    for (;;) {}  /* 恢复策略（亮灯/复位/回退）——不得返回 */
}
```
