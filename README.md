# Oh My Robot

面向机器人的通用嵌入式开发框架。

## 项目简介

Oh My Robot（OM）是一个面向机器人的跨平台嵌入式开发框架。它将机器人软件中常见的实时控制、多外设协作、跨任务通信等场景抽象为分层清晰、可移植的软件基础设施，使开发者能够专注于业务逻辑而非底层 plumbing。平台适配通过构建系统选择性编译，不侵入架构核心。

## 设计哲学

**分层原语化。** 框架将软件栈划分为单一功能原语（同步语义、异步调度、数据传输、OS 抽象、平台无关基底、可选算法工具箱），再由领域模块（通用服务、驱动抽象）组合原语形成可复用能力。业务层可以直取任意一层的原语或组合，不受中间层约束——只引入你需要的抽象，不为不需要的抽象付出代价。

**平台无关核心，适配有序聚合。** core 和 algorithm 不依赖任何 OS、外设或板级代码。所有平台适配（RTOS 端口、外设 BSP、工具链绑定、sync 加速后端）统一收敛在 platform 层，由构建系统按目标选择性编译。换一颗芯片或换一个 RTOS 不应触碰到架构核心。

**关注点分离。** sync 只提供执行协调（同步语义），不承载数据；ipc 只提供原始数据传输（字节流或类型化消息），不涉及寻址、路由或业务语义。drivers 与 services 对等互不依赖——驱动层不应依赖日志或通信服务。

**抽象先行，但不隐藏成本。** OSAL 对 RTOS 做最小可移植抽象，PAL 对外设做硬件无关封装。抽象层的目标是消除移植摩擦，而非提供"万能接口"——每一层边界明确，承诺什么、不承诺什么，均有规范约束。

## 文档导航

### 入门

| 文档 | 说明 |
|------|------|
| [快速开始](docs/quick_start.md) | 环境搭建、工具链安装、最小项目构建与调试 |

### 架构

| 文档 | 说明 |
|------|------|
| [架构参考手册](docs/architecture/architecture_reference_manual.md) | 分层结构、各层职责边界、依赖方向——**架构唯一总规范源** |
| [架构决策记录 (ADR)](docs/adr/adr_readme.md) | 已沉淀的架构决策及背景（OSAL 同步契约、CAN 架构、IPC 分层等） |

### 构建

| 文档 | 说明 |
|------|------|
| [构建系统参考手册](docs/build/reference_manual.md) | OM 构建体系完整参考 |
| [构建系统最佳实践](docs/build/build_system_best_practices.md) | XMake 工程实践、脚本域边界、代码组织范式 |
| [构建任务手册](docs/build/build_tasks_manual.md) | 常用构建任务与工作流 |
| [维护手册](docs/build/maintenance_manual.md) | 构建系统维护指南 |

### 研发规范

| 文档 | 说明 |
|------|------|
| [Git 协作规范](docs/process/git_collaboration_spec.md) | 分支职责、提交纪律、PR 流程 |
| [Git 发布与版本规范](docs/process/git_version_release_spec.md) | Tag、版本号、Release 流程 |
| [文档治理规范](docs/process/document_governance_spec.md) | ADR 提纯、Issue 沙盒、版本归档 |

### 模块设计文档

各模块的设计文档和 API 说明随代码分布：

| 模块 | 设计文档 |
|------|----------|
| async | [`lib/async/docs/workqueue.md`](lib/async/docs/workqueue.md) |
| sync | [`lib/sync/docs/completion_design.md`](lib/sync/docs/completion_design.md) |
| ipc | [`lib/ipc/docs/pipe/pipe_design.md`](lib/ipc/docs/pipe/pipe_design.md) |
| drivers | [`lib/drivers/docs/`](lib/drivers/docs/)（device、PAL、GPIO、SPI） |
| peripheral (CAN/串口) | [`lib/drivers/include/drivers/peripheral/`](lib/drivers/include/drivers/peripheral/) |
| motor | [`lib/drivers/include/drivers/motor/vendors/`](lib/drivers/include/drivers/motor/vendors/)（电机厂商适配文档） |
| data_struct | [`lib/data_struct/docs/mpsc/mpsc_design.md`](lib/data_struct/docs/mpsc/mpsc_design.md) |
