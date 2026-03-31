# OM 分层与仓库边界规范（长期稳定）
本文用于明确 `oh-my-robot` 的正式职责边界与依赖方向。当前长期目标是把 `oh-my-robot` 固定为“跨项目底座”，只承载平台、OS 与设备抽象能力，不再承载机器人业务子系统。

## 规范来源约束（强制）
- 本文档 [`oh-my-robot/docs/architecture/layer_dependency_spec.md`](layer_dependency_spec.md) 是全仓“框架层边界与依赖方向”的唯一总规范源。
- 其他文档（OSAL/SYNC、services、drivers、build）只能做专项补充，必须引用本文，不得重新定义总依赖矩阵。

## 仓库分层
### 1) `third_party/`（外部依赖）
- 含义：第三方代码（FreeRTOS、CMSIS、HAL 等）。
- 规则：禁止业务逻辑直接修改；如需扩展，优先在 `platform/` 或 `drivers/` 做封装。
- 约束：公共头文件不得直接暴露 third_party 类型/宏；如需对外暴露，必须通过封装或句柄隔离。

### 2) `platform/`（端口与平台适配）
- 含义：OSAL 端口、工具链与 ABI 适配、RTOS 绑定。
- 边界：不包含板级初始化与业务语义。
- 可依赖：`core`、`third_party`。
- 禁止依赖：`services`、`drivers`、外部领域仓库。

### 3) `platform/bsp/`（板级支持）
- 含义：板卡、芯片、启动文件、时钟与外设物理连接。
- 边界：只处理与具体 MCU/板卡强耦合的初始化和板级构建输入。
- 可依赖：`core`、`drivers`（仅通过 PAL/Device 公共接口）、`third_party`。
- 禁止依赖：`osal`、`sync`、`services`、外部领域仓库。

### 4) `lib/include/core` + `lib/source/core`（基础能力层）
- 含义：基础类型、错误码、通用宏、平台无关轻量算法与数据结构。
- 边界：不包含 OS、设备驱动、板级或业务语义。
- 可依赖：必要的 `third_party`（需封装在实现或内部头）。
- 禁止依赖：`osal`、`sync`、`async`、`drivers`、`services`、`bsp`、`platform`、外部领域仓库。

### 5) `lib/osal/`（操作系统抽象层）
- 含义：线程、互斥、信号量、队列、时间、定时器、事件对象等最小原语。
- 规则：
  - OSAL 只定义“端口必须实现”的最小集合。
  - OSAL 不承诺更高层业务语义。
- 可依赖：`core`。
- 说明：端口实现必须放在 `platform/` 中，避免 OSAL 公共接口混杂平台细节。

### 6) `lib/sync/`（同步语义层）
- 含义：基于 OSAL 原语组合出的同步抽象（例如 completion）。
- 规则：
  - 对外 API 不暴露具体 RTOS 类型。
  - 默认实现仅依赖 OSAL。
  - 可选加速实现必须满足跨模块复用约束。

### 7) `lib/async/`（异步执行基座）
- 含义：在 `osal/sync` 之上提供通用执行器、工作队列等可复用运行时能力。
- 边界：只提供通用执行语义，不承载机器人业务模块。
- 可依赖：`core`、`osal`、`sync`。
- 禁止依赖：`drivers`、`services`、外部领域仓库。

### 8) `lib/services/`（通用服务层）
- 含义：可复用服务组件（例如 log、config、comm、diagnostics、fs）。
- 边界：服务语义必须保持项目无关，不得绑定具体机器人机构或裁判业务。
- 可依赖：`core`、`osal`、`sync`、`async`。
- 禁止直接依赖：`drivers` 核心路径（实现侧适配除外）、外部领域仓库。

### 9) `lib/drivers/`（驱动与 PAL）
- 含义：设备模型、外设驱动、PAL、通用执行器/传感器抽象。
- 边界：保持硬件无关抽象；板级差异通过 PAL 接口交由 `bsp`/`platform` 处理。
- 可依赖：`core`、`osal`、`sync`、必要的 `third_party`（尽量通过 BSP 或 port 封装）。
- 禁止依赖：`services` 核心路径、外部领域仓库。
- 规则：禁止直接 include `bsp/` 私有头文件。

### 10) 外部 `domain repo`（领域应用仓库，仓库外）
- 含义：机器人业务子系统与机构编排，例如 `chassis`、`gimbal`、`arm`、`referee`、`supercap`、整机状态机。
- 说明：该层不属于 `oh-my-robot` 正式边界，应放在独立仓库（例如 `omr-robotics`）中，仅通过 `oh-my-robot` 的公开头文件与聚合目标接入。

### 11) 外部 `project repo`（具体项目仓库，仓库外）
- 含义：项目入口、参数表、设备 ID、标定值、任务装配、比赛/产品策略、最终镜像目标。
- 说明：项目仓库负责聚合 `oh-my-robot` 与领域仓库，不反向把业务模块回灌到框架仓库。

## 迁移过渡约束
- `lib/systems/` 已从当前仓库移除，不再保留任何残留文件。
- 首批 `arm`、`chassis`、`gimbal`、`robot`、`supercap` 接口与占位实现已开始迁往 `omr-robotics`。
- 后续不得在 `oh-my-robot` 内重新引入 `lib/systems/` 或同类业务层目录。
- 任何新的机器人业务模块必须直接进入外部领域仓库。

## 依赖方向（强约束）
允许依赖方向：
- `services` → `async` → `sync` → `osal` → `core` → `third_party`
- `services` → `sync` → `osal` → `core` → `third_party`
- `drivers` → `sync` → `osal` → `core` → `third_party`
- `bsp` → `drivers/core/third_party`
- `platform` → `core/third_party`
- `domain repo` → `services/drivers/async/sync/osal/core`
- `project repo` → `domain repo` + `oh-my-robot`

禁止依赖方向（示例）：
- `drivers(core)` → `services`
- `services(core)` → `drivers`
- `osal` → `services/drivers/domain repo`
- `bsp` → `osal/sync/services/domain repo`
- `core` → 任何上层模块
- `oh-my-robot` 内任何正式模块 → `domain repo` / `project repo`

## 依赖与 include 边界规则
- 公共头文件仅能 include 本层或下层公开头；不得 include `third_party` 类型或私有实现头。
- 实现文件可 include 同层私有头，但不得穿透到上层目录。
- 同层之间不得形成环依赖；必要时拆分子层或抽象接口。
- 领域仓库与项目仓库不得直接 include `platform/`、`bsp/` 私有实现头。

## 聚合目标约定
- `tar_oh_my_robot`：`oh-my-robot` 的正式聚合目标，只传播底座层能力，不包含 `systems` 或任何领域业务模块。
- 领域仓库应自行提供独立聚合目标（例如 `tar_omr_robotics`），由项目仓库在顶层显式组合。

## 落地检查清单（Review 用）
- `tar_oh_my_robot` 是否仍传播 `systems` 目标或头文件。
- 是否出现 `drivers(core)` include `services/...`。
- 是否出现 `services(core)` include `drivers/...`。
- 是否出现 `bsp` include `osal/...` 或 `sync/...`。
- 公共头文件是否暴露 third_party 类型/宏。
- `platform` 是否避免依赖上层业务。
- `core` 是否仅包含基础能力，未引入 OS/驱动/业务语义。
- 新增机器人业务模块是否仍试图回灌到框架仓库。

## 头文件聚合入口规范
为降低上层使用成本，可保留聚合头作为对外入口，但需明确边界：
1. 对外入口（领域层/项目层/样例/测试）可包含聚合头（如 `omlib.h`、`osal/osal.h`）。
2. 框架内部实现应优先包含最小必需头文件，避免依赖聚合头形成隐式耦合。
3. 聚合头内容应受控扩展，不得成为“全局大头文件”。
4. 若出现 clangd `unused-includes` 提示，内部实现应按最小依赖原则清理；对外入口可忽略提示。
