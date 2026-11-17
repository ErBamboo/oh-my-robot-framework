# Boot 构建样例

本目录放 bootloader 与槽应用测试样例；构建壳由工程生成。app_a/app_b 共用
`samples/boot/app/main.c`，按链接变体构建出不同槽身份。

## 样例行为

- **app_a / app_b**：注册 USART6 串口日志后端，启动时输出槽身份、链接基址、
  `SCB->VTOR`、布局指纹与构建时间；随后创建低优先级心跳线程，每秒输出
  `APP_A/B heartbeat n`。VTOR 与期望值不符时输出 ERROR。
- **bootloader**：执行调度器前初始化，校验生成的分区表，读取 meta 最新有效
  entry，并读取 A/B 槽头 magic；结果写入固定符号 `g_boot_sample_state`
  （只读，不写 meta、不跳转）。启动决策/跳转在 M2/M3 接入，样例不提前实现。
- 观测方式：
  - app 日志：USART6，115200；USB-TTL 只接 TX/RX/GND、3.3V，不接 VCC；
  - bootloader：用 Ozone/GDB 加载 ELF 后观察 `g_boot_sample_state`，或用
    `arm-none-eabi-nm` 取符号地址后 J-Link `mem32` 读取；
  - 直接烧 app ELF 只写入链接地址（槽基址 + 0x200），不会写槽头 0x200；
    因此 bootloader 读到的槽头 magic 在未烧完整槽镜像前是空白（0xFFFFFFFF）。
  - armclang 产物烧写用 HEX/BIN：J-Link `loadfile` 直接烧 armlink ELF 时不会
    写入 `.data` 的 flash 载入区，启动时 `__scatterload` 会把空白 flash 拷入
    RAM，表现为早期 HardFault。

## 构建变体

| 产物 | os | om_link_variant | om_selfreg_profile | om_framework_main | 链接基址 / VTOR |
| --- | --- | --- | --- | --- | --- |
| bootloader | none | bootloader | boot | off | 0x08000000 / 0 |
| app_a | freertos | app_a | full | on | 0x08010200 / 0x10200 |
| app_b | freertos | app_b | full | on | 0x08110200 / 0x110200 |

源码：

- bootloader：`samples/boot/bootloader/main.c`
- app_a / app_b：`samples/boot/app/main.c`（同一份源码，`OM_BOOT_VARIANT_APP_A/B` 选择槽身份）

## 命令（gnu-rm 示例）

```sh
xmake f -c --board=rm-a-board --os=none --toolchain=gnu-rm -m debug \
    --om_link_variant=bootloader --om_selfreg_profile=boot --om_framework_main=off
xmake build bootloader

xmake f -c --board=rm-a-board --os=freertos --toolchain=gnu-rm -m debug \
    --om_link_variant=app_a --om_selfreg_profile=full
xmake build app_a

xmake f -c --board=rm-a-board --os=freertos --toolchain=gnu-rm -m debug \
    --om_link_variant=app_b --om_selfreg_profile=full
xmake build app_b
```

- 工程壳的槽基址与容量由 `cfg/boards/<board>/om_bootcfg.json` 声明；构建规则调用 `tools/omlayout` 生成 C 头、分区表、链接脚本与上位机 JSON 到 `build/.../gen/om_bootcfg/`，生成物不手改。旧的手写 `om_bootcfg.h` 仅作过渡兼容，与 JSON 同时存在时构建失败。
- 链接脚本由 `platform/bsp/boards/<board>/linker/templates/` 的模板填入 ROM 基址/长度生成，`board_assets` 在链接变体下优先使用生成脚本。
- `tools/omimg` 与 `tools/omhost` 可用 `--layout build/.../om_bootcfg.generated.json` 读取同一布局，校验负载偏移与槽容量。
- 布局单一源示例见 `samples/boot/layout/rm-a-board/om_bootcfg.json`；生成器同时校验链接脚本与镜像契约。
- 布局源可用 `tools/omhost` 界面编辑：校验复用 `tools/omlayout` 规则，保存只写源文件，重新构建后生成物与链接地址才更新。
- 重复复位优先用 J-Link 软复位（SWD 发 `SYSRESETREQ`，无需焊接 NRST）；需要真正断电/冷启动时用 `tools/omrelay` 的 `cycle`（接电源回路）。
- 上位机测试台（`tools/omhost`）可探测串口/J-Link、由用户分配资源，并运行 `tools/omhost/testcases/` 中的复位、镜像与布局用例。
- 决策数据为 2 份 meta（`meta` 32K）+ `meta_resv` 16K 提交进度区；`0x10C000` 16K 保留不用。app 侧在无已擦副本时擦除非最新副本（2026-11-16 决定）。
- bootloader 形态不注入 `lib/services/src`、`lib/systems/src` 与 SEGGER RTT；需要日志时在 boot 自注册子集中显式加入最小后端。
- app_a/app_b 的向量表偏移由布局源中的槽偏移 + payload offset 解析，写入生成的 `om_bootcfg.h` 槽宏；`bsp_cpu.c` 按构建变体选择对应宏，启动时写入 `SCB->VTOR`。
