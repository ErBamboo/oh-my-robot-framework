# Log 服务设计文档

## 概述

Log 是 OM 框架 **services 层**的日志服务（services 层第一个真实服务），对标业界成熟日志方案（Zephyr LOG、Linux printk、RT-Thread log/ULOG、spdlog），围绕**自身核心功能**独立设计：级别体系、printf 风格流式格式化、过滤（编译期模块级 + 运行时后端级）、输出后端抽象（多后端广播 + per-backend 级别）、并发保护（临界区贯穿 + 中断上下文契约）。

核心设计原则（决策见 ADR-0015 (log_service)）：

- **独立服务**：核心功能驱动设计，不被单一下游需求扭曲本体 API 与分层；**fatal 设施是下游消费方**，其记录点接入（panic 路径）属消费方适配，列入演进阶梯 v3——log 本体不因 fatal 改变设计
- **模块注册制**：每模块编译期声明级别（Zephyr 同构），级别以下编译期零成本裁剪（常量折叠）
- **流式格式化**：格式化器逐段推后端，任意长日志不截断，栈占用 = 段缓冲 + 格式化状态（不随日志长度增长）
- **后端抽象**：广播模型 + per-backend 运行时级别；后端契约 = **分段友好**（一条日志多段回调）+ **快速提交**（绝不阻塞轮询）
- **打日志无失败路径**：日志失败绝不打扰调用方（业界共识）；错误码只出现在管理类 API（后端注册/注销）

代码位置：公共头 `services/log/log.h` 为 API 事实源；实现位于 services 层 log 模块（实现文件路径以符号 grep 定位，不在此列举）；经 selfreg 规则直接编译进 binary；**无初始化生命周期**（静态零初始化，无需 OM_INIT 注册）。

## 运行形态与并发模型

**v1 = 同步库形态**：log 服务**不是任务**——无线程、无队列、无工作循环，是嵌在调用方执行上下文里的库（rt_kprintf / Zephyr LOG sync 同构）。后端也不是消费者任务：`push()` 回调在**调用者的线程/中断上下文**里被同步执行，"消费"发生在记录发生的同一上下文。

```
线程 ──┐
线程 ──┼─→ om_log_log() ─→ ① 编译期过滤 ─→ ② 有无后端接受?
中断 ──┘                                  │ 无 → return（零开销）
                                          │ 有
                                    ┌─────┘
                            ③ 临界区（kernel 层临界区原语，串行化所有生产者）
                            ④ emit：头部 → 流式格式化一次 → 逐段广播到全部
                              通过过滤的后端（同一上下文顺序调用，后端零并发）
                            ⑤ 退临界区
```

**匹配语义（广播模型，Zephyr LOG / Linux printk 同构）**：一条消息被所有**通过 per-backend 级别过滤**的后端消费；过滤不通过的后端整条跳过（零成本，不等格式化）。实现上**一次格式化、多路扇出**——formatter 只跑一遍，段依次推给所有接受它的后端，无每后端独立格式化、无消息复制；后端按"快速提交"契约自行决定留存（推发送缓冲/拷入自己的写队列）。后端增删零感知，生产者不需要了解消费拓扑。

**并发处理**：所有生产者（多线程/中断）× 所有后端收敛在**一个串行化点**——v1 是贯穿整条记录的临界区（整条原子、不穿插）；后端之间零并发、零同步需求（同一上下文顺序调用）。代价：临界区长度 = 最慢后端的一个 push（"快速提交"契约约束在短区间）。单核假设。

**v2 演进 = 异步队列形态**（生产者入队 → 日志线程消费输出，printk kthread / Zephyr async / ULOG 同构）：串行化点变为两级——生产者侧临界区只护入队（极短），消费者侧单一日志线程串行化输出；后端间仍零并发。慢后端（flash 写/网络）可在自己实现内自建任务/缓冲（log 服务本体只认 `push` 契约，不规定后端内部形态）。

## API 面

```c
typedef enum {
    OM_LOG_LEVEL_DEBUG = 0,  /* 升序严重度（spdlog/log.c 同款方向） */
    OM_LOG_LEVEL_INFO,
    OM_LOG_LEVEL_WARN,
    OM_LOG_LEVEL_ERROR,
    OM_LOG_LEVEL_FATAL,
    OM_LOG_LEVEL_OFF,        /* 置顶：设置为 OFF = 全关（msg >= OFF 永不成立） */
    OM_LOG_LEVEL_MAX,        /* 计数哨兵 */
} OmLogLevel;
```

```c
/* 模块注册：每个 TU 顶部一次；level 为编译期级别（其下整个编出去） */
#define OM_LOG_MODULE(name, level) ...

/* 调用宏：引用本 TU 注册的模块实例；未注册模块就调用 → 编译错误（强制先注册） */
#define OM_LOG_DEBUG(fmt, ...)  ...
#define OM_LOG_INFO(fmt, ...)   ...
#define OM_LOG_WARN(fmt, ...)   ...
#define OM_LOG_ERROR(fmt, ...)  ...
#define OM_LOG_FATAL(fmt, ...)  ...
```

```c
typedef struct OmLogBackend {
    const char *name;                              /* 查找/调试用 */
    void (*push)(const char *segment, size_t len); /* 流式段推送：快速提交，不得阻塞 */
    void (*flush)(void);                           /* 可选：强制刷出，可为 NULL */
} OmLogBackend;

OmRet om_log_backend_register(OmLogBackend *backend);   /* 重复注册 → OM_ERR_ALREADY；表满 → OM_ERR_FULL */
OmRet om_log_backend_unregister(OmLogBackend *backend); /* 未注册 → OM_ERR_NOT_FOUND */
OmRet om_log_backend_set_level(const char *backend_name, OmLogLevel level);
```

## 语义契约

1. **过滤流水线**（调用链 `OM_LOG_XXX` → 入口 `om_log_log`）：
   ```
   ① 编译期：level < 模块 compileLevel → 返回（常量折叠，零成本）
   ② 运行时：无后端接受该级别（无后端 / 全部 per-backend 过滤不通过）→ 返回（零格式化开销）
   ③ 临界区（kernel 层临界区原语，中断上下文嵌套安全）
   ④ emit：头部（级别+模块名）→ 流式格式化 → 每段对每个通过过滤的后端调 push
   ⑤ 退出临界区
   ```
2. **过滤语义**：`msg.level >= threshold` 通过（set WARN → 只出 WARN/ERROR/FATAL）
3. **并发保护**：临界区贯穿整条（③⑤ 之间）；**中断上下文可调用**（同一路径，不穿插）——文档约束：中断里只打 ERROR/FATAL 短消息，格式化开销由调用方自担
4. **后端契约**：`push` 必须快速提交（提交 DMA/推发送缓冲，绝不轮询等待）——这是临界区短、中断调用成立的前提；后端不得假设一次 `push` = 一条日志（分段友好）
5. **打日志无失败路径**：后端 push 失败 → 该段丢弃 + 最小丢计数（诊断可观测）；调用方不受影响
6. **未就绪行为**：调度器前（init 早期级别）调用 → 模块未注册/无后端 → 走 ①② 返回（静默丢弃）；deferred logging（早期缓冲 → 服务就绪后 flush）列入演进阶梯 v3
7. **枚举只增不改**：`OmLogLevel` 只增（后续补 TRACE 安全）；级别常量 `OM_LOG_LEVEL_*` 为本服务唯一属主（kernel 侧 vestigial errhandler 路径使用改名后的 `OM_CPU_LOG_LEVEL_*`，见 ADR-0015）

## 用户指南

```c
#include "services/log/log.h"

OM_LOG_MODULE(supercap, OM_LOG_LEVEL_INFO);

void supercap_update(Supercap *cap)
{
    OM_LOG_DEBUG("低于模块级别，编译期裁掉");              /* 零成本 */
    OM_LOG_INFO("电压 %d.%02d V", mv / 1000, mv % 1000);  /* 流式任意长 */
    OM_LOG_ERROR("电压跌落");                              /* 运行期必出 */
}
```

**接后端**（BSP/平台侧注册，举例）：

```c
#include "services/log/log.h"

static void serial_push(const char *segment, size_t len) { /* 推发送缓冲，不阻塞 */ }
static OmLogBackend g_serial_backend = { "serial", serial_push, NULL };

OmRet serial_log_init(void)
{
    return om_log_backend_register(&g_serial_backend);
}
OM_INIT_DRIVER(serial_log_init);  /* 早于任何打日志的业务代码 */
```

**中断上下文**：允许调用，只打 ERROR/FATAL 短消息；后端已按快速提交契约实现。

## 常见陷阱

1. **每 TU 一次注册**：`OM_LOG_MODULE` 每 TU 顶部一次；同 TU 重复注册 → 重复定义
2. **未注册就调用**：调用宏引用本 TU 模块实例，未注册 = 编译错误（特性：强制先注册）
3. **后端阻塞轮询**：违反快速提交契约 → 临界区被拉长、中断调用堵死（ISR 场景直接灾难）
4. **假设一条日志一次 push**：分段友好是契约——任意长日志天然多段
5. **依赖初始化生命周期**：log 无 OM_INIT 注册、无初始化函数——不要等"log 就绪"信号，静态零初始化即可用
6. **调度器前期望有日志**：v1 静默丢弃（未就绪走过滤流水线返回）；deferred 在 v3

## 演进阶梯（终点画像见 ADR-0015）

- **v1（本次）**：同步模式 + 流式 + 模块注册制 + 后端抽象 + 临界区/中断契约 + host 测试
- **v2**：异步模式（调用方入队零耗时、日志线程 emit——emit 已函数化，只换执行者）+ 统一时间戳（头部字段扩展）
- **v3**：运行时级别调节（模块过滤表）+ shell 前端 + 非易失持久化后端 + **fatal 记录点接入（panic 路径：禁中断同步直出）** + deferred logging（挂接丢弃点）
- **v4**：远程日志链路 + 后端动态管理 + 异步 core host 测试（OS 桩）

**结构预留（v1 已内建）**：emit 独立函数化（v2 异步换执行者）；头部生成独立（v2 时间戳扩展）；丢弃点独立（v3 deferred 挂接）；后端结构体新增字段兼容（designated initializer + 调用点 NULL 宽容，v3 panic 钩子同规则）。

## 参考索引

（稳定锚：公开头文件 / 决策记录 / 关联文档；实现文件路径以符号 grep 定位）

- `services/log/log.h`——API 事实源（级别枚举/注册与调用宏/后端接口/管理 API）
- `docs/adr/0015-log_service.md`——定位与边界、模块注册制/后端抽象/级别体系决策、演进阶梯
- `docs/adr/0014-fatal_error_and_startup_split.md`——fatal 设施（下游消费方；记录点接入演进见 ADR-0015 v3）
- `docs/error_code_system.md`——OmRet 错误码体系（模块别名映射）
