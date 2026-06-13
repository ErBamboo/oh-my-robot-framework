# Oh My Robot

面向机器人的跨平台嵌入式开发框架。

[![License](https://img.shields.io/badge/license-Apache%202.0-blue)](LICENSE)
[![XMake](https://img.shields.io/badge/xmake-%E2%89%A53.0.7-orange)](https://xmake.io)

## 项目简介

Oh My Robot（OM）将机器人软件中常见的实时控制、多外设协作、跨任务通信等场景抽象为分层清晰、可移植的软件基础设施。框架的核心哲学是**分层原语化**——将软件栈划分为同步语义、异步调度、数据传输、OS 抽象、可选算法工具箱等独立原语，业务层可以直取任意一层进行组合，只引入你需要的抽象。平台适配统一收敛在 platform 层，由构建系统选择性编译，换芯片或换 RTOS 不触碰架构核心。

## 支持的平台（持续更新中）

| 类型 | 支持项 |
|------|--------|
| **MCU** | STM32F4 系列 |
| **RTOS** | FreeRTOS |
| **工具链** | arm-none-eabi-gcc (gnu-rm)、armclang (Arm Compiler 6) |
| **调试器** | J-Link、DAPLink (OpenOCD) |
| **构建系统** | XMake ≥ 3.0.7 |

## 项目结构

```
oh-my-robot/
├── lib/
│   ├── core/          # 核心：基础类型、错误码、原子操作、数据结构
│   ├── algorithm/     # 算法：PID、滤波等可选计算原语
│   ├── osal/          # OS 抽象层：线程、互斥、信号量、队列、定时器
│   ├── sync/          # 同步层：事件通知、栅栏同步（无数据载荷）
│   ├── async/         # 异步层：工作队列、延时执行
│   ├── ipc/           # 跨上下文数据传输：字节流、类型化消息
│   ├── drivers/       # 驱动与 PAL：设备模型、外设抽象
│   ├── services/      # 通用服务：日志、通信、诊断、文件系统
│   └── systems/       # 业务子系统：底盘、云台、机械臂
├── platform/          # 平台适配：BSP、OSAL 端口、工具链绑定
├── tools/             # 工具与脚本
├── samples/           # 示例代码
├── docs/              # 项目文档
└── build/             # 构建系统（XMake）
```

## 文档

| 文档 | 说明 |
|------|------|
| [快速开始](docs/quick_start.md) | 环境搭建、工具链安装、构建与调试 |
| [架构参考手册](docs/architecture/architecture_reference_manual.md) | 分层结构、职责边界、依赖方向——**架构唯一总规范源** |
| [构建系统参考手册](docs/build/reference_manual.md) | OM 构建体系完整参考 |
| [构建任务手册](docs/build/build_tasks_manual.md) | 常用构建任务与工作流 |

模块设计文档随代码分布在各 `lib/*/docs/` 目录下，包括 async、sync、ipc、drivers、motor 等模块。

## 贡献

提交 PR 前请阅读 [Git 协作规范](docs/process/git_collaboration_spec.md)（分支职责、提交纪律、PR 流程）和 [发布与版本规范](docs/process/git_version_release_spec.md)。

## 反馈与讨论

- **Bug 与功能请求**：[GitHub Issues](https://github.com/oh-my-robot/oh-my-robot-framework/issues)
- **发布记录**：[GitHub Releases](https://github.com/oh-my-robot/oh-my-robot-framework/releases)

## 许可证

[Apache 2.0](LICENSE)
