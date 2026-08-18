# ADR-0015：Log 服务（services 层日志服务）

- 状态：已设计（v1 实施随 feature/log-service 落地）
- 日期：2026-08-19
- 参考：Zephyr LOG（模块注册制/后端抽象/日志线程/过滤表）、Linux printk（环形缓冲/console 异步化/丢日志不丢调用）、RT-Thread log 与 ULOG（定长缓冲/异步模式）、spdlog（级别方向/OFF 语义/多 sink）

## 背景 (Context)

1. **services 层无真实服务范式**：comm 等全部为骨架（空 README），"分层原语化"缺少首个服务验证；断言、启动失败、硬件异常均无输出，诊断靠 printf 散落/断点
2. **fatal 设施记录点为空**（ADR-0014 (fatal_error_and_startup_split)）：ctx 携带 file/line/pc/detail 却无处输出——但 **log 是独立服务，不被 fatal 需求驱动**；fatal 记录点接入是下游消费方适配，不得扭曲 log 本体设计
3. **调度器前无日志设施**：ADR-0014 已明确 fatal "不承诺记录"；日志服务的早期缓冲（deferred logging）是独立演进项

## 考虑过的方案 (Options)

### v1 范围
- **最小闭环 / +后端抽象 / +异步 / 全上（含 deferred）**：采纳 **最小闭环 + 后端抽象**——多后端广播 + per-backend 级别 v1 落地；异步（调用方零耗时/ISR 全安全）列 v2；deferred 列 v3
- **定长缓冲单次写 / 流式分段 / 极简子集**：采纳 **v1 即流式**——任意长不截断、栈占用恒定（段缓冲 + 状态）；后端接口定义成**分段友好**（一条日志多段回调），与定长单次写实现兼容（单次写 = 一段的平凡特例），v2 换异步格式化器后端接口不动
- **模块注册制（Zephyr 风格）/ 全局调用（rt_kprintf 风格）/ tag 参数制**：采纳**模块注册制**——编译期 per-module 裁剪、资源可控；运行时 per-module 过滤表"后续可加"（v3 与 shell 一起）；tag 字符串运行时比较开销大、易写错
- **级别体系：复用既有 OmLogLevel / 独立设计**：采纳**独立设计**——`OmLogLevel` typedef 全仓零引用（仅宏常量被 vestigial errhandler 路径使用），不背历史包袱；5 级升序（DEBUG=0→FATAL=4，spdlog/log.c 同款方向）+ `OM_LOG_LEVEL_OFF` 置顶（set OFF 全关，`msg >= OFF` 永不成立）+ `OM_LOG_LEVEL_MAX` 计数哨兵；过滤语义 `msg.level >= threshold`；TRACE 不加（枚举只增不改，后补安全）；**`OM_LOG_LEVEL_*` 名归 log 唯一属主**，kernel vestigial errhandler 路径常量改名 `OM_CPU_LOG_LEVEL_*` 迁至其属主头文件（数值不变，调用点纯改名零行为变化）

### 并发保护与中断契约
- **互斥锁 / 临界区 / 调度器锁原语**：采纳**临界区贯穿整条**（现成 kernel 层临界区原语，中断上下文嵌套安全）；不引入调度器锁原语（Zephyr sync 用它只为"ISR 也能调"——我们的 ISR 廉价调用正道是 v2 异步入队，届时调度器锁可能永不需要）
- **中断上下文：允许 / 禁止**：采纳**允许**（同一临界区路径，不穿插）——文档约束"只打 ERROR/FATAL 短消息 + 后端必须快速提交"（快速提交 = 临界区短、中断调用成立的前提）

### 架构形态与可测性
- **services 单包分层 / formatter 下沉 kernel-core / services 通用测试脚手架**：采纳**单包分层**（formatter 纯 C 无依赖 → host 可测；core 过滤判定纯函数化 → host 可测）——formatter 下沉 kernel 违背"服务归属 services"；通用脚手架对单服务 YAGNI
- **host 测试**：沿用 om_core_test 手法（直接注入源文件 + 无宿主测试框架），新建 log 专属 host 测试工程

### 预留策略（演进需求已知，v1 结构预留）
- **结构预留进 v1**：① emit（格式化+广播）独立函数化——v2 异步只换执行者（sync = 调用线程、async = 日志线程，Zephyr `log_msg_process` 同构）；② 头部生成独立——v2 时间戳为字段扩展；③ 丢弃点独立——v3 deferred 缓冲挂接；④ 后端结构体新增字段兼容（designated initializer + 调用点 NULL 宽容，v3 panic 钩子同规则）
- **不预埋模块运行时过滤表**：v3 加 = 注册宏定义一处 + core 两处，**用户模块零改动**（宏展开自动跟进）——不预埋不会造成版本递进大改

### 异步队列基础设施（v2 复用评估；原则：不追求高复用、追求满足需求）
- **队列**：采纳 **osal 消息队列**——定长元素值拷贝 + 阻塞收 + ISR 发 + 满返 WOULD_BLOCK 四合一命中（与"参数包定长区"元素语义互相印证），零适配；**mpsc_ringbuf** 否决——MPSC 无锁 CAS 入队更硬核，但纯轮询 pop 无门铃，需自拼"MPSC 门铃通道"（框架标注为 ipc 层未来扩展、不存在），而日志入队 = 一次定长参数包拷贝，无锁优势不成立；**ipc Pipe** 否决——SPSC 硬约束 + 字节流非消息帧 + 按可用空间截断写，与"多生产者/消息帧"双重错位，需外部临界区串行化 + 自定帧协议
- **日志线程**：采纳 **osal 线程**（死循环 recv→emit，栈/优先级可配；LOW 优先级带语义注释即"日志刷新"）；**async Workqueue** 否决——自带 worker 线程看似可承载消费者，但 Work 去重语义 + 调用者持有节点，高频消息流需 Work 池复用管理；无界链表无容量上限；是"执行投递"原语（卸任务到后台跑回调）而非"数据传递"原语（消息内容搬到消费者）
- **唤醒**：队列自带（recv 阻塞 + send 唤醒），零额外原语；**事件标志否决**——ISR set 走 timer service 命令队列（容量软肋 + 依赖 timer 配置）；信号量仅作 mpsc 方案备选
- **时间戳**：采纳 `osal_time_now_monotonic`（32 位单调 ms、线程/ISR 双安全、回绕语义官方化）
- **表**：后端/模块表 = 定长数组（临界区保护）；corelist（无并发保护）、avltree（缺陷标注 + 全堆）均否决
- **新造权衡**：唯一浮现的新造候选 = MPSC 门铃通道——当出现通用"多生产者无锁入队 + 阻塞消费"需求（不止日志）时，作为 ipc 层独立原语另开 PR；日志 v2 不需要，**v2 零新造基础设施**

## 最终决策 (Decision)

- **定位**：services 层独立服务，对标 Zephyr LOG 级能力；fatal 是下游消费方（panic 路径接入列 v3）
- **运行形态**：v1 同步库形态（无任务无队列，后端回调在调用方上下文同步直写，rt_kprintf / Zephyr sync 同构）；v2 演进为异步队列形态（生产者入队 + 日志线程消费）；生产者↔后端匹配 = 广播模型（一条消息被所有通过 per-backend 级别过滤的后端消费，一次格式化多路扇出，Zephyr / printk 同构）；并发收敛于单串行化点（v1 临界区贯穿整条 / v2 队列+日志线程两级），后端间零并发零同步需求
- **级别体系**（`services/log/log.h`）：`OM_LOG_LEVEL_DEBUG=0/INFO/WARN/ERROR/FATAL/OFF/MAX`（OFF 置顶、MAX 计数哨兵、只增不改）；过滤 `msg.level >= threshold`
- **API**：`OM_LOG_MODULE(name, level)` 每 TU 一次（生成静态模块实例 {name, compileLevel}，编译期裁剪 = 常量折叠）；调用宏 `OM_LOG_DEBUG/INFO/WARN/ERROR/FATAL(fmt, ...)`（引用本 TU 实例，未注册 = 编译错误）；后端接口 `OmLogBackend{name, push, flush}`（push 快速提交、flush 可 NULL）+ `om_log_backend_register/unregister/set_level`（错误码：`OM_ERR_ALREADY/FULL/NOT_FOUND/INVALID_ARG`）
- **过滤流水线**：① 编译期模块级别（折叠）→ ② 运行时无后端接受 → 返回（零格式化）→ ③ 临界区 → ④ emit（头部 + 流式格式化 + per-backend 过滤广播）→ ⑤ 退临界区
- **打日志无失败路径**：后端失败 → 段丢弃 + 最小丢计数；调度器前未就绪 → 静默丢弃（deferred 列 v3）
- **配置**（`core/om_config.h`，appcfg 可覆写）：`OM_USE_LOG` / `OM_LOG_MAX_BACKENDS`（默认 4）/ `OM_LOG_SEGMENT_SIZE`（默认 32）
- **级别常量归位**：`om_def.h` 删除 `OmLogLevel` 枚举（typedef 零引用）；`OM_CPU_LOG_LEVEL_*`（数值不变）迁 om_cpu.h（errhandler 路径属主，使用点均已包含该头，零新增 include）；log.h 成为 `OM_LOG_LEVEL_*` 唯一属主
- **架构**：services 层 log 模块三分（formatter 纯 C 流式 / core 过滤判定纯函数 + 临界区编排 / backend 注册表）；经 selfreg 规则直接注入 binary；**无初始化生命周期**（静态零初始化，无 OM_INIT entry）——**仅限同步模式**：异步模式（v2）引入 `OM_INIT_SERVICE` 建日志线程，"零 init"是同步模式的属性而非服务永恒属性
- **host 测试**：log 专属 host 测试工程——formatter（格式符/段切分/任意长/非法格式串）+ 过滤判定（threshold/OFF/per-backend/折叠语义）；分发/临界区设备验证

## 影响 (Consequences)

- **正面**：services 层第一个真实服务，确立模块注册制/后端抽象/级别体系的层内范式；编译期 per-module 裁剪 + 运行时 per-backend 过滤；任意长流式日志 + 中断上下文可用；formatter/过滤判定 host 可测；结构预留使 v2 异步（换 emit 执行者）与 v3（deferred 挂丢弃点、panic 钩子加字段）均为增量演进
- **约束**：后端必须快速提交（违反者中断场景自担临界区长度）；中断里只打 ERROR/FATAL 短消息；一条日志多段（后端不得假设单次 push = 一条）；每 TU 一次注册；枚举只增不改；v1 调度器前无日志（静默丢弃）
- **兼容**：`om_def.h` 删除 `OmLogLevel`（零类型引用，已验证）；`OM_CPU_LOG_LEVEL_*` 数值与成员语义不变（调用点纯改名，零行为变化）；`OM_LOG_LEVEL_*` 名称易主为 log 服务，任何 TU 同时包含 om_def.h 与 log.h 不再冲突
- **演进**（阶梯）：
  - **v2 异步模式**（emit 换执行者 + 统一时间戳）——**基础设施复用（零新造，逐项权衡见 Options）**：队列 = osal 消息队列（定长元素值拷贝 + 阻塞收 + ISR 发 + 满返 WOULD_BLOCK，四合一命中；与"参数包定长区"元素语义互相印证）；日志线程 = osal 线程（LOW 优先级带，该带语义注释即"日志刷新"）；消费者唤醒由队列自带（不用事件标志——其 ISR set 走 timer service 命令队列，有容量软肋）；时间戳 = `osal_time_now_monotonic`（线程/ISR 双安全）；后端/模块表 = 定长数组（不用链表/树：corelist 无并发保护、avltree 有缺陷标注）；**不新造 MPSC 门铃通道**（框架标注的 ipc 层未来扩展，日志无"无锁入队"硬需求——入队 = 一次定长参数包拷贝）
  - **v3**：运行时级别调节 + shell 前端 + 非易失持久化后端 + fatal 记录点接入（panic 路径）+ deferred logging
  - **v4**：远程日志链路 + 后端动态管理 + 异步 core host 测试（OS 桩）
