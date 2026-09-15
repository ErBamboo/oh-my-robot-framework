# Bootloader（S2-3）落地缺口分析

> **状态**：2026-09-16 现状实核。依据 = `docs/boot_ota/reference_design_notes.md` 的 K-30..K-35、`multi_strategy_boot_design.md` §5（Q-12..Q-16）、ADR-0025；镜像头契约见 `docs/boot_ota/image_header_contract.md`。
> **范围**：开工序列步骤 ②（Bootloader 最小工程）在 rm-a/F427 上的落地账目。后门协议 v0（帧与命令组编号）属独立批次，本文件只登记其接口位置。
> **目录命名**：upstream Issue 尚未开，按规范落 `issue_000_`；Issue 建立后随号迁移。

## 1. 已就绪（可直接复用，不重复建造）

| 件 | 出处 | 用途 |
|---|---|---|
| 镜像格式契约头 | `lib/boot/include/boot/image.h`（64B 头 + 256B 摘要区 + 十步判定顺序 + 尺寸编译期锁定） | 决策核的解析/校验依据；bootloader 与 app 共享的 ABI |
| CRC-32/ISO-HDLC 原语 | `lib/algorithm`（`om_crc32_iso_hdlc`，**支持分段续算**） | 流式校验整镜像（K-29 要求每次上电全量校验） |
| 打包工具与 host 语料 | `tools/omimg`、`tools/omhost`、`samples/host/boot_image_test`（30 项，跨语言对拍） | 产出可校验镜像；工具与目标端摘要一致性 |
| 分区表 v2 | `lib/drivers/include/drivers/storage/partition.h`：const 注册表 + `om_partition_query`（按名取几何）+ `om_partition_open`（名 + 器件 + 几何三合一） | 槽基址/容量按**分区名**获取；无运行期注册、无 init 顺序、API 可重入（ADR-0024） |
| FlashDev 同步面 + F4 适配器 | `pal_flash_dev.h`（read 同步 / write / erase 同步等待原语）+ `bsp_flash_f4.c`（几何 `writeUnit=4`、`erasedValue=0xFF`、EOP 中断主路径 + `BSP_FLASH4_IRQ_DISABLED` 轮询退化） | 槽与 meta 的全部擦写；os=none 下提交即执行 |
| osal-none 端口 | `platform/osal/none`（临界区真实现 / mutex / sem / 时间 / 软件定时器）+ `portable/cortex-m4`（SysTick 时基、PRIMASK 嵌套、IPSR 判定） | bootloader 运行时形态（ADR-0022） |
| 日志裁剪面 | `OM_USE_LOG`（服务级裁剪）、`OM_LOG_ASYNC=0`（现场触发，零 OSAL）、串口后端 + 级别宏 | bootloader 日志（接入点自定，见缺口 11） |
| 启动文件与弱 main 逃生通道 | `startup_stm32f427xx.s`（Reset_Handler → SystemInit → main）；`om_main.c` 弱 `main` + `om_framework_main=off` | bootloader 写强 main 即覆盖框架启动编排（ADR-0013） |
| 面积报告链路 | 构建后自动打印 FLASH/RAM used/total（区域名识别覆盖 ROM） | R-NFR-2 的可观测面 |
| 构建壳约定与先例 | `_verify_init/xmake.lua` 的 12 个 `verify_*` target（`add_deps("tar_oh_my_robot")` + 规则集 + `add_files`） | 新增 bootloader/app target 的照抄对象 |

## 2. 缺口清单（按落地顺序）

| # | 事项 | 现状（出处） | 需要新增/改动 |
|---|---|---|---|
| 1 | **槽感知链接基址** | 每板每工具链只有**一份** `startup`/`linkerscript`（`platform/bsp/assets.lua:73-75` 返回单值，无 target 维度）；ROM 现为 `0x08000000 LENGTH 2048K`（`linker/gcc/stm32f427iihx.ld:50`、`linker/arm/stm32f427iihx.sct:11`） | 解析层扩 target/tag 维度；产出三份链接脚本：bootloader（64K 域）、app_a（基址 `0x08010200`）、app_b（基址 `0x08110200`）；链接基址与 `OM_IMAGE_PAYLOAD_OFFSET` **同源** |
| 2 | **bootloader 专用链接脚本** | 无 | 64K 域 `.ld`/`.sct`：ROM `0x08000000 LENGTH 64K`，**必须保留 7 级 `.om_init_<N>` 段与边界符号**（linkguard 强制），保留 `HardFault_Handler` 强符号 |
| 3 | **app 侧 VTOR 注入** | 框架代码**从不写** `SCB->VTOR`；厂商 `system_stm32f4xx.c:179-181` 的写入被 `USER_VECT_TAB_ADDRESS` 门控，而该宏 `:94` 是注释态 | 按槽打开并配置 `VECT_TAB_OFFSET`（`0x10200`/`0x110200`），或由板适配显式写 VTOR；须有**工程级覆写机制**（厂商文件不直改） |
| 4 | **按 target 裁剪** | `selfreg` 无条件注入 `lib/services/src`、`lib/systems/src`（log 全族 + supercap）与 SEGGER_RTT（`xmake/rules/selfreg.lua:23-36`）；板级自注册/覆盖源无 target 形参（`board_assets.lua:34-40`、`inputs.lua:26-50` 含 peripherals 自动 glob） | 增加 bootloader 形态的注入裁剪（按 os 轴或 target 属性过滤）；否则 64K 域会被 log 全族 + 全部外设适配器撑爆 |
| 5 | **多 target 构建壳约定** | 壳为非仓库文件（`init_workspace` 生成单 binary 壳）；现有真实壳 `_verify_init/xmake.lua` 为多 target 先例 | bootloader target（`--os=none`）+ app target 并存；`om_preset.lua` 的 `os` 与 `flash.target` 需随 target 切换 |
| 6 | **bootloader 版板级初始化** | `bsp_cpu.c:86-98` 把时钟配置 + `om_cpu_register` + `DWT_Init` 绑在一起（`OM_INIT_BOARD` 注册） | 提供裁剪版（至少时钟 + `SystemCoreClock`——osal-none 的 arch 时基依赖它先就绪），或确认整体复用 |
| 7 | **启动流形态** | `om_system_startup()` 在 os=none 下**不可用**：post 段建 init 线程 → 直调占位 → `om_init_thread` 末尾 `osal_thread_exit()` 死循环（`osal_thread_none.c:84-95`） | 三选一：bootloader 写强 `main` / 构建期 `om_framework_main=off` / 只调 `om_startup_pre_scheduler()`；选定后写进 bootloader 工程模板 |
| 8 | **决策核本体** | `lib/boot/` 只有 `image.h`；`tar_awboot` 为 headeronly（`lib/xmake.lua:132-140`，注释已预告转 static） | 解析/摘要校验/槽位判定/提交动作实现；`tar_awboot` 转 static 并补依赖（partition / algorithm） |
| 9 | **决策数据读写模块** | 无（`boot_ota_requirements.md` 原列"entry 布局未定"） | 按 ADR-0025 的 32B 定长 entry 实现读（CRC 有效 + seq 最大）/写（擦另一份 → 写字段 → CRC 最后） |
| 10 | **跳转模块** | 全仓无 VTOR 写入、无跳转代码 | 按 K-32 清单 + Q-13 方案 A 实现（含 `HAL_DeInit()`、停 SysTick 并清 pending、ICER/ICPR 全清、放开中断） |
| 11 | **bootloader 日志接入点** | 无 `om_log_init` 公开 API；接入 = `OM_LOG_MODULE` + 后端实例 + 注册（`OM_INIT_DRIVER` 链路） | 单执行流下不依赖 initcall 的接入写法（手写注册）；早期滞留容量按 bootloader 重估（默认环 16 槽 ≈ 1.2KB） |
| 12 | **分区表实例数据文件** | `OM_BOOT_PARTITIONS`（`docs/config/om_bootcfg.h.example:35-40`）**全仓零消费点** | 新增 `boot_partition_data.c`（展开 X-macro → const 表 → `OM_PARTITION_REGISTRY`），两工程各编一份 |
| 13 | **bootcfg 工程片段** | 仓库内无 `<project>/cfg/boards/rm-a-board/om_bootcfg.h`；模板刚修正为 v2 API | 建片段（存在方触发 `-DOM_USE_BOOTCFG` 注入） |
| 14 | **后门命令组 v0** | 无实现（K-21/Q-08 记为演进项；帧与编号未定） | **独立批次**：协议帧（K-26 已给方向）、命令组编号、进入窗口/触发条件（Q-16 非凭证门槛） |
| 15 | **决策核 host 语料** | 现有 `boot_image_test` 只跑内存缓冲；`partition_test` 的 `flash_sim` 是 256KB 均匀 4KB 扇区 | 新建决策核 host target：非均匀几何 + 多分区（bootloader/app_a/meta/app_b）夹具；端到端用例 = 写镜像 → 经分区句柄读回 → 校验摘要；CI 挂载照 `ci.yml:147-154` |
| 16 | **armclang ELF 后处理硬编码** | `xmake/rules/image_convert.lua:55,66,69` 写死 `0x20000000` 与 `0x08000000 + (rw_off - 0x40)` | 参数化 flash/RAM 基址——槽链接基址下 LMA 修正会错位 |
| 17 | **linkguard 对裁剪 binary 的适用性** | 守卫要求 7 级边界符号 + 强符号（`toolchain_linkguard.lua`），GCC 侧边界符号由 `PROVIDE` 定义（未被引用可能不落地） | 确认/调整守卫对 bootloader binary 的策略，避免误报 |
| 18 | **面积门禁** | 报告只打印不 fail | 可选：bootloader 目标加 64K 阈值断言 |
| 19 | **构建脚本可疑路径** | `platform/bsp/boards/rm-a-board/xmake.lua:21` 导入 `build/modules`（该目录不存在；模块实际在 `xmake/modules`），当前靠 xmake 默认搜索路径兜住 | S2-3 动构建时确认并统一（本次未改，避免影响现有构建） |
| 20 | **ART 缓存未使能 + D-cache 勘误未处理**（2026-09-16 实核新增） | ① vendor `SystemInit` 为裁剪版、**不设 `FLASH_ACR`**；全仓非 vendor 代码**零 `ICEN`/`DCEN`/`PRFTEN` 使能点**（仅 `HAL_RCC_ClockConfig(..., FLASH_LATENCY_5)` 设了等待周期）→ **ART 指令/数据缓存当前是关的**；② F4 适配器未处理 **ST ES0206 勘误 2.2.15**（"Data cache might be corrupted during Flash memory read-while-write operation"，workaround = 写前 `DCEN=0` → 写后 `DCRST` 复位 → 重开；Zephyr `drivers/flash/flash_stm32f4x.c` 已实现并引用该勘误） | 二选一并落文档：① **保持缓存关闭**（当前事实）并写明"性能代价 + 为什么不能随手打开"；② **开缓存**（180MHz 下显然是性能正解）并**同时在适配器实现勘误 workaround**。注意：勘误当前**不生效**（数据缓存未开），但一旦为性能开启，就正好命中我们的 RWW 场景（写 bank2 / 跑 bank1） |
| 21 | **决策数据副本份数未定**（Q-12 待讨论） | 擦除归属已改引导侧（K-36/K-37/K-38），不变式要求副本 ≥3 份；2 份推演见 `multi_strategy_boot_design.md` §5.2 | 定份数后同步：`meta`/`meta_resv` 用途、bank2 头部 16K 空档的归属（与日志分区候选地互斥）、未确认窗口的启动时间预算 |
| 22 | **跨介质槽的落地前提**（若将来启用，K-39） | 分区表已支持 `devName`（架构就绪），但落地还差：外部器件进 flash map、驱动支持单字节读写与"片内→片外"写、跨介质时 `BOOT_MAX_IMG_SECTORS` 按外片粒度放大与写对齐显式覆盖、direct-xip 需外片 memory-mapped | 步骤 ④（外部 flash 批次）随 W25Q 驱动一起评估；**推论**：F427 片内变扇区槽排除 swap-offset/move，swap 只能 scratch 且 scratch ≥ 128K |
| 23 | **策略级几何约束无校验**（2026-09-16 实核新增） | 现状：只有运行期（注册期）的**通用几何**校验（扇区对齐/不越容量/不重叠，`om_partition_registry_validate`）；**策略级约束无人管**——如 direct-xip 的"两槽等大"、swap 的"scratch ≥ 最大扇区"、overwrite 的"staging 存在"。生态做法（K-40）：几何开放 + **构建期工具链**强制语义约束，而"等大性"几乎无人做构建期强制、失效形态是**静默降级** | 补两层（都符合本框架"配置错误显式报错、不静默"原则）：① **编译期 `_Static_assert`**（布局表是编译期常量，可拦等大性/对齐关系/scratch 与 staging 的存在性与最小尺寸/槽容量 ≥ 镜像上限）；② **工具侧容量校验**（`omimg verify` 已有 `--slot-capacity`，接进构建流程）。另可借鉴 settings-NVS 范式：把"用户填绝对值"改为"填相对量 + 框架按几何折算/夹取" |

## 3. 阻塞关系

- **1 / 3 / 4 是硬前置**：链接基址与裁剪不解决，bootloader 无法成为可构建的独立 binary。
- **7 是入口前置**：不选定启动流形态，bootloader 进不了 main。
- **8 / 9 / 10 是主体**，其中 9 依赖 ADR-0025 的 entry 契约；10 依赖 K-32 清单。
- **14（后门）与 15（host 语料）可并行**：前者独立批次（协议未定），后者依赖 8。
- **两路调研已完成（2026-09-16）** → K-39（跨介质槽：MCUboot/NCS 官方支持，几何约束 = 扇区尺寸互为倍数 + scratch ≥ 最大扇区 + scratch 模式等面积）、K-40（配置面：几何开放 + 构建期工具强制语义约束；"等大性"生态几乎无人做构建期强制且失效形态为静默降级）。落地项见缺口 #23。
- RWW 事实（`flash_dev_design.md` §8）：bootloader 位于 bank1 头部、与 app_a 同擦写域 → **擦写 app_a 会 stall bootloader**（16K 擦 ≈260ms、128K 擦 ≈1059ms），擦写 app_b 与 meta（bank2）不 stall；os=none 下无让出，属硬阻塞 → 由 R-DL-7/8（停等 + 超时覆盖最坏擦除）承接。

## 4. 同期执行的文档口径回改（2026-09-16）

| 项 | 处理 |
|---|---|
| `OM_FLASH_SYNC_ONLY` 幽灵宏（文档 6 处 vs 零代码消费） | ADR-0022 / `flash_dev_impl_design.md` D-08 / `multi_strategy_boot_design.md` Q-11 / K-16 / K-19 加**修订注记**；`pal_flash_dev.h` 两处注释直接改正 |
| thread "返 NOT_SUPPORTED" 表述 | 随上条修订注记一并说明（实现 = 直调占位） |
| `lib/osal/README.md` 仍列已删的 event/queue 与不存在的 linux 端口 | 直接改正（列表换为最小原语集 + `none` 端口 + `osal_port.h` 取值） |
| bootcfg 模板调用 v2 已删除的 `om_partition_register()` | 直接改正为 const 注册表 + `om_partition_registry_validate` |
| ST 文档引用纠错 | VTOR/NVIC 权威在 PM0214（RM0090 无此内容）；F4 IAP 例程对应 AN3965——已记入 K-32 与更新记录 |
| `rm-a-board/xmake.lua:21` 可疑路径 | 仅记账（缺口 19），未改动 |

## 5. 关联

- 决策正文：`docs/adr/0025-boot_decision_data_and_jump_contract.md`
- 调研档案：`docs/boot_ota/reference_design_notes.md`（K-24、K-30..K-35）
- 设计索引：`docs/boot_ota/multi_strategy_boot_design.md` §3.3/§3.4/§5/§5.1
- 需求基线：`docs/boot_ota/boot_ota_requirements.md` v2
