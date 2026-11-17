# omrelay — USB 继电器控制（测试台复位/上电）

用 CH340 USB 继电器给台架提供**上位机可控的复位与断电重启**，服务于
bootloader 救援会话的进入窗口、串口下载/续传与掉电半写测试。

本工具只操作继电器，不含任何固件协议；串口下载的线格式仍由 bootloader 侧定义。

## 快速开始

```sh
# 列出串口（CH340 会标记出来）
python tools/omrelay/relay.py ports

# 查询通道状态（9600 默认按 LCUS 发 FF；115200 默认按 TC 发 A0 帧）
python tools/omrelay/relay.py query --port COM10
python tools/omrelay/relay.py query --port COM10 --baud 115200
python tools/omrelay/relay.py query --port COM10 --channel 1 --style tc

# 吸合/释放
python tools/omrelay/relay.py on  --port COM10 --channel 1
python tools/omrelay/relay.py off --port COM10 --channel 1

# 吸合 150 ms 后释放（接 NRST-GND 时即一次复位）
python tools/omrelay/relay.py pulse --port COM10 --channel 1 --ms 150

# 断电 1 s 后上电并保持（接电源回路时即一次冷启动）
python tools/omrelay/relay.py cycle --port COM10 --channel 2 --off-ms 1000
```

`--port` 省略时自动选择唯一的 CH340（VID:PID `1A86:7523`）；
检测到多个 CH340 会报错并要求显式指定。通道也可写 `0x0F`（全部）。
有多个 CH340 时，对每个候选执行 `query`，返回 `CH1:ON/OFF` 的就是继电器。

依赖 pyserial：`pip install pyserial`。

## 接线建议

**首选：复位脉冲（不切电源）**

- 继电器常开触点（`COM1`/`NO1`）跨接板子的 `NRST` 与 `GND`；
- `pulse` 吸合期间把 NRST 拉低，释放后正常启动；
- 触点与板子电气隔离，不引入地环路，也不会误切主电源。

**需要真冷启动时：串入直流电源回路**

- 触点串在板子的直流输入回路（`COM2`/`NO2`），用 `cycle` 控制断电/上电；
- 先核对触点额定电压/电流覆盖板子输入，并考虑上电浪涌；不要用继电器直接切市电；
- 断电后若 USB-TTL 仍接在板子上，只接 `TX`/`RX`/`GND`，**不要接 VCC**：
  TX 空闲高电平可能经保护二极管反灌，让 MCU 没有真正掉电。使用 3.3V 电平适配器。

其余通道可留给 `BOOT0`、外设电源或备用。

## 与 J-Link 软复位的分工

**只复位时优先用 J-Link，不需要焊接复位按钮。** 已验证 J-Link Commander 可通过
SWD 发 `AIRCR.SYSRESETREQ` 复位并恢复运行：

```sh
cat > reset.jlink <<'EOF'
device STM32F427IIHx
si SWD
speed 4000
connect
r
g
exit
EOF

JLink.exe -NoGui 1 -ExitOnError 1 -SelectEmuBySN <SN> -CommandFile reset.jlink
```

- `r` 复位并 halt（默认 `NORMAL` 策略，走 `SYSRESETREQ`），`g` 恢复运行；
- 多台 J-Link 用 `-SelectEmuBySN <SN>` 选择；
- 不需要连接 J-Link 的 nRST，仅 SWDIO/SWCLK/GND 即可。

继电器只在需要**真正断电**（冷启动、掉电半写、续传）时使用 `cycle`；
NRST 触点方案仅在无法使用 SWD 时才需要。

## 与 omhost 测试台

`tools/omhost` 的测试台把本工具作为 `relay` 角色后端：页面探测串口后由用户
分配继电器端口，测试用例的 `relay_pulse` / `relay_cycle` / `relay_query`
步骤直接调用同一协议。查询应答兼容 `CH1:OFF` 与 `CH1: OFF` 两种格式
（LCUS 实测冒号后带空格）。

## 协议

四字节帧：`A0 <通道> <动作> <校验>`，校验 = 前三字节之和的低 8 位。

| 字节 | 含义 |
|---|---|
| 1 | 起始标志 `0xA0` |
| 2 | 通道 `0x01`..`0x08`；`0x0F` = 全部 |
| 3 | 动作 `0x00` 关、`0x01` 开、`0x02` 查询 |
| 4 | 前三字节之和的低 8 位 |

示例：

| 操作 | 帧 |
|---|---|
| 开第一路 | `A0 01 01 A2` |
| 关第一路 | `A0 01 00 A1` |
| 开第四路 | `A0 04 01 A5` |
| 开全部 | `A0 0F 01 B0` |
| 查第一路 | `A0 01 02 A3` |
| 查全部（TC） | `A0 0F 02 B1` |
| 查全部（LCUS） | `FF` |

- TC 系列默认 **115200**；LCUS 系列默认 **9600**（本工具默认 9600）。
- TC 系列查询应答为 ASCII `CH1:ON\r\n`；LCUS 2/4 路同样返回文本，8 路返回状态字节。
  `query` 会打印原始 HEX 与解析出的 `CHn=ON/OFF`。
- 查询帧不混发：TC 用 A0 帧，LCUS 用单字节 `FF`。混发时 LCUS 可能把 `FF`
  当作 A0 帧的校验字节而吞掉。`query --style auto`（默认）按波特率选择：
  9600 → LCUS，其它 → TC；可用 `--style tc|lucs` 强制。
- LCUS 只支持查询全部，`--channel` 仅对 TC 风格生效。

## 与 bootloader 测试的关系

- 救援会话要求**冷启动窗口内**收到握手帧：用 `pulse`/`cycle` 把复位/上电与上位机
  发送握手对齐，避免手按复位的时间抖动。
- 续传/半写测试需要在传输中途断电：`cycle` 或外部脚本组合 `off` + 延时 + `on`。
- 串口下载落地前，本工具可独立用于 M1 槽基址启动、M3 跳转/回退的重复复位验证。

## 测试

```sh
python -m unittest discover -s tools/omrelay/tests -v
```

单元测试用假串口对象，不需要接硬件；不发真实继电器命令。
