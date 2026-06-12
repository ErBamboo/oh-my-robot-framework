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
├──────────────────────────────────────────────────┤
│   platform（平台适配）   │    bsp（板级支持）       │
├──────────────────────────────────────────────────┤
│              third_party（外部依赖）                │
└──────────────────────────────────────────────────┘
```

> **设计思想**：下层为**单一功能原语**（sync 提供同步语义、async 提供执行调度、ipc 提供跨上下文数据传输、osal 抽象操作系统、core 提供基础能力），中层（services、drivers）是原语组合而成的**领域模块**，各有独立语义。业务（systems）可直取任意一层——原语或组合，不受中间层约束。

**依赖方向**：上层依赖下层，可跨层直连。

**禁止依赖方向（示例）**：`drivers → services`、`services → drivers`、`osal → 任何上层`、`bsp → osal/sync/ipc`、`core → 任何上层`。完整依赖矩阵见 [`layer_dependency_spec.md`](layer_dependency_spec.md)。

## 2. 各层职责与边界

### 2.1 third_party — 外部依赖

- **职责**：第三方代码（FreeRTOS、CMSIS、HAL 等）。
- **规则**：禁止业务逻辑直接修改；如需扩展，优先在 `platform/` 或 `drivers/` 做封装。
- **约束**：公共头文件不得直接暴露 third_party 类型/宏；如需对外暴露，必须通过封装或句柄隔离。

### 2.2 bsp — 板级支持

- **职责**：芯片板卡初始化、外设底层配置、启动文件、时钟与引脚物理连接。与具体 MCU 强耦合。
- **边界**：不承担 OSAL 端口或业务语义。
- **可依赖**：`core`、`drivers`（仅通过 PAL 接口）、`third_party`。
- **禁止依赖**：`osal`、`sync`、`ipc`、`services`、`systems`。

### 2.3 platform — 端口与平台适配

- **职责**：OSAL 端口实现、sync 加速后端、工具链/ABI 适配、RTOS 绑定。
- **边界**：不包含板级初始化与外设物理连接。
- **可依赖**：`core`、`third_party`（通过平台内接口层）。
- **禁止依赖**：`services`、`systems`。

### 2.4 core — 基础能力层

- **职责**：基础类型、错误码、通用宏、原子操作、平台无关的数据结构与算法。
- **边界**：不包含 OS、设备驱动、板级或业务语义。
- **可依赖**：必要的 `third_party`（需封装在实现或内部头）。
- **禁止依赖**：`osal`、`sync`、`async`、`ipc`、`drivers`、`services`、`systems`、`bsp`、`platform`。

### 2.5 osal — 操作系统抽象层

- **职责**：对 RTOS/系统调用做最小可移植抽象（线程、互斥、信号量、队列、时间、定时器、事件对象等）。
- **规则**：OSAL 只定义"端口必须实现"的最小原语，不承诺更高层语义。端口实现放在 `platform/` 中，避免公共接口混杂平台细节。
- **可依赖**：`core`。

### 2.6 sync — 同步语义层

- **职责**：基于 OSAL 原语组合出的纯同步信号抽象（例如事件通知、栅栏同步等），无数据载荷。
- **与 ipc 的区别**：sync 只传递"事件信号"，不传递数据。
- **规则**：对外 API 不暴露具体 RTOS 类型；默认实现仅依赖 OSAL，可选加速实现须满足跨模块复用约束。
- **可依赖**：`core`、`osal`。

### 2.7 ipc — 跨上下文数据传输层

- **职责**：跨上下文（Task↔Task / ISR→Task / Task→ISR）的字节流/消息传输通道，有数据载荷。
- **与 sync 的区别**：ipc 传输带数据的通道，sync 只传递事件信号。
- **与 services/comm 的区别**：ipc 提供无结构的字节流，comm 提供带帧格式和路由的结构化消息。
- **可依赖**：`core`、`osal`。
- **禁止依赖**：`services`、`drivers`、`systems`。

### 2.8 async — 异步执行基座

- **职责**：在 `osal`/`sync` 之上提供通用执行调度能力（工作队列、延时执行等）。
- **边界**：只提供通用执行语义，不承载业务模块。
- **可依赖**：`core`、`osal`、`sync`。
- **禁止依赖**：`drivers`、`services`、`systems`。

### 2.9 drivers — 驱动与 PAL

- **职责**：设备模型、外设驱动、平台适配层（PAL），面向可复用/可移植的硬件抽象。
- **边界**：保持硬件无关抽象，板级差异通过 PAL 接口交由 `bsp`/`platform` 处理。
- **可依赖**：`core`、`osal`、`sync`、`ipc`、必要的 `third_party`（尽量通过 BSP 或 port 封装）。
- **禁止依赖**：`services` 核心路径、`systems`。
- **规则**：禁止直接 include `bsp/` 私有头文件。当把具体总线实现接入 `services/comm` 时，须通过实现侧 adapter 模式解耦（`services/comm` 核心不依赖 adapter，`drivers` 核心不反向依赖 adapter）。

### 2.10 services — 通用服务层

- **职责**：可复用的通用服务组件（日志、配置、通信、诊断、文件系统等）。
- **边界**：服务语义必须保持项目无关，不得绑定具体机器人机构或裁判业务。
- **可依赖**：`core`、`osal`、`sync`、`ipc`。
- **禁止依赖**：`drivers` 核心路径（实现侧 adapter 解耦除外）、`systems`。

### 2.11 systems — 业务子系统层

- **职责**：机器人系统级模块（chassis、gimbal、arm、robot 等），业务语义明确。可位于 `oh-my-robot` 仓库内，也可位于独立领域仓库。
- **可依赖**：`services`、`drivers`、`sync`、`ipc`、`osal`、`core`。
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
- 领域仓库与项目仓库不得直接 include `platform/`、`bsp/` 私有实现头。
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
