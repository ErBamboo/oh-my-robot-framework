# omlayout — boot layout source generator

把版本控制的 JSON 布局声明转换为 C 头、分区表、上位机清单和布局指纹。

JSON 布局声明是唯一事实源；生成的 C 头、分区表与 JSON 不得手工修改。当前阶段
只生成 C 头、分区表和生成清单，链接脚本先做一致性校验，后续再切换为生成片段。

## 命令

```sh
# 生成
python tools/omlayout/omlayout.py generate \
    --spec samples/boot/layout/rm-a-board/om_bootcfg.json \
    --out build/layout

# 校验布局、现有链接脚本和镜像契约
python tools/omlayout/omlayout.py check \
    --spec samples/boot/layout/rm-a-board/om_bootcfg.json \
    --linker-root platform/bsp/boards/rm-a-board/linker \
    --contract lib/boot/include/boot/image.h

# 单元测试
python -m unittest discover -s tools/omlayout/tests -v
```

## 生成物

- `om_bootcfg.h`：`OM_BOOT_PARTITIONS`、槽位链接/VTOR 常量、payload offset、
  meta 份数、布局指纹
- `boot_partition_data.c` / `.h`：const 分区表与 `om_boot_partition_registry()`
- `om_bootcfg.generated.json`：供 `omimg` / `omhost` 消费的布局清单

## 源文件格式

源文件是 JSON，偏移为 flash 相对偏移。`flash.eraseRegions` 必须连续覆盖
整个 flash。`app` 分区必须声明 `slot = "a" | "b"`，链接基址由生成器按
`flash.base + offset + payloadOffset` 推导；VTOR 偏移为 `offset + payloadOffset`。

`payloadOffset` 属于镜像契约，GUI 中应作为只读派生值展示；`check` 会与
`image.h` 的 `OM_IMAGE_PAYLOAD_OFFSET` 对拍。

JSON Schema 见 `tools/omlayout/om_bootcfg.schema.json`，供编辑器和上位机表单使用。
CLI 与未来的上位机使用同一套校验逻辑；GUI 保存同一个 JSON 源文件，不直接生成
C 头。

生成器会校验：

- 分区与保留区不重叠、在 flash 内、按擦除单位对齐；
- app_a/app_b 等大；
- meta 分区的独立擦除单位数不少于 `metaCopies`；
- `payloadOffset` 与 `image.h` 的 `OM_IMAGE_PAYLOAD_OFFSET` 一致（`check` 命令）；
- 当前链接脚本的 ROM 地址/长度与声明一致（`check --linker-root`）。

## 文件位置

```text
框架示例：       samples/boot/layout/rm-a-board/om_bootcfg.json
用户项目源：     cfg/boards/<board>/om_bootcfg.json
构建生成物：     build/.../gen/om_bootcfg/
```

## 边界

当前 `check` 只校验已有链接脚本，不生成链接脚本，也不替代构建期生成。链接
脚本的模板化/生成、VTOR 的构建注入、GUI 编辑器和 CI 漂移检查属于后续切片。
