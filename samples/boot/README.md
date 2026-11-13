# Boot 构建样例

本目录只放 bootloader 最小入口；构建壳由工程生成。三个产物可共用同一份 app 源码，按链接变体各构建一次。

## 构建变体

| 产物 | os | om_link_variant | om_selfreg_profile | om_framework_main | 链接基址 / VTOR |
| --- | --- | --- | --- | --- | --- |
| bootloader | none | bootloader | boot | off | 0x08000000 / 0 |
| app_a | freertos | app_a | full | on | 0x08010200 / 0x10200 |
| app_b | freertos | app_b | full | on | 0x08110200 / 0x110200 |

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

- 槽基址与容量由工程 `cfg/boards/<board>/om_bootcfg.h` 的分区表给出；链接脚本与 `OM_IMAGE_PAYLOAD_OFFSET` 必须与之同源。
- bootloader 形态不注入 `lib/services/src`、`lib/systems/src` 与 SEGGER RTT；需要日志时在 boot 自注册子集中显式加入最小后端。
- app_a/app_b 的向量表偏移由 `OM_BOARD_VECT_TAB_OFFSET` 注入 `bsp_cpu.c`，启动时写入 `SCB->VTOR`。
