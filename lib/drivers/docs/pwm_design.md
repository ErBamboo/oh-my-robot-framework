# Oh My Robot PWM 子系统架构设计

> 版本：v1.0
> 日期：2026-07-03
> 基于：Linux kernel pwm.h / Zephyr pwm.h / RT-Thread rt\_drv\_pwm.h / ESP-IDF mcpwm.h
> 项目约束：Device 模型、OsalIrqLock、静态分配

***

## 1. 架构分层

```
+=======================================================================+
|  Application                                                          |
|  e.g. led.c, servo.c, motor_foc.c                                     |
|  Operates through: pwm_channel_config / enable / disable / set_pulse  |
|                    pwm_channel_get_state / pwm_channel_get_capability |
+=======================================================================+
           |                              |
           | Direct typed API             | Device API (system integration)
           | (channel-level ops)          | (controller lifecycle)
           v                              v
+=====================================================================+
|  PwmChannel (Lightweight Handle — NOT a Device)                     |
|  - ctrl: PwmController*   (resolved once at pwm_channel_get)        |
|  - channel: uint8_t       (0-based index)                           |
|  Size: 12 bytes. Passed by value. ISR-friendly.                     |
+=====================================================================+
           |
           | points to
           v
+====================================================================+
|  PwmController (IS a Device — registered via device_register)       |
|  - parent: Device          (global device list, device_find)        |
|  - ops: PwmOps*            (BSP function table)                     |
|  - cap: PwmCapability*     (channels, resolution, supported features)|
|  - chState: PwmChannelState*  (per-channel state, BSP provides array)|
|  Responsibilities: chState management, ns↔cycles conversion,        |
|                    enable/disable idempotency, ISR path cache        |
+====================================================================+
           |
           | PwmOps function pointer table (4 required callbacks)
           v
+====================================================================+
|  PwmOps (BSP Hardware Abstraction — platform-independent)           |
|  - channelConfig(ctrl, ch, periodCycles, pulseCycles, polarity)     |
|  - channelEnable(ctrl, ch)                                          |
|  - channelDisable(ctrl, ch)                                         |
|  - channelSetPulse(ctrl, ch, pulseCycles)    [ISR-safe]             |
+====================================================================+
           |
           | BSP implementation
           v
+====================================================================+
|  BSP Private (Platform-specific)                                    |
|  e.g. BspPwm { TIM_HandleTypeDef tim; PwmController parent; ... }   |
|  Pin alternate function and clock tree config in BSP init           |
|  Controller-level output enable configured once in registration     |
|  Timer base initialized once, not per channel configuration         |
+====================================================================+
           |
           v
+====================================================================+
|  MCU Hardware (timer peripheral + pin alternate function + clock)   |
+====================================================================+
```

***

## 2. 对象关系图解

### 2.1 结构关系：所有权与引用

```
                         ┌─────────────────────────┐
                         │     Device 注册表         │
                         │   device_find("pwm1")    │
                         └────────────┬────────────┘
                                      │ 按名称查找
                                      ▼
┌─────────────────────────────────────────────────────────────────┐
│                        BSP (Static)                              │
│                                                                  │
│  struct BspPwm {                                                 │
│    TIM_HandleTypeDef tim;     ←── BSP 硬件句柄                   │
│    PwmController     parent;  ←── 嵌入框架控制器                  │
│    PwmChannelState   chState[4]; ───────────────────────┐        │
│  };                                                      │        │
│                                                          │        │
│  ┌───────────────────────────────────────────────────┐   │        │
│  │ PwmController (IS a Device)                        │   │        │
│  │                                                    │   │        │
│  │  parent: Device  ←── 设备链表节点                   │   │        │
│  │  ops: PwmOps*    ────────────→ 4 个 BSP 回调        │   │        │
│  │  cap: PwmCapability* ────────→ 只读能力声明          │   │        │
│  │  chState: PwmChannelState* ──→ chState[0] ──────────┼───┘        │
│  │                               chState[1]  (per-channel 状态)     │
│  │                               chState[2]                         │
│  │                               chState[3]                         │
│  └───────────────────────┬───────────────────────────┘              │
└──────────────────────────┼──────────────────────────────────────────┘
                           │
              PwmChannelSpec { "pwm1", 0 }
                  │
                  │ pwm_channel_get()
                  ▼
         PwmChannel { ctrl → &parent, channel = 0 }
              │
              │ 传递到所有通道级 API
              ▼
    pwm_channel_config / enable / disable / set_pulse / get_state
```

### 2.2 PwmChannel 为何不是 Device

硬件约束：多数 MCU 的 PWM 外设中，同一控制器的所有通道共享时基单元（周期/预分频器）。如果每个通道都是独立 Device，配置 ch0 的周期会静默覆盖 ch3——违反 Device 模型的独立性假设。控制器作为 Device、通道作为轻量句柄的设计直接反映了这一硬件现实。

***

## 3. 数据结构

### 3.1 PwmChannelState — per-channel 状态（框架是唯一真相源）

```c
typedef struct {
    uint32_t    periodNs;       // 已配置的周期 (ns)，0 = 从未 config
    uint32_t    periodCycles;   // 硬件周期 ticks（ISR 快速路径使用）
    uint32_t    pulseNs;        // 最近一次 config 的脉宽 (ns)
    PwmPolarity polarity;       // 当前输出极性
    bool        enabled;        // 是否正在输出（框架级幂等）
} PwmChannelState;
```

**设计要点**：

- `periodNs == 0` 作为"未配置"哨兵值（注册时初始化为 0）
- `periodCycles` 与 `periodNs` 配对，为 `set_pulse` 提供 ns→cycles 比例
- `enabled` 由框架管理，`channelEnable/Disable` 幂等性不依赖 BSP
- 字节对齐后约 20 bytes/channel

### 3.2 PwmChannelSpec / PwmChannel — 双类型句柄

```c
// 编译时声明（static const，零运行时开销）
typedef struct {
    const char *controller;  // 如 "pwm1"
    uint8_t     channel;     // 通道号 (0-based)
} PwmChannelSpec;

// 运行时句柄（12 bytes，按值传递，ISR 安全）
typedef struct {
    PwmController *ctrl;     // 已解析的控制器指针
    uint8_t        channel;  // 通道号
} PwmChannel;
```

编译时声明 + 运行时解析：`PwmChannelSpec` 可声明为 `static const`，零运行时构建开销。`pwm_channel_get()` 调用 `device_find()` 解析 Spec → Handle，后续操作零字符串查找。

### 3.3 PwmCapability — 能力声明

```c
typedef struct {
    uint8_t  numChannels;   // 通道数 (注册时确定)
    uint32_t minPeriodNs;   // 最小周期 → 最高频率
    uint32_t maxPeriodNs;   // 最大周期 → 最低频率
    uint32_t resolutionHz;  // 计数器时钟 (ns→cycles 转换因子)
    uint32_t caps;          // 能力位图 (PWM_CAP_POLARITY_* 等)
    uint8_t  counterWidth;  // 计数器位宽 (16/32)
} PwmCapability;
```

独立于 PwmController，多个同型号控制器可共享同一 `PwmCapability` 实例。

### 3.4 PwmOps — BSP 硬件操作函数表

```c
struct PwmOps {
    OmRet (*channelConfig)(PwmController *ctrl, uint8_t channel,
                            uint32_t periodCycles, uint32_t pulseCycles,
                            PwmPolarity polarity);
    OmRet (*channelEnable)(PwmController *ctrl, uint8_t channel);
    OmRet (*channelDisable)(PwmController *ctrl, uint8_t channel);
    OmRet (*channelSetPulse)(PwmController *ctrl, uint8_t channel,
                              uint32_t pulseCycles);     // ISR-safe
};
```

4 个回调，全部必须实现。`channelSetPulse` 标记 ISR 安全——内部只做寄存器写入，不阻塞、不调用 HAL 函数。

### 3.5 PwmController — 控制器（IS a Device）

```c
struct PwmController {
    Device              parent;     // 内嵌 Device，注册到全局链表
    const PwmOps       *ops;        // BSP 函数表
    const PwmCapability *cap;       // 能力声明 (只读)
    PwmChannelState    *chState;    // per-channel 状态数组 (BSP 提供存储)
};
```

5 个字段，无冗余。BSP 通过 `ctrl->parent.handle` 访问私有数据。

***

## 4. API 函数签名

### 4.1 通道解析与状态查询

```c
OmRet pwm_channel_get(const PwmChannelSpec *spec, PwmChannel *ch);
const PwmChannelState *pwm_channel_get_state(PwmChannel ch);
const PwmCapability *pwm_channel_get_capability(PwmChannel ch);
```

### 4.2 通道配置与启停

```c
OmRet pwm_channel_config(PwmChannel ch, const PwmChannelConfig *cfg);
OmRet pwm_channel_enable(PwmChannel ch);   // 框架级幂等
OmRet pwm_channel_disable(PwmChannel ch);  // 框架级幂等
```

### 4.3 运行时脉宽更新

```c
OmRet pwm_channel_set_pulse(PwmChannel ch, uint32_t pulse_ns);  // ISR-safe
```

### 4.4 BSP 注册

```c
OmRet pwm_controller_register(PwmController *ctrl, const char *name,
                               const PwmCapability *cap,
                               const PwmOps *ops, void *priv,
                               PwmChannelState *chState);
```

### 4.5 Device 接口（系统集成）

```c
DevInterface:
   init    = pwm_dev_init     — 验证 chState 已就绪
   open    = pwm_dev_open     — 声明控制器使用权
   close   = pwm_dev_close    — 停止全部通道 (osal_irq_lock 保护)
   read    = NULL             — PWM 非数据流外设
   write   = NULL             — 使用 pwm_channel_set_pulse 直接 API
   control = pwm_dev_control  — SUSPEND / RESUME / GET_CAPABILITY
```

***

## 5. 关键设计决策

### D1: PwmController 作为 Device，PwmChannel 不作为 Device

| 决策               | 理由                                                         |
| ---------------- | ---------------------------------------------------------- |
| PwmChannel 是轻量句柄 | 通道共享控制器时基（周期/预分频器），不是独立物理资源。Device 模型会假造独立性。   |
| PwmController 是 Device | 控制器是独立物理外设，有 init/open/close 生命周期，适合 Device 模型。 |
| ISR 性能           | 值传递 12 bytes，零间接调用。                                        |

### D2: 框架持有 per-channel 状态 — 对标 Linux pwm\_device.state

| 决策                    | 理由                                          |
| --------------------- | ------------------------------------------- |
| chState\[] 归框架管理      | 框架是真相源——enable 幂等、pulse 校验、get\_state 都靠它   |
| BSP 仅提供存储             | BSP 声明 `PwmChannelState chState[4]`，内容由框架维护 |
| periodNs==0 作为"未配置"哨兵 | 零开销语义，不需要额外 bool 字段                         |

### D3: 三级 config 与 HAL\_TIM\_PWM\_Init 的一次性调用

| 层级                   | 操作                                  | 何时执行  |
| -------------------- | ----------------------------------- | ----- |
| bsp\_pwm\_register() | HAL\_TIM\_PWM\_Init（一次性时基初始化）       | 启动时一次 |
| channelConfig()      | 直接写 周期/预分频/比较 寄存器（无 TIM\_EGR\_UG） | 每次配置  |
| channelEnable()      | HAL\_TIM\_PWM\_Start（通道输出使能 + 计数器使能）    | 每次启动  |

`时基初始化接口` 内部写 `全局更新事件` 会复位所有通道的计数器。在运行时调用会在其他通道上产生毛刺。因此移到 register 阶段一次性执行。

### D4: config + enable 分离 — 对标 厂商 HAL 三步模型

| 决策                       | 理由                                 |
| ------------------------ | ---------------------------------- |
| 分离 config 和 enable       | 允许多通道先配好再统一启动，避免中间态毛刺              |
| 对标硬件原生三步模型 | 多数 MCU 支持 Init→Config→Start 三步分离，硬件预装载保证无毛刺  |
| 区别于 Zephyr 的 set\_cycles | Zephyr 每次设周期+脉宽一步到位，不适合我们"先配后启"的用例 |

### D5: channelSetPulse 是 ISR 安全的

| 决策          | 理由                                          |
| ----------- | ------------------------------------------- |
| 直接写比较寄存器 | 硬件比较寄存器写入是单条 MMIO store（或等效操作）  |
| 预计算 timerHz | BSP 在 register 时计算一次，存入 BspPwm.timerHz      |
| 无 HAL 调用    | ISR 路径零 `HAL_RCC_GetXXXFreq()` 调用           |
| 框架层无阻塞      | `pulse_ns * cachedCycles / cachedNs` 是纯整数运算 |

### D6: BDTR 和 MOE 在 register 阶段一次性配置

部分硬件有控制器级的全局输出使能开关。这些在 BSP 注册阶段配置一次，各通道共享。

### D7: 同一控制器通道共享周期 — 文档化硬件约束

多数 MCU 的 PWM 控制器所有通道共享 ARR/PSC。同一控制器的不同 `channelConfig` 调用若传入不同 periodNs，后者覆盖前者。框架不阻止（过于侵入），但 API 文档明确说明此约束。

***

## 6. 关键流程

### 6.1 通道配置 (pwm\_channel\_config)

```
pwm_channel_config(ch, cfg)
  │
  ├── pwm_channel_validate(ch) → 校验 ctrl 非空 + channel 在范围内
  ├── 校验 cfg 参数（period/pulse/polarity 合法性）
  ├── ns_to_cycles(cap, periodNs) → periodCycles
  ├── ns_to_cycles(cap, pulseNs) → pulseCycles
  ├── ops->channelConfig(ctrl, ch, periodCycles, pulseCycles, polarity)
  │     │
  │     ├── bsp_pwm_to_timer_cycles() → 转换到 硬件时钟域
  │     ├── bsp_pwm_calc_psc_arr() → 计算预分频/周期寄存器值
  │     ├── htim->PSC = psc; htim->ARR = arr  (直接寄存器写入, 不调 Init)
  │     └── 硬件通道配置接口() → 写比较值 + 极性
  │
  ├── osal_irq_lock → chState[ch].periodNs = cfg->periodNs  (ISR 并发保护)
  │                   chState[ch].periodCycles = periodCycles
  │                   chState[ch].pulseNs = cfg->pulseNs
  │                   chState[ch].polarity = cfg->polarity
  └── osal_irq_unlock
```

### 6.2 通道使能 (pwm\_channel\_enable) — 框架级幂等

```
pwm_channel_enable(ch)
  │
  ├── pwm_channel_validate(ch)
  ├── if (chState[ch].periodNs == 0) → OM_ERR_CONFLICT  (未 config)
  ├── if (chState[ch].enabled) → OM_OK                  (幂等)
  ├── ops->channelEnable(ctrl, ch)
  │     ├── 硬件启动接口() → 通道输出使能 + 计数器使能
  │     └── 全局输出使能接口() (for 高级定时器)
  └── chState[ch].enabled = true
```

### 6.3 运行时脉宽更新 (pwm\_channel\_set\_pulse) — ISR 路径

```
pwm_channel_set_pulse(ch, pulse_ns)
  │
  ├── pwm_channel_validate(ch)
  ├── if (!chState.enabled) → OM_ERR_CONFLICT  (configuredMask 检测)
  ├── if (pulse_ns > chState.periodNs) → OM_ERR_RANGE
  ├── pulse_ns == 0            → pulse_cycles = 0              (快速路径)
  ├── pulse_ns == periodNs     → pulse_cycles = periodCycles   (快速路径)
  ├── else → (pulse_ns * periodCycles) / periodNs              (64-bit)
  └── ops->channelSetPulse(ctrl, ch, pulse_cycles)
        └── 硬件比较寄存器写入() → 单条 MMIO store, ISR 安全
```

### 6.4 Device close — 安全停止全部通道

```
pwm_dev_close(dev)
  │
  ├── osal_irq_lock
  ├── for i in 0..numChannels:
  │     if chState[i].enabled:
  │       ops->channelDisable(ctrl, i)
  │       chState[i].enabled = false
  └── osal_irq_unlock
```

***

## 7. 设计决策对照开源方案

| 决策                | OM                     | Linux                   | Zephyr            | RT-Thread       | 厂商 HAL                  |
| ----------------- | ---------------------- | ----------------------- | ----------------- | --------------- | -------------------------- |
| **通道状态归属**        | 框架层 (chState)          | 框架层 (pwm\_device.state) | 驱动层               | BSP 层           | HAL 层 (hdma/ChannelState)  |
| **控制器模型**         | PwmController (Device) | pwm\_chip (device)      | struct device     | rt\_device\_pwm | TIM\_HandleTypeDef         |
| **通道模型**          | PwmChannel (轻量句柄)      | pwm\_device (独立对象)      | uint32\_t channel | int channel     | TIM\_CHANNEL\_x 宏          |
| **enable 幂等**     | 框架级                    | 驱动 apply 层              | 不适用（无独立 enable）   | 依赖 BSP          | 无保护                        |
| **config+enable** | 分离                     | 原子 apply                | set\_cycles 打包    | 完全分离            | 分离                         |
| **能力声明**          | PwmCapability struct   | ops 函数指针判空              | DT + Kconfig      | 无（BSP 私有）       | 无                          |
| **ISR 脉宽更新**      | channelSetPulse        | apply (atomic)          | set\_cycles       | 无专用 API         | \_\_HAL\_TIM\_SET\_COMPARE |

***

## 8. 相关文件

| 文件                                                                     | 说明                            |
| ---------------------------------------------------------------------- | ----------------------------- |
| `lib/drivers/include/drivers/peripheral/pwm/pal_pwm_dev.h`             | PAL 接口定义                      |
| `lib/drivers/src/peripheral/pwm/hal_pwm.c`                             | 框架实现                          |
| `lib/drivers/include/drivers/model/device.h`                           | Device 模型 + DEVICE\_TYPE\_PWM |
| `lib/include/core/om_config.h`                                         | OM\_USE\_HAL\_PWM 裁剪宏         |
| `lib/drivers/include/drivers/peripheral/pal_dev.h`                     | PAL 聚合入口                      |
| `platform/bsp/boards/<board>/include/bsp_pwm.h`                         | BSP 头文件（板级实例声明）        |
| `platform/bsp/boards/<board>/source/peripherals/pwm/bsp_pwm_impl.c`     | BSP 实现（硬件 ops）             |
| `docs/03_pwm_pal_interface_design.md`                                  | PAL 接口设计规格（workspace）         |

