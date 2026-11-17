# omhost — 上位机（固件打包 / 镜像检查 / 测试台）

本地 Web 界面：拖入裸固件 `.bin` → 填版本与目标槽位 → 打包 → 看镜像头字段与摘要 →
下载成品；拖入一个**已有镜像**则自动切换为检查模式，直接给出判定顺序的结论。
可加载工程生成的布局清单，让打包/检查使用同一份槽容量与负载偏移；
也可直接编辑布局源（校验通过才写回），作为布局的图形化主入口。

测试台页面探测串口与 J-Link，由用户分配角色；测试矩阵用 JSON 用例累积，
选中后一键运行，步骤与日志在页面内显示。

界面按工位拆分为“镜像 / 布局 / 测试台”，左侧导航切换，URL hash（`#image`、
`#layout`、`#bench`）可直达；各工位首次打开时才加载对应数据。

## 用法

```sh
tools/omhost/omhost.bat          # Windows：起后端并自动开界面
tools/omhost/omhost.sh           # Linux/macOS

# 或直接跑后端（不自动开浏览器）
python tools/omhost/server.py --port 8787 --no-browser

# 加载工程布局清单（推荐；校验负载偏移与槽容量）
python tools/omhost/server.py --layout build/gen/rm-a-board/om_bootcfg/om_bootcfg.generated.json

# 同时指定布局源（界面可编辑；未指定时按清单板名在 ./cfg/boards/<board>/ 下查找）
python tools/omhost/server.py \
    --layout build/gen/rm-a-board/om_bootcfg/om_bootcfg.generated.json \
    --layout-source cfg/boards/rm-a-board/om_bootcfg.json

# 测试台：资源分配默认存 ~/.omhost/config.json；可追加用例目录
python tools/omhost/server.py \
    --state ~/.omhost/config.json \
    --test-dir cfg/omhost/testcases
```

界面地址 `http://127.0.0.1:8787/`；`Ctrl+C` 退出。

同一端口只允许一个 omhost 实例。重复启动时先探测 `/api/health`：若已有 omhost
实例，直接打开现有界面并退出；端口被其它程序占用才报错。界面左下角显示后端
PID；若页面报“未知路径 /api/...”，通常是旧 omhost 进程仍在服务，结束旧进程后
重启即可（旧进程会读取磁盘上的新 `index.html`，但路由仍是旧代码）。

## 架构：界面不含任何契约逻辑

```
浏览器（index.html：展示与交互）
    │  HTTP（仅本机）
    ▼
server.py（本地后端：路由与 JSON 编解码）
    │  import
    ▼
omimg（契约的唯一 Python 实现：打包、解析、判定顺序、摘要）
    │  load_contract() 运行前解析
    ▼
lib/boot/include/boot/image.h（C 契约头 = 事实源）

--layout（可选）
    ▼
om_bootcfg.generated.json（构建生成的布局清单）
    → omhost 校验负载偏移、槽标识与槽容量

--layout-source（可选）
    ▼
cfg/boards/<board>/om_bootcfg.json（布局源，构建时生成上面的清单与链接脚本）
    → 界面读取 / 校验 / 写回；校验复用 omlayout 的同一套规则

bench.py（测试台）
    ├─ 资源探测：pyserial 串口列表 + JLink.exe ShowEmuList
    ├─ 角色分配：relay / serial / jlink，保存到 ~/.omhost/config.json
    ├─ 测试矩阵：tools/omhost/testcases/*.json（可追加 --test-dir）
    ├─ 执行器：omrelay 开关/脉冲/断电、J-Link 软复位、镜像打包/检查、布局校验
    └─ 产物与日志：~/.omhost/runs/<run-id>/
```

**因此不存在"第二份契约实现"可漂移**：镜像的字段布局、摘要算法、判定顺序全在
`omimg` 一处；界面连**一个常量都不写死**——魔数、头长、负载偏移、摘要区尺寸、
算法列表全部经 `/api/contract` 从后端取，而后端又从 C 契约头加载。换契约，界面
自动跟随。

界面对"拖入的是固件还是镜像"的判定同样如此：读文件头 4 字节与契约里的魔数比较，
不硬编码 `0x4F4D5249`。

## 端点

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 界面 |
| GET | `/api/health` | 后端 PID 与启动时间（用于识别旧实例） |
| GET | `/api/contract` | 契约常量（含加载状态、后端 PID、契约头路径与可选布局信息） |
| GET | `/api/layout` | 布局源内容 + 校验后的布局投影（指纹、槽链接基址等） |
| POST | `/api/layout/validate` | 请求体 = 布局源 JSON；校验并返回布局投影，不落盘 |
| POST | `/api/layout` | 请求体 = 布局源 JSON；校验通过后原子写回源文件 |
| GET | `/api/resources` | 探测串口与 J-Link，返回已保存的角色分配与在线状态 |
| POST | `/api/resources` | 请求体 = `{roles, settings}`；保存资源分配 |
| POST | `/api/resources/relay-probe` | 请求体 = `{port, baud}`；只读查询继电器四路状态 |
| GET | `/api/tests` | 测试矩阵列表（含阻塞原因与缺失资源） |
| POST | `/api/tests/run` | 请求体 = `{id}`；启动测试，返回运行对象 |
| GET | `/api/tests/run?id=` | 查询运行状态、逐步结果与日志 |
| POST | `/api/pack?version=&slot=&algo=&payloadOffset=` | 请求体 = 裸负载；返回头字段、摘要、校验结论、成品镜像（base64） |
| POST | `/api/inspect` | 请求体 = 镜像；返回头字段与判定顺序结论 |

指定 `--layout` 后，打包默认使用布局负载偏移，并按镜像 `slotId` 取槽容量校验；
检查模式同样按布局偏移与槽容量校验。未指定时退回契约头单镜像行为。

布局源编辑器校验失败的 JSON 不会落盘；保存采用同目录临时文件 + 原子替换。
保存只改源文件，生成头、分区表、清单与链接脚本仍由下一次构建重新生成，
因此界面在保存后提示"重新构建后生效"。指定 `--layout` 时，后端按 mtime 自动
重载生成清单，构建完成后刷新页面即可看到新指纹，无需重启后端。

## 测试台

### 资源：后端探测，用户分配

- 串口：pyserial 列出全部端口与 VID:PID；CH340 单独标记。
- J-Link：调用 `JLink.exe` 的 `ShowEmuList` 列出序列号与型号。
- 角色：
  - `relay`：USB 继电器串口（`tools/omrelay` 协议）；
  - `serial`：被测板串口（串口下载/日志，协议落地后使用）；
  - `jlink`：J-Link 序列号（软复位）。
- 同一串口只能分配一个角色；存在多个 CH340/J-Link 时由用户显式选择，
  不自动猜测。资源分配保存到 `~/.omhost/config.json`（可用 `--state` 覆盖）。
- 页面上的“查询继电器状态”只发查询帧，不改变继电器状态。

### 测试矩阵：一个 JSON 一个用例

内置目录 `tools/omhost/testcases/`；工程可在 `cfg/omhost/testcases/` 追加，
或用 `--test-dir` 指定。新用例放入目录后点“刷新”即可出现。

用例格式：

```json
{
  "schema": 1,
  "id": "bench-relay-pulse",
  "name": "继电器复位脉冲（CH1）",
  "description": "CH1 吸合 150 ms 后释放。",
  "steps": [
    { "type": "relay_query", "channel": 1, "expect": "off" },
    { "type": "relay_pulse", "channel": 1, "ms": 150 },
    { "type": "delay", "ms": 500 }
  ]
}
```

角色由步骤类型推导，不需要在用例里重复声明：

| 步骤 | 作用 |
|---|---|
| `delay` | 等待 `ms` |
| `relay_pulse` | 通道吸合 `ms` 后释放 |
| `relay_cycle` | 通道释放 `offMs` 后吸合并保持 |
| `relay_set` | 通道吸合/释放（`state: on/off`） |
| `relay_query` | 查询并校验 `expect: all_off/all_on` 或 `channel` + `expect: on/off` |
| `jlink_reset` | J-Link 软复位；`run: false` 时复位后保持 halt |
| `layout_validate` | 校验布局源 |
| `image_pack` | 用布局打包镜像；支持 `payloadText` / `payloadFile`、`slot`、`version`、`algo` |
| `image_inspect` | 用布局检查镜像 |
| `serial_download` | 串口下载（待 OMR-49，当前用例需标 `blocked`） |

- `layout` / `payloadFile` 路径先按用例所在目录解析，再按仓库根与当前目录解析。
- `output` / `image` 产物名相对运行目录；运行目录在 `~/.omhost/runs/<run-id>/`。
- 用例标 `"blocked": "OMR-49"` 时在页面显示为“待 OMR-49”，运行按钮禁用。
- 同一时间只允许一个运行；逐步状态、日志与产物路径在页面内显示。

内置用例：

| id | 需要 | 说明 |
|---|---|---|
| `host-layout-image-a` | 无 | 布局校验 + A 槽镜像打包/检查 |
| `bench-relay-pulse` | relay | CH1 脉冲并确认释放 |
| `bench-jlink-reset` | jlink | J-Link 软复位并恢复运行 |
| `bench-power-cycle` | relay | CH2 断电 1 s 后上电并保持（需接电源回路） |
| `ota-download-a` | relay + serial | 串口下载到 A 槽（待 OMR-49） |
| `ota-resume-b` | relay + serial | B 槽中断续传（待 OMR-49） |

## 分期

| 期 | 能力 | 状态 |
|---|---|---|
| ① | 固件打包 + 头字段预览 + 镜像检查 + 布局清单校验 + 布局源编辑 + 测试台（资源分配 / 用例矩阵 / 硬件复位与检查） | **已交付** |
| ② | 串口下载器（握手 / 块传输 / 进度 / 重传 / 续传） | 待后门协议实现 |
| ③ | 设备信息与状态读取（当前槽 / 版本 / 待确认） | 待契约的读命令 |
| ④ | 运行日志 / 诊断同屏显示 | 待设备日志通道 |

②③④ 都会走同一个后端：协议客户端用 Python 实现在后端侧，界面只负责展示——
这样协议逻辑也能被无硬件测试（字节流夹具对拍 C 侧实现），不必每次接板子。
