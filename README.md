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

## 快速开始

### 环境要求

- `arm-none-eabi-gcc` 或 `armclang`
- XMake ≥ 3.0.7
- （可选）J-Link 或 OpenOCD + DAPLink 探针，用于烧录和调试

### 最小项目

```lua
-- xmake.lua
set_project("my-robot")
set_xmakever("3.0.7")
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = os.projectdir()})

includes("oh-my-robot")

target("robot_project")
    set_kind("binary")
    set_filename("robot_project.elf")
    add_deps("tar_oh_my_robot")
    add_rules("oh_my_robot.context", "oh_my_robot.board_assets", "oh_my_robot.image_convert")
    add_files("main.c")
target_end()
```

```bash
xmake f -c --toolchain=gnu-rm -m debug
xmake
```

详细的环境搭建、`om_preset.lua` 配置和 VSCode 调试步骤见 [快速开始指南](docs/quick_start.md)。

## 文档

| 文档 | 说明 |
|------|------|
| [架构参考手册](docs/architecture/architecture_reference_manual.md) | 分层结构、职责边界、依赖方向——**架构唯一总规范源** |
| [构建系统参考手册](docs/build/reference_manual.md) | OM 构建体系完整参考 |
| [构建任务手册](docs/build/build_tasks_manual.md) | 常用构建任务与工作流 |

模块设计文档随代码分布在各 `lib/*/docs/` 目录下，包括 async、sync、ipc、drivers、motor 等模块。

## 贡献

提交 PR 前请阅读 [Git 协作规范](docs/process/git_collaboration_spec.md)（分支职责、提交纪律、PR 流程）和 [发布与版本规范](docs/process/git_version_release_spec.md)。（这些撰写时是面向AI Agents的）

## 许可证

[Apache 2.0](LICENSE)
