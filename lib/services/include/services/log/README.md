# Log 服务设计文档

## 概述

Log 是 OM 框架 **services 层**的日志服务（services 层第一个真实服务），对标业界成熟日志方案（Zephyr LOG、Linux printk、RT-Thread log/ULOG、spdlog），围绕**自身核心功能**独立设计：级别体系、printf 风格流式格式化、过滤（编译期模块级 + 运行时后端级）、输出后端抽象（多后端广播 + per-backend 级别）、并发保护（统一异步：入队 + 日志线程两级串行化；中断上下文契约）。

核心设计原则（决策见 ADR-0015 (log_service)）：

- **独立服务**：核心功能驱动设计，不被单一下游需求扭曲本体 API 与分层；**fatal 设施是下游消费方**，其记录点接入（panic 路径）属消费方适配，列入演进阶梯 v3——log 本体不因 fatal 改变设计
- **模块注册制**：每模块编译期声明级别（Zephyr 同构），级别以下编译期零成本裁剪（常量折叠）
- **流式格式化**：格式化器逐段推后端，任意长日志不截断，栈占用 = 段缓冲 + 格式化状态（不随日志长度增长）
- **后端抽象**：广播模型 + per-backend 运行时级别；后端契约 = **分段友好**（一条日志多段回调）+ **快速提交**（绝不阻塞轮询）
- **打日志无失败路径**：日志失败绝不打扰调用方（业界共识）；错误码只出现在管理类 API（后端注册/注销）

代码位置：公共头 `services/log/log.h` 为 API 事实源；实现位于 services 层 log 模块（实现文件路径以符号 grep 定位，不在此列举）；经 selfreg 规则直接编译进 binary。
**初始化生命周期**：统一异步形态下经 `OM_INIT_SERVICE` 在 SERVICE 级建消息队列 + 日志线程（调度器后——此前的日志走同步兜底，见下节）；最小配置（`OM_LOG_ASYNC` 裁剪）为纯库形态（零 init，v1 语义）。**调用者无需等待"就绪"信号、无需显式初始化**——就绪判定 + 兜底内建（见 ADR-0015 (log_service)）。

## 运行形态与并发模型

**统一异步形态（v2 定稿）**：log 常驻**一个日志线程 + 一个定长消息队列**（printk kthread / Zephyr async / ULOG 同构）。调用方恒走单一提交路径——**就绪（队列已建）**：打包参数包（1-2µs，无临界区）→ 非阻塞入队；**未就绪**：同步兜底（调用侧格式化，36µs）。格式化/扇出恒在**日志线程**（LOW 带）——**所有调用者（含 1kHz 控制环/ISR）恒 1-2µs 返回**，后端按"快速提交"契约被日志线程串行调用（后端零并发、零同步需求）。

```
线程 ──┐
线程 ──┼─→ om_log_log() ─→ ① 编译期过滤 ─→ 就绪判定（OM_LOG_ASYNC 定义 && 队列已建?）
中断 ──┘                                  │ 就绪                    │ 未就绪 / 能力裁剪
                                          ▼                         ▼
                                    打包（1-2µs，无临界区）       同步兜底：③ 临界区 → ④ 调用侧
                                    → 非阻塞入队                   emit（格式化+扇出，36µs——
                                      │ （ISR 自动分流）            调度器前/无 RTOS；早期低频
                                      │ 满 → 丢弃+计数              日志可接受；无后端 → 静默）
                                      ▼
                              日志线程（LOW 带）：recv → emit（格式化+扇出）→ 后端 push
                                              （emit 内含"无后端接受 → 零格式化返回"）
```

**就绪判定与兜底语义**：`log_async_can_enqueue()` = 队列已建（队列/线程经 `OM_INIT_SERVICE` 于 SERVICE 级建——**调度器前/线程创建前窗口内为 false**，调用方走下方同步兜底）。兜底不换语义、不引入 deferred：**同一代码骨架覆盖三情形**——就绪 = 打包入队 + 线程格式化；未就绪 = 调用侧格式化；最小配置（`OM_LOG_ASYNC` 未定义，log_async.c 编译为空）= 同兜底（即 v1 库形态）。`OM_LOG_ASYNC` 默认定于 `core/om_config.h`（能力常驻），无 RTOS/裸机等最小配置经 `om_appcfg.h` `#undef` 裁剪。

**匹配语义（广播模型，Zephyr LOG / Linux printk 同构）**：一条消息被所有**通过 per-backend 级别过滤**的后端消费；过滤不通过的后端整条跳过（零成本，不等格式化）。实现上**一次格式化、多路扇出**——formatter 只跑一遍，段依次推给所有接受它的后端，无每后端独立格式化、无消息复制；后端按"快速提交"契约自行决定留存（推发送缓冲/拷入自己的写队列）。后端增删零感知，生产者不需要了解消费拓扑。

**并发处理**：两级串行化——生产者侧**打包为调用栈本地变量、无临界区**，入队 = 一次定长记录拷贝（线程 `osal_queue_send` 非阻塞 / ISR 自动分流 `send_from_isr`），队列数据结构自带串行化；消费者侧**单一日志线程**串行化 recv → emit。后端之间零并发、零同步需求（同一日志线程顺序调用）。代价：入队临界区极短（拷贝定长记录），格式化/扇出代价移到日志线程（不占生产者时间）。单核假设。慢后端（flash 写/网络）可在自己实现内自建任务/缓冲（log 服务本体只认 `push` 契约，不规定后端内部形态）。

**逐步执行位置**（管线：①提交 → ②匹配 → ③格式化 → ④投递扇出 → ⑤后端输出；"服务核心/formatter/后端"均指 log 服务的执行逻辑）：

| 步骤 | 统一异步（就绪：打包入队） | 兜底（未就绪/最小配置） | v3 混合（+ panic 直出旁路） |
|---|---|---|---|
| ① 提交 | 生产者：编译期过滤 → 就绪判定 → 打包（1-2µs）→ 非阻塞入队 | 生产者：编译期过滤 → 临界区 | 同左；调度器前 = 同右兜底（deferred = 早期缓冲，v3 图景） |
| ② 匹配 | 日志线程投递时（无后端接受 → 零格式化返回） | 生产者（临界区内） | 同左；panic 路径跳过（全出） |
| ③ 格式化 | 日志线程（时间戳此时统一加） | 生产者（formatter） | 日志线程；panic = 故障上下文直出 |
| ④ 投递扇出 | 日志线程 | 生产者 | 同左；panic = 故障上下文直出 |
| ⑤ 后端输出 | 日志线程 | 生产者 | 同左；panic = 故障上下文直出 |

跨版本不变量：**提交永远在生产者上下文**；**后端输出跟随投递的执行者**（后端实现不感知执行者是谁）；**匹配=决定谁收、投递=实际推送**（统一异步在同一日志线程连续/兜底在调用侧临界区内连续——结构性保证零额外机制）；**程序序保证**：同生产者调用序 = push 调用序（就绪 = 队列 FIFO；兜底 = 临界区程序序）。

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
    const char *name;                                                          /* 查找/调试用 */
    void (*push)(struct OmLogBackend *backend, const char *segment, size_t len); /* 流式段推送：快速提交，不得阻塞 */
    void (*flush)(struct OmLogBackend *backend);                               /* 可选：强制刷出，可为 NULL */
} OmLogBackend;  /* push/flush 携带 backend 指针——container_of 取实例状态（多实例，Zephyr 同构） */

OmRet om_log_backend_register(OmLogBackend *backend, OmLogLevel level); /* 注册携带初始级别；重复 → ALREADY；表满 → FULL */
OmRet om_log_backend_unregister(OmLogBackend *backend);                /* 未注册 → OM_ERR_NOT_FOUND */
OmRet om_log_backend_set_level(const char *backend_name, OmLogLevel level); /* 运行时动态调整（per-backend 过滤） */
```

## 语义契约

1. **过滤流水线**（调用链 `OM_LOG_XXX` → 入口 `om_log_log`）：
   ```
   就绪（默认配置，队列已建）：
     ① 编译期：level < 模块 compileLevel → 返回（常量折叠，零成本）
     ② 打包参数包 → 非阻塞入队（满 → 丢弃+计数；ISR 自动分流）——调用侧到此（1-2µs）
     细筛 = 日志线程 emit 链上："无后端接受该级别 → 返回（零格式化开销）"
   兜底（未就绪/最小配置）：
     ① 编译期：同左
     ② 运行时：无后端接受该级别（无后端 / 全部 per-backend 过滤不通过）→ 返回（零格式化开销）
     ③ 临界区（kernel 层临界区原语，中断上下文嵌套安全）
     ④ emit：头部（级别+时间戳 `[HH:MM:SS.mmm]`+模块名）→ 流式格式化 → 每段对每个通过过滤的后端调 push
     ⑤ 退出临界区
   ```
2. **过滤语义**：`msg.level >= threshold` 通过（set WARN → 只出 WARN/ERROR/FATAL）
3. **并发保护**：临界区贯穿整条（③⑤ 之间）；**中断上下文可调用**（同一路径，不穿插）——文档约束：中断里只打 ERROR/FATAL 短消息，格式化开销由调用方自担
4. **FIFO 边界（logger 侧承诺）**：logger 按"单生产者 FIFO + 消息原子"语义**提交**消息给后端——同生产者调用序 = push 调用序（v1 = 临界区程序序，v2 = 队列 FIFO，结构性保证零额外机制）；一条消息的所有段连续推送、消息边界不穿插；广播一致（所有接受的后端在同一临界区内收到完整段序列）。**事务完成点 = push 返回**（同步模式下调用返回前全部 push 已完成）——push 是"提交"而非"送达"；边界之后（后端时序/乱序/送达/持久化，如 CAN 总线仲裁重排、DMA 排空延迟、后端内部队列）由后端自持，logger 不承诺
5. **后端契约**：`push` 必须快速提交（提交 DMA/推发送缓冲，绝不轮询等待）——这是临界区短、中断调用成立的前提；后端不得假设一次 `push` = 一条日志（分段友好）
6. **打日志无失败路径**：后端 push 失败 → 该段丢弃，调用方不受影响。**失败感知分层**（业界共识：输出回调不带状态——Zephyr backend process / printk console write / spdlog sink 均 void）：后端提交被拒（如发送缓冲满）由后端自持诊断；队列满丢弃由 log 服务在队列层计数（v2 异步，printk"丢日志不丢调用"语义）；v1 同步模式 log 服务侧无失败可感知——这是无失败路径契约的推论，非缺陷
7. **未就绪行为**：调度器前（init 早期级别）调用 → 模块未注册/无后端 → 走 ①② 返回（静默丢弃）；deferred logging（早期缓冲 → 服务就绪后 flush）列入演进阶梯 v3
8. **枚举只增不改**：`OmLogLevel` 只增（后续补 TRACE 安全）；级别常量 `OM_LOG_LEVEL_*` 为本服务唯一属主（kernel 侧 vestigial errhandler 路径使用改名后的 `OM_CPU_LOG_LEVEL_*`，见 ADR-0015）
9. **消息结束符**：框架统一追加 `\n`（emit 一处，广播一致——所有后端收到同一行边界；后端可做表示层映射（如转 CRLF）但不得改变行边界语义；未来协议化（\n/NUL 帧/长度前缀）改 emit 一处即全局一致）

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
    return om_log_backend_register(&g_serial_backend, OM_LOG_LEVEL_INFO); /* 初始级别：只收 INFO 及以上 */
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
- **v2**：异步模式（调用方入队零耗时、日志线程 emit——emit 已函数化，只换执行者）+ 统一时间戳（头部字段扩展）；**基础设施复用（零新造）**：队列 = osal 消息队列（定长元素/阻塞收/ISR 发/满丢弃，与参数包定长区互相印证）、日志线程 = osal 线程（LOW 带）、唤醒 = 队列自带、时间戳 = `osal_time_now_monotonic`（线程/ISR 双安全）——权衡细节见 ADR-0015 (log_service)
- **v3**：运行时级别调节（模块过滤表）+ shell 前端 + 非易失持久化后端 + **fatal 记录点接入（panic 路径：禁中断同步直出）** + deferred logging（挂接丢弃点）
- **v4**：远程日志链路 + 后端动态管理 + 异步 core host 测试（OS 桩）

**结构预留（v1 已内建）**：emit 独立函数化（v2 异步换执行者）；头部生成独立（v2 时间戳扩展）；丢弃点独立（v3 deferred 挂接）；后端结构体新增字段兼容（designated initializer + 调用点 NULL 宽容，v3 panic 钩子同规则）。

## 参考索引

（稳定锚：公开头文件 / 决策记录 / 关联文档；实现文件路径以符号 grep 定位）

- `services/log/log.h`——API 事实源（级别枚举/注册与调用宏/后端接口/管理 API）
- `docs/adr/0015-log_service.md`——定位与边界、模块注册制/后端抽象/级别体系决策、演进阶梯
- `docs/adr/0014-fatal_error_and_startup_split.md`——fatal 设施（下游消费方；记录点接入演进见 ADR-0015 v3）
- `docs/error_code_system.md`——OmRet 错误码体系（模块别名映射）
