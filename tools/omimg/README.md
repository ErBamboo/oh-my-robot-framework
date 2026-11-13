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

**负载偏移**（`--payload-offset`）默认取契约头的约定值（`0x200`），须 0x200 对齐。
工程在 `om_bootcfg.h` 覆写该值时必须同步给本工具传参——它与 app 的链接偏移是
同一个值，两者不一致时镜像仍能打包，但启动才会失败。

## 两侧一致性

契约要求主机侧与目标侧的摘要**逐字节一致**。本工具的保障有四层：

1. **契约头是唯一事实源**：每次运行先解析 C 契约头加载魔数/版本/头长/负载偏移/
   摘要区尺寸，模块内的字面量只作"契约头不可读"时的兜底。C 侧改契约 → 工具下次
   运行即跟随，不会继续产出旧布局；
2. **头布局串长度断言**：字段增删后若没同步本文件的 `_HDR_FMT`，`pack` 立即报错；
3. **跨语言对拍基准**：`selftest` 用固定负载与元数据组装镜像，断言摘要与总长等于
   与 host 语料同一组值。契约改动会让两侧各自失败——改契约是有意识的动作，不是
   悄悄漂移；
4. **host 语料**（`samples/host/boot_image_test/`）用 C 侧契约头与原语组装同一镜像
   与基准比对，30 项含损坏检出与补齐边界。

改动契约、字段布局或摘要算法后：跑 `selftest` + host 语料，并按需更新基准值。

## 与契约文档的关系

字段语义与判定顺序的事实源是 `docs/boot_ota/image_header_contract.md` 与 C 契约头；
本文件只做实现，不复述契约。
