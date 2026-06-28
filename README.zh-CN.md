# nRF52840 Dongle BLE Central AT Host

[English README](README.md)

这是仓库内唯一正式维护的方案：

- `apps/nrf52840_ble_at_host`

当前固件定位：

- 基于 `nrf52840dongle/nrf52840` 的 BLE Central AT Host
- 默认兼容 WLT8016 的 `FFF0 / FFF1 / FFF2` Profile
- 单 USB CDC 串口
- 单连接，`ConnIndex` 固定为 `0`
- 支持 AT 控制面和高速二进制数据模式

## 5 分钟上手

1. 构建固件

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_nrf52840_ble_at_host.ps1
```

2. 刷写到 Dongle 的 DFU 口，例如 `COM11`

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\flash_nrf52840_ble_at_host.ps1 -Port COM11
```

3. 查应用串口，例如 `COM13`

```powershell
nrfutil device list
Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Description | Format-Table -AutoSize
```

4. 串口连上后，先确认控制面正常

```text
AT
+OK
```

5. 扫描、连接、进入数据模式

```text
AT+STARTSCAN=3,SITE
+IND=SCDA,<index>,<mac>,<name>,<addr_type>,<rssi>
+IND=SCED

AT+STARTCON=<mac>,<addr_type>
+OK
+IND=BLMC,0
+IND=MTU,<payload_len>
+IND=BLMN,0

AT+ENTERDATAMODE=0
+OK
+IND=DATAMODE,1
```

6. 退出数据模式并查看统计

```text
+++
+IND=DATAMODE,0
AT+STREAMSTAT
```

如果只想重刷现有产物：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\flash_nrf52840_ble_at_host.ps1 -Port COM11 -SkipBuild
```

如果要发布当前固件归档：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\publish_nrf52840_ble_at_host_firmware.ps1
```

## 最小命令闭环

控制面最小闭环：

```text
AT
AT+GETSVCUUID
AT+STARTSCAN=3,SITE
AT+STARTCON=<mac>,<addr_type>
AT+CONSTATUS
AT+SEND=0,4
>
PING
AT+DISCON=0
```

数据面最小闭环：

```text
AT
AT+STARTSCAN=3,SITE
AT+STARTCON=<mac>,<addr_type>
AT+ENTERDATAMODE=0
<raw binary stream>
+++
AT+STREAMSTAT
AT+DISCON=0
```

## 常用命令速查

支持的 AT 命令：

```text
AT
AT+STARTSCAN=<sec>[,<name_prefix>]
AT+STARTCON=<mac>,<addr_type>
AT+DISCON=0
AT+SEND=0,<len>
AT+CONSTATUS
AT+SETSVCUUID=<service>,<notify>,<write>
AT+GETSVCUUID
AT+SETMTU=<mtu>
AT+GETMTU
AT+CONMTUREQ=0
AT+CONPARAREQ=0,<min>,<max>,<latency>,<timeout>
AT+ENTERDATAMODE=0
AT+STREAMSTAT
```

典型事件：

```text
+IND=SCDA,<index>,<mac>,<name>,<addr_type>,<rssi>
+IND=SCED
+IND=BLMC,0
+IND=MTU,<payload_len>
+IND=BLMN,0
+IND=BLMD,0
+IND=RECV,0,<len>,<ascii_payload>
+IND=DATAMODE,1
+IND=DATAMODE,0
```

默认 Profile：

```text
Service UUID = 0xFFF0
Notify UUID  = 0xFFF1
Write UUID   = 0xFFF2
```

## 数据模式

`AT+ENTERDATAMODE=0` 是当前主功能。

进入条件：

- 已连接
- Profile 已 ready
- 已找到 notify / write characteristic
- write characteristic 支持 `Write Without Response`

进入后：

- 串口收到的所有字节都作为原始二进制 payload 写到当前 `write_uuid`
- BLE notify 收到的所有字节都原样输出到同一个 CDC 串口
- 不再解析 `AT` 命令
- 不再输出 `+IND=RECV` 文本事件
- `0x00`、`CR/LF`、`AT`、随机字节都按数据处理

退出规则：

```text
+++
```

但必须满足静默保护时间：

- `+++` 前静默约 `1000 ms`
- `+++` 后继续静默约 `1000 ms`

退出成功后会返回：

```text
+IND=DATAMODE,0
```

控制面和数据面不会同时工作。处于数据模式时，这个 CDC 串口就是 `串口 <-> BLE notify/write` 的二进制透传通道。

## 流统计与三层速率口径

统计命令：

```text
AT+STREAMSTAT
```

返回格式：

```text
+OK=H2B,<bytes>,B2H,<bytes>,WOP,<count>,NOP,<count>,BP,<count>,MAXTX,<depth>,MAXRX,<depth>,ENT,<count>,UDISC,<count>
```

字段含义：

- `H2B`: Host -> BLE 总字节数
- `B2H`: BLE -> Host 总字节数
- `WOP`: BLE write 操作次数
- `NOP`: notify 次数
- `BP`: 背压计数
- `MAXTX`: Host -> BLE 队列最大深度
- `MAXRX`: BLE -> Host 队列最大深度
- `ENT`: 进入数据模式次数
- `UDISC`: 非预期断连次数

后续测速统一拆成 3 层：

- `PC -> dongle`
  以 `H2B / WOP` 为准，只能证明主机数据已经进入 dongle 并提交成 BLE write
- `peer notify -> dongle -> PC`
  以 `B2H / NOP` 为准，只能证明对端 notify 已经回到 CDC
- `peer 应用层真正收齐并确认`
  以 Python 端到端回显统计为准，不能只看 `H2B` 或串口工具显示的 `Bps`

也就是说：

- `H2B` 达标，不等于对端业务层已经按同样帧格式收齐
- `B2H` 很高，也不等于对端返回的是整帧镜像回显

## 端到端压测

专用脚本：

- `python/examples/e2e_stream_test.py`

作用：

- 按指定的 BLE 目标名称扫描并按设备名连接
- 若控制面不同步，会先尝试 `AT` 重同步；仍失败时自动尝试 `+++` 恢复
- 进入数据模式后发送固定 `190B` 帧
- 帧头包含 `seq`、`monotonic_ms`、`payload_len`

支持两种模式：

- `blind_rate`
  固定节拍发送，不等回显
- `echo_gated`
  未确认帧数超过阈值前不继续推新帧

推荐命令：

```powershell
python .\python\examples\e2e_stream_test.py --port COM13 --target-name <BLE_NAME> --duration 60 --mode blind_rate --hz 20 --frame-size 190
python .\python\examples\e2e_stream_test.py --port COM13 --target-name <BLE_NAME> --duration 60 --mode echo_gated --hz 20 --frame-size 190 --max-outstanding 1
```

脚本输出：

- `tx_frames`
- `rx_frames`
- `lost_frames`
- `reordered_frames`
- `tx_bps`
- `rx_bps`
- `avg_rtt_ms`
- `max_rtt_ms`

判读规则：

- 如果 `H2B` 达标，但 `rx_frames = 0` 且 `B2H > 0`，说明对端确实在回 notify，但不是整帧镜像回显
- 如果 `blind_rate` 下掉帧，而 `echo_gated` 恢复稳定，说明本端链路基本打通，对端业务处理或回显策略才是瓶颈
- 如果两种模式都不稳定，再回到固件调度或对端协议实现继续排查

## 已知限制

- 只支持单连接
- `ConnIndex` 固定为 `0`
- `AT` 模式下 `+IND=RECV` 仅适合可打印 ASCII
- 任意二进制透传请使用数据模式
- 退出数据模式时优先保证控制面恢复，不保证保留退出瞬间仍在队列里的全部 BLE -> Host 尾包

## 仓库入口

- `apps/nrf52840_ble_at_host/`
  固件源码
- `firmware/nrf52840_ble_at_host/`
  当前正式固件归档
- `python/ble_at.py`
  Python 控制辅助
- `python/examples/`
  扫描、循环测试、压测示例
- `scripts/build_nrf52840_ble_at_host.ps1`
  正式构建入口
- `scripts/flash_nrf52840_ble_at_host.ps1`
  正式刷写入口
- `scripts/publish_nrf52840_ble_at_host_firmware.ps1`
  正式发布入口

## 参考资料

协议或模组相关的私有本地参考资料可以单独保留在版本库之外，但不作为项目公开入口的一部分。
