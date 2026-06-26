# OM 框架移植向导

本文档说明将 oh-my-robot-framework 移植到新 MCU / 新 RTOS 时，必须提供的配置入口及其职责。

## 配置入口全景

```
include 顺序:  om_port_compiler.h → om_port_hw.h → om_osal_portdef.h → om_config.h
                           │              │                │                │
                        编译器检测      中断原语        OS 强制参数      功能裁剪
```

| 入口 | 谁写 | 性质 | 位置 | 触发方式 |
|---|---|---|---|---|
| `om_osal_portdef.h` | OS 端口适配者 | **强制** | `platform/osal/<os>/` | `osal_config.h` 直接 `#include` |
| `om_bsp_portdef.h` | BSP 作者 | **预留**（当前空） | `platform/bsp/boards/<board>/` | 待定 |
| `om_appcfg.h` | 应用作者 | **可选** | 用户工程目录 | 定义 `OM_USE_APPCFG` 后，`om_config.h` 引入 |

## 1. `om_osal_portdef.h` — OS 端口强制定义

### 何时需要

每次将框架适配到新的 RTOS（或更换 RTOS 版本）时，OS 端口适配者必须在对应 `platform/osal/<os>/` 目录下创建此文件。

### 必须提供的内容

以 FreeRTOS 为例：

```c
// platform/osal/freertos/om_osal_portdef.h

#ifndef OM_OSAL_PORTDEF_H
#define OM_OSAL_PORTDEF_H

/* Event Flags 可用位宽 — FreeRTOS 使用 uint32_t 的低 24 位 */
#ifndef OM_OSAL_EVENT_FLAGS_USABLE_MASK
#define OM_OSAL_EVENT_FLAGS_USABLE_MASK   0x00FFFFFF
#endif

/* 对齐 FreeRTOSConfig.h 的默认值 — 若项目的 config 不同，通过 -D 覆写 */
#ifndef OM_OSAL_PRIORITY_MAX
#define OM_OSAL_PRIORITY_MAX              32u
#endif

#ifndef OM_OSAL_TASK_NAME_MAX
#define OM_OSAL_TASK_NAME_MAX             16u
#endif

#endif
```

### 工作机制

```
osal_config.h:
  #include "om_osal_portdef.h"   ← 若文件不存在 → 编译错误

osal_event.h:
  #ifndef OM_OSAL_EVENT_FLAGS_USABLE_MASK
  #error "..."                   ← 端口定义文件存在但值缺失 → 编译错误
  #endif
```

- **xmake 构建**：构建系统自动将 `platform/osal/<os>/` 加入 include path
- **手动工程**（CCS/IAR 等）：需手动将该目录加入 include path
- 值可通过 `-D` 编译选项覆盖（`#ifndef` 保护）

### 如何验证

配置完成后，编译 OSAL 层目标。若通过，说明所有强制参数已就位。

## 2. `om_bsp_portdef.h` — BSP 端口定义（预留）

当前版本 BSP 级无强制参数。`om_port_hw.h` 已覆盖中断原语。本文件为未来扩展预留（如 Flash 起始地址、内存布局差异等）。

## 3. `om_appcfg.h` — 应用配置覆写

### 何时使用

当 `om_config.h` 提供的默认功能裁剪（`OM_USE_*`）或优化开关（`OM_SYNC_ACCEL`）需要调整时，应用作者在自己的工程目录下创建此文件。

### 示例

```c
// <project>/om_appcfg.h

/* 关闭不需要的 HAL */
#undef OM_USE_HAL_CAN

/* 开启 sync 加速后端 */
#define OM_SYNC_ACCEL 1

/* 自定义日志级别 */
#define OM_DEFAULT_LOG_LEVEL  OM_LOG_LEVEL_WARN
```

### 工作机制

```c
// om_config.h 末尾:
#ifdef OM_USE_APPCFG
#include "om_appcfg.h"
#endif
```

- 默认不启用（未定义 `OM_USE_APPCFG` 时 `om_config.h` 不做任何额外 include）
- 启用方式：编译选项中添加 `-DOM_USE_APPCFG`，或将 `#define OM_USE_APPCFG` 放在 `#include "core/om_config.h"` 之前
- `om_appcfg.h` 的 include path 需由用户自行保证

## 移植检查清单

- [ ] `platform/osal/<os>/om_osal_portdef.h` 已创建且包含所有强制参数
- [ ] `om_osal_portdef.h` 所在目录已在 include path 中
- [ ] `osal_config.h` 编译通过（确认 `#include "om_osal_portdef.h"` 找到了文件）
- [ ] FreeRTOSConfig.h 中的值与 `om_osal_portdef.h` 中的值一致（或通过 `-D` 覆写）
- [ ] 编译 OSAL + sync 目标，无 error
