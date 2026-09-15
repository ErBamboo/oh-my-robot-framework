# ADR-0025：bootloader 决策数据契约与跳转契约（S2-3 主干）

- 状态：已决策（2026-09-16，Q-12..Q-16 拍板）
- 日期：2026-09-16
- 参考：ADR-0021 (boot_multi_strategy_skeleton)、ADR-0022 (osal_none_bare_metal)、ADR-0024 (partition_registry_handle)；`docs/boot_ota/reference_design_notes.md` 的 K-24 / K-30..K-35；`docs/boot_ota/multi_strategy_boot_design.md` §3.3/§3.4/§5；`docs/boot_ota/boot_ota_requirements.md`

## 背景 (Context)

开工序列步骤 ②（Bootloader 最小工程）进入 S2-3 落地阶段。前置件全部就绪：FlashDev v1（PR #70）、分区表抽象 v2（ADR-0024）、osal-none v2（ADR-0022）、日志三件套、镜像头契约 v1 与主机侧工具（`lib/boot/include/boot/image.h` / `tools/omimg` / `tools/host/boot_image_test`）。

三个原记"实现期定"的开口现在必须闭合——否则决策核无法开工：

1. **决策数据 entry 布局**：K-24 已定三条铁律（提交标记最后写、永不擦当前有效副本、计数器放 entry 本体），但字段与宽度未定（`boot_ota_requirements.md` 明列为"尚未拍板的数值"）。
2. **中断责任方**：R-JMP-2 明文要求"bootloader 跳转前放开 / app 早期统一放开——二者择一"，此前未择。
3. **后门门禁形态**：K-21 记"门禁位置实现期定"；R-DL-5 的判据悬空。

S2-3 主干五路源码级实核（K-30..K-35，2026-09-16）补齐了生态事实；框架现状实核另发现四条硬约束：① F4 flash 后端以 **EOP 中断为主路径**（`bsp_flash_f4.c:328-331` 使能 NVIC + ISR 投信号量），bootloader 承担提交与后门直写（K-13/Q-09）故**需要中断**；② os=none 下 `om_system_startup()` **不可用**——post 段建 init 线程经直调占位落到 `osal_thread_exit()` 死循环（`osal_thread_none.c:84-95`），且 `osal_kernel_start()` 为 no-op、**不存在中断放开点**；③ selfreg 与板级源自注册**无 target 裁剪维度**，log 全族与全部外设适配器会进入任何 binary；④ 一个板 + 一个工具链**只有一份链接脚本**（`assets.lua:73-75` 无 target 维度）。

## 考虑过的方案 (Options)

### 决策数据 entry（Q-12）

- **A. 32B 定长整写式**：每次状态跃迁擦另一份 16K 扇区、整条写入。**采纳。** 一次 OTA 生命周期擦除 ≈ N+2 次（N=3 → 5 次），耐久模型无争议，保留 Q-06 的"N 可配"。
- B. 追加式（扇区内 16B 记录顺序追加，写满才轮转擦）：擦除次数 ≈ 0～1 次/OTA，但收益建立在"一个擦除周期内多次 program 不损及寿命/数据保持"之上——**该口径无厂商原文背书**（K-30）。**否决（首版），记演进**：前置条件 = 拿到厂商依据 + 记录长度按 `writeUnit` 参数化 + 目标器件无 ECC 整页写约束。
- C. ESP 一次性语义（16B，无计数字段，未确认的第二次启动即回退）：与生态对齐度最高、代码最简，但放弃 Q-06 已拍板的"N 可配"。**否决。**

### 中断责任方（Q-13）

- **A. bootloader 跳转前重建复位态并放开**：关中断 → ICER/ICPR 全清 → 停 SysTick 并清其 pending → `HAL_DeInit()` → 写 VTOR → 装 MSP → 清 CONTROL → 放开中断 → 跳。**采纳。** 依据：① OpenBLT 的成文先例（注释原文："The Cortex-M4 core has interrupts enabled out of reset … Enable them here again, so it does not have to be done by the user program."，K-33）；② **app 侧零改动**——现有启动代码的隐含契约就是"复位默认 PRIMASK=0、NVIC 复位全禁"，方案 A 正是重建该状态；③ os=none 与 FreeRTOS 两形态通吃；④ 放开窗口安全的前提（NVIC 全清）由清单自身保证。
- B. app 早期放开（MCUboot+Zephyr 主流）：**否决。** 代价 = os=none 下**今天不存在放开点**（`osal_kernel_start()` 为 no-op），需新增放开时机并重新定义 pre-scheduler 段（EARLIEST/BOARD/DRIVER）的中断语义——为"跳转动作"这一个点改动所有形态的启动契约。

### 看门狗（Q-14）

- **首版不启用**：bootloader 不启动 IWDG。**采纳。** 依据：F427 的 IWDG 一旦启动**无法软件关闭**（HAL 无 DeInit；选项字节 `WDG_SW=0` 时上电即在跑），而 OMR 当前无任何看门狗驱动（`HAL_IWDG_MODULE_ENABLED` 为注释态）；生态在跳转点均不碰狗（K-33）。
- 启用并定义"谁喂"契约：**否决（首版）**，记演进——启用时该契约不可留给隐式约定（MCUboot Cypress 与 ESP Kconfig 两处成文先例）。

### 防降级（Q-15）

- **首版不纳入**。**采纳。** 依据：direct-xip 下纯版本比较不构成"拒绝降级"（MCUboot 自身 `depends on !BOOT_DIRECT_XIP`，direct 的默认行为是选最高版本），而硬件安全计数器要真正防得住必须依赖签名链（protected TLV 亦只被摘要覆盖，无签名时可重算）——签名是 Q-04 已记的演进档（K-34）。
- 纯版本比较 / 硬件计数器：**否决（首版）**，记演进。

### 后门门禁（Q-16）

- **非凭证门槛**：进入窗口 + 触发条件（无有效镜像等）+ 命令面编译期裁剪。**采纳。** 依据：这是生态既有形态（ST 走 option bytes、mcumgr 走传输层权限、U-Boot 走编译期裁剪），且与 R-DL-1（两槽全坏时仍能被完整灌写）不冲突——"要不要凭证"与"能不能兜底刷写"在生态里是两个独立旋钮（K-35）。
- 会话凭证（seed&key 类）：**否决（首版）**，记演进。

## 最终决策 (Decision)

**决策数据 entry（32B 定长，小端）**

| 偏移 | 宽 | 字段 | 说明 |
|---|---|---|---|
| 0x00 | u32 | `seq` | 单调递增；`0xFFFFFFFF` 保留为"擦除/无效"哨兵，永不写入 |
| 0x04 | u32 | `activeSlot` | 当前应启动的槽（0=A / 1=B） |
| 0x08 | u32 | `state` | 0=EMPTY / 1=PENDING / 2=VALID / 3=ABORTED |
| 0x0C | u32 | `bootCount` | 未确认窗口内的已启动次数（仅 PENDING 时推进） |
| 0x10 | u32 | `imageVersion` | 所选槽镜像头版本快照（诊断；防降级演进时复用） |
| 0x14 | u32 | `flags` | 保留（当前 0；bit0 语义预留 = 本次写由确认触发） |
| 0x18 | u32 | `reserved` | 恒 `0xFF`（与擦除态一致，便于判断"是否被写过"） |
| 0x1C | u32 | `crc32` | CRC-32/ISO-HDLC，覆盖 `0x00..0x1B`，**最后写** |

- 每份占一个 16K 扇区、entry 置于**扇区头部**；两份轮转（`meta` 分区 0x100000 / 0x104000）。
- **读取**：CRC 有效且 `seq` 最大者胜；两份皆无效 → 冷启动路径（挑合法镜像，K-24/失败模式矩阵）。seq 比较用序号算术，`0xFFFFFFFF` 哨兵排除在外。
- **写序**：擦另一份扇区 → 写 `0x00..0x1B` → **最后写 `crc32`**；永不擦当前有效份。
- **计数**：`state ∈ {PENDING}` 时在**跳转 app 前**写入 `bootCount+1` 并**顺带 `seq+1`**；`state == VALID` 时**完全跳过写**（稳态零写）；confirm = 一次写（置 VALID）；达限回退 = 校验另一槽合法后写一条指向它的 entry（state=ABORTED/指针回退），不合法则停后门。
- **N 默认 3**（`OM_BOOT_ROLLBACK_LIMIT`，工程可覆写）。

**跳转契约（bootloader → app）**

清单固定为：关中断 → NVIC ICER/ICPR 全清 → 停 SysTick 并清其 pending → `HAL_DeInit()` → 写 VTOR（= 目标槽负载基址）→ 装 MSP（= 目标向量表[0]）→ 清 CONTROL → 放开中断 → 取复位向量跳转。**中断放开的责任方 = bootloader**（app 既有启动流程零改动）。bootloader 自身运行期允许开中断（EOP 主路径），跳转前必须回到"NVIC 全清 + 已关中断"的确定态再按清单放开。

**看门狗**：首版不启用（bootloader 不启动 IWDG）。

**防降级**：首版不纳入。

**门禁**：首版非凭证门槛（进入窗口 + 触发条件 + 命令面编译期裁剪）。

演进项（追加式决策数据记录、看门狗保护、防降级、会话凭证）登记在 `multi_strategy_boot_design.md` §5.1。

## 影响 (Consequences)

- 正面：三个"实现期定"开口闭合，S2-3 决策核可开工；CRC 覆盖决策字段全集（相对 ESP 只护 4 字节 seq 的改进）；方案 A 使 app 启动代码零改动且两 os 形态通吃；entry 布局自带 `flags`/`reserved`，将来换追加式范式只需升 entry 版本、不动分区布局。
- 行为：meta 每次状态跃迁 = 一次 16K 扇区擦（N=3 时一次 OTA 生命周期 ≈ 5 次，两扇区轮转分摊）；bootloader 跳转前必须执行完整清理清单，不得沿用 ST 例程的"裸跳"。
- 约束：擦除寿命按整写式口径计；**无防降级** → 结构合法但版本更旧的镜像会被正常启动（演进档补）；**无凭证门禁** → 物理可达 + 命中窗口即可进入后门（演进档补）。
- 兼容：FlashDev 与分区表零改动；镜像头契约零改动（决策状态严格不进镜像头，IH-13）；osal-none 端口零改动。
- 后续：S2-3 落地缺口（槽感知链接脚本、bootloader target 形态、selfreg 按 target 裁剪、跳转实现、决策核、后门命令组 v0、决策核 host 语料）另表跟踪；同期执行的文档口径回改见提交记录。
