# omlayout — boot layout source generator

把版本控制的布局声明文件转换为 C 头、分区表、上位机 JSON 和布局指纹。

布局声明文件是唯一事实源；生成的 C 头与 JSON 不得手工修改。当前阶段只生成
C 头、分区表和 JSON，链接脚本先做一致性校验，后续再切换为生成片段。

## 命令

```sh
# 生成
python tools/omlayout/omlayout.py generate \
    --spec samples/boot/layout/rm-a-board.toml \
    --out build/layout

# 校验 schema、现有链接脚本和镜像契约
python tools/omlayout/omlayout.py check \
    --spec samples/boot/layout/rm-a-board.toml \
    --linker-root platform/bsp/boards/rm-a-board/linker \
    --contract lib/boot/include/boot/image.h

# 单元测试
python -m unittest discover -s tools/omlayout/tests -v
```

## 生成物

- `om_bootcfg.h`：`OM_BOOT_PARTITIONS`、槽位链接/ VTOR 常量、payload offset、
  meta 份数、布局指纹
- `boot_partition_data.c` / `.h`：const 分区表与 `om_boot_partition_registry()`
- `om_layout.json`：供 `omimg` / `omhost` 消费的布局清单

## 源文件格式

源文件是 TOML，偏移为 flash 相对偏移。`flash.erase_regions` 必须连续覆盖
整个 flash。`app` 分区必须声明 `slot = "a" | "b"`，链接基址由生成器按
`flash.base + offset + payload_offset` 推导；VTOR 偏移为 `offset + payload_offset`。

生成器会校验：

- 分区与保留区不重叠、在 flash 内、按擦除单位对齐；
- app_a/app_b 等大；
- meta 分区的独立擦除单位数不少于 `meta_copies`；
- `payload_offset` 与 `image.h` 的 `OM_IMAGE_PAYLOAD_OFFSET` 一致（`check` 命令）；
- 当前链接脚本的 ROM 地址/长度与声明一致（`check --linker-root`）。

## 边界

当前 `check` 只校验已有链接脚本，不生成链接脚本，也不替代构建期生成。链接
脚本的模板化/生成、VTOR 的构建注入和 CI 漂移检查属于后续切片。
