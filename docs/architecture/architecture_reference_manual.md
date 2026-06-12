# OM 架构参考手册

## 0. 说明

- 本文是 `oh-my-robot` 框架的**架构参考**，描述分层结构、各层职责边界与横切规则。
- **分层与依赖方向**的唯一总规范源为 [`layer_dependency_spec.md`](layer_dependency_spec.md)，本文引用要点但不重复定义依赖矩阵。
- 本文**不包含**：头文件路径、API 清单、类型名、函数签名、include 依赖统计——这些是实现细节，应通过头文件注释或 Doxygen 维护。

## 1. 分层架构总览

```
┌──────────────────────────────────────────────────┐
│                 systems  业务子系统                │
├──────────────────────────────────────────────────┤
│   services（通用服务）   │   drivers（驱动与 PAL）  │
├──────────────────────────────────────────────────┤
│   async（异步）  │  ipc（数传）  │  sync（同步）    │
├──────────────────────────────────────────────────┤
│              osal（操作系统抽象）                   │
├──────────────────────────────────────────────────┤
│              core（基础能力）                       │
╞══════════════════════════════════════════════════╡
│              platform（平台适配）                   │
│   bsp · osal端口 · sync加速 · 工具链/ABI · RTOS绑定  │
├──────────────────────────────────────────────────┤
│              third_party（外部依赖）                │
└──────────────────────────────────────────────────┘
```

> 双线以上为**架构定义**（接口与抽象），双线以下为**平台实现**。`platform` 是所有平台适配代码的有序聚合，配合构建系统选择性编译；`third_party` 为第三方外部代码，仅供适配与移植使用。
>
> **设计思想**：下层为**单一功能原语**（sync 提供同步语义、async 提供执行调度、ipc 提供跨上下文数据传输、osal 抽象操作系统、core 提供基础能力），中层（services、drivers）是原语组合而成的**领域模块**，各有独立语义。业务（systems）可直取任意一层——原语或组合，不受中间层约束。

**依赖方向**：上层依赖下层，可跨层直连。

**禁止依赖方向（示例）**：`drivers → services`、`services → drivers`、`osal → 任何上层`、`platform → services/systems`、`core → 任何上层`。完整依赖矩阵见 [`layer_dependency_spec.md`](layer_dependency_spec.md)。

## 2. 各层职责与边界

双线之下（`platform`、`third_party`）为**平台实现**。`platform` 是所有平台适配代码的有序聚合，配合构建系统选择性编译；`third_party` 为第三方代码，仅供适配与移植使用。双线之上为**架构定义**，按角色分为三组：**单一功能原语组件** → **领域模块** → **业务**。依赖方向自顶向下，可跨层直连。

### 2.1 third_party — 外部依赖

- **职责**：第三方代码（FreeRTOS、CMSIS、HAL 等），仅用于框架的适配与移植，不面向上层模块直接使用。
- **约束**：公共头文件不得直接暴露 third_party 类型/宏；如需对外暴露，必须通过封装或句柄隔离。

### 2.2 platform — 平台适配

- **职责**：所有平台适配代码的有序聚合，配合构建系统选择性编译。涵盖板级初始化与外设配置（bsp）、OSAL 端口实现、sync 加速后端、工具链/ABI 适配、RTOS 绑定。
- **可依赖**：`core`、`third_party`。
- **禁止依赖**：`services`、`systems`。

### 2.3 单一功能原语 — core / osal / sync / async / ipc

这些层各自提供单一、独立的功能原语，不承载业务或领域语义。任何上层模块（drivers、services、systems）均可按需直取任意原语进行组合。

**core — 基础能力层**
- **职责**：基础类型、错误码、通用宏、原子操作、平台无关的数据结构与算法。
- **边界**：不包含 OS、设备驱动、板级或业务语义。不依赖任何上层。
- **禁止依赖**：`osal`、`sync`、`async`、`ipc`、`drivers`、`services`、`systems`、`platform`。

**osal — 操作系统抽象层**

- **职责**：对 RTOS/系统调用做最小可移植抽象（线程、互斥、信号量、队列、时间、定时器、事件对象等）。
- **移植**：osal 自身仅定义接口和跨平台通用逻辑；各 RTOS 的具体端口实现位于 `platform/`，由构建系统按目标选择性编译。
- **规则**：OSAL 只定义"端口必须实现"的最小原语，不承诺更高层语义。端口实现不得混杂在 osal 公共接口中。
- **可依赖**：`core`。

**sync — 同步语义层**

- **职责**：基于 OSAL 原语组合出的纯同步信号抽象（例如事件通知、栅栏同步等），无数据载荷。
- **与 ipc 的区别**：sync 提供同步语义（协调执行顺序），无数据载荷。
- **规则**：对外 API 不暴露具体 RTOS 类型；默认实现仅依赖 OSAL，可选加速实现须满足跨模块复用约束。
- **可依赖**：`core`、`osal`。

**async — 异步执行基座**
- **职责**：在 `osal`/`sync` 之上提供通用执行调度能力（工作队列、延时执行等）。
- **边界**：只提供通用执行语义，不承载业务模块。
- **可依赖**：`core`、`osal`、`sync`。
- **禁止依赖**：`drivers`、`services`、`systems`。

**ipc — 跨上下文数据传输层**

- **职责**：在不同执行上下文之间提供原始数据传输能力。传输原语只有两类——**无结构字节流**和**类型化离散消息**，不涉及寻址、路由、帧协议或业务语义。跨上下文场景（Task↔Task / ISR↔Task 等）通过 API 变体支持，不作为独立通信方模块。
- **与 sync 的区别**：ipc 传输数据载荷，sync 提供同步语义，无数据载荷。
- **与 services/comm 的区别**：ipc 提供原始传输原语（字节流或类型化元素），comm 在此基础上增加帧格式与路由，形成结构化消息。
- **范围**：单核内跨上下文传输。核间通信为 future scope。
- **可依赖**：`core`、`osal`。
- **禁止依赖**：`services`、`drivers`、`systems`。

### 2.4 领域模块 — drivers / services

基于下层原语组合而成的领域模块，各有独立语义。二者对等、互不依赖，均可直取任意原语层。

**drivers — 驱动与 PAL**
- **职责**：设备模型、外设驱动、平台适配层（PAL），面向可复用/可移植的硬件抽象。
- **边界**：保持硬件无关抽象，板级差异通过 PAL 接口交由 `platform` 处理。
- **可依赖**：`core`、`osal`、`sync`、`async`。
- **禁止依赖**：`platform`（任意代码）、`services` 核心路径、`systems`。
- **规则**：当把具体总线实现接入 `services/comm` 时，须通过实现侧 adapter 模式解耦（`services/comm` 核心不依赖 adapter，`drivers` 核心不反向依赖 adapter）。

**services — 通用服务层**
- **职责**：可复用的通用服务组件（日志、配置、通信、诊断、文件系统等）。
- **边界**：服务语义必须保持项目无关，不得绑定具体机器人机构或业务。
- **可依赖**：`core`、`osal`、`sync`、`ipc`。
- **禁止依赖**：`drivers` 核心路径（实现侧 adapter 解耦除外）、`systems`。

### 2.5 业务 — systems

**systems — 业务子系统层**

- **职责**：机器人系统级模块（chassis、gimbal、arm、robot 等），业务语义明确。可位于 `oh-my-robot` 仓库内，也可位于独立领域仓库。
- **可依赖**：`services`、`drivers`、`sync`、`ipc`、`osal`、`core` — 即业务可直取领域模块或任意原语，不受中间层约束。
- **说明**：可直接依赖 `drivers`（驱动层视为硬件无关抽象），必要时可绕过 `services`。

## 3. 横切规则

### 3.1 命名约定

| 元素 | 约定 | 示例 |
|------|------|------|
| 结构体实体类型 | `_s` 后缀 | `Device_s` |
| 句柄/指针/抽象别名 | `_t` 后缀 | `Device_t` |
| 枚举类型 | `_e` 后缀 | `OmRet_e` |
| 联合类型 | `_u` 后缀 | — |
| 类型名 | `CamelCase` | `HalCanHandler` |
| 成员变量 | `camelBack` | `rxBuffer` |
| 函数名、变量、参数 | `snake_case` | `device_init()` |
| 宏、枚举常量 | `UPPER_CASE` | `DEV_STATUS_OK` |

### 3.2 include 边界规则

- 公共头文件仅能 include 本层或下层公开头；不得 include `third_party` 类型或私有实现头。
- 实现文件可 include 同层私有头，但不得穿透到上层目录。
- 同层之间不得形成环依赖；必要时拆分子层或抽象接口。
- 领域仓库与项目仓库不得直接 include `platform/` 私有实现。
- 对外入口可包含聚合头（如 `omlib.h`、`osal/osal.h`），框架内部实现应优先包含最小必需头文件。

### 3.3 聚合目标约定

- `tar_oh_my_robot`：框架聚合目标，只传播底座层能力（core / osal / sync / ipc / drivers / third_party），不包含 `systems` 或领域业务模块。
- `tar_om_full`：完整聚合目标，在 `tar_oh_my_robot` 基础上包含 services / systems。
- 领域仓库应自行提供独立聚合目标，由项目仓库在顶层显式组合。

## 附录 A：架构决策索引

所有架构决策记录（ADR）存放于 [`docs/adr/`](../adr/adr_readme.md)。

| 编号 | 主题 | 涉及层级 | 状态 |
|------|------|---------|------|
| 0001 | OSAL 与 SYNC 的职责语义边界 | osal, sync | 已实施 |
| 0002 | 电机统一模型顶层设计 | drivers | 部分实施 |
| 0003 | 电机 Vendor Adapter 边界 | drivers | 部分实施 |
| 0004 | 电机语义合同编号体系 | drivers | 已建立 |
| 0005 | CAN 组织架构分层 | drivers | 已实施 |
| 0006 | CAN 接收路径建模选型 | drivers | 已实施 |
| 0007 | 串口通信适配器分层边界 | drivers, services | 已实施 |
| 0008 | 诊断守护服务语义冻结 | services | 未实施 |

## 附录 B：相关文档

| 文档 | 说明 |
|------|------|
| [`layer_dependency_spec.md`](layer_dependency_spec.md) | 分层与依赖方向的**唯一总规范源** |
| [`docs/process/document_governance_spec.md`](../process/document_governance_spec.md) | 文档治理规范（ADR 提纯与归档流程） |
| [`docs/process/git_collaboration_spec.md`](../process/git_collaboration_spec.md) | Git 协作规范（分支架构与 SOP） |
| [`docs/process/git_version_release_spec.md`](../process/git_version_release_spec.md) | 发布与版本控制规范 |
