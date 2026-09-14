# omimg — 镜像打包与检查

镜像格式契约（`lib/boot/include/boot/image.h`）的**主机侧实现**：把裸负载打包成
带头部与摘要区的完整镜像，或反向解析、校验一个镜像。只用标准库。

## 用法

```sh
# 打包：裸 .bin → 镜像
python tools/omimg/omimg.py pack --input app.bin --output app_a.img.bin --version 1.2.3 --slot a

# 解析：打印头部字段
python tools/omimg/omimg.py inspect app_a.img.bin

# 校验：按契约判定顺序检查（可选给出槽容量以校验越界）
python tools/omimg/omimg.py verify app_a.img.bin --slot-capacity 0xF0000

# 自检：算法自检向量 + 契约常量一致性 + 往返与损坏检出
python tools/omimg/omimg.py selftest
```

`pack` 的默认行为：摘要算法 CRC32、`flags = SLOT_BOUND`、负载尾部补齐到 4 字节
倍数（补 `0xFF`）、摘要区其余部分填 `0xFF`。同一输入重复执行产出逐字节相同的文件。

## 两侧一致性

契约要求主机侧与目标侧的摘要**逐字节一致**，本工具的保障有两层：

1. `selftest` 解析 C 契约头，核对魔数/版本/头长/负载偏移/摘要区尺寸五项常量——
   两侧常量漂移会在自检里直接失败；
2. 主机侧 host 语料（`samples/host/boot_image_test/`）用 C 侧的契约头与原语组装
   同一个镜像，与工具产出的摘要比对。摘要覆盖整段头+填充+负载，**摘要一致即字节
   布局一致**。

改动本工具的字段布局或摘要算法后，必须同时跑 `selftest` 与 host 语料。

## 与契约文档的关系

字段语义与判定顺序的事实源是 `docs/boot_ota/image_header_contract.md` 与 C 契约头；
本文件只做实现，不复述契约。
