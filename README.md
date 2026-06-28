# nRF52840 Dongle BLE Central AT Host

[简体中文说明](README.zh-CN.md)

This repository now maintains a single official firmware path:

- `apps/nrf52840_ble_at_host`

Current firmware scope:

- BLE Central AT Host for `nrf52840dongle/nrf52840`
- WLT8016-compatible default profile: `FFF0 / FFF1 / FFF2`
- Single USB CDC port
- Single connection, with `ConnIndex` fixed to `0`
- AT control plane plus high-speed binary data mode

## 5-Minute Quick Start

1. Build the firmware

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_nrf52840_ble_at_host.ps1
```

2. Flash the dongle through the DFU port, for example `COM11`

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\flash_nrf52840_ble_at_host.ps1 -Port COM11
```

3. Find the application port, for example `COM13`

```powershell
nrfutil device list
Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Description | Format-Table -AutoSize
```

4. Verify that the control plane responds

```text
AT
+OK
```

5. Scan, connect, and enter data mode

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

6. Exit data mode and inspect stream statistics

```text
+++
+IND=DATAMODE,0
AT+STREAMSTAT
```

To reflash existing artifacts only:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\flash_nrf52840_ble_at_host.ps1 -Port COM11 -SkipBuild
```

To publish the current firmware archive:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\publish_nrf52840_ble_at_host_firmware.ps1
```

## Minimum Command Loops

Minimum control-plane loop:

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

Minimum data-plane loop:

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

## AT Command Quick Reference

Supported AT commands:

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

Typical events:

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

Default profile:

```text
Service UUID = 0xFFF0
Notify UUID  = 0xFFF1
Write UUID   = 0xFFF2
```

## Data Mode

`AT+ENTERDATAMODE=0` is the primary high-speed mode.

Entry conditions:

- A connection is already established
- The active profile is ready
- Notify and write characteristics have both been discovered
- The write characteristic supports `Write Without Response`

Once entered:

- Every byte from USB CDC is sent as raw binary payload to the current `write_uuid`
- Every BLE notify payload is forwarded unchanged to the same CDC port
- AT parsing is disabled
- Text events such as `+IND=RECV` are no longer emitted
- `0x00`, `CR/LF`, `AT`, and arbitrary binary are all treated as data

Exit sequence:

```text
+++
```

Guard times apply:

- Quiet for about `1000 ms` before `+++`
- Stay quiet for about `1000 ms` after `+++`

Successful exit returns:

```text
+IND=DATAMODE,0
```

The control plane and data plane do not run at the same time. In data mode, the CDC port behaves as a binary bridge between `serial` and the active BLE `notify/write` path.

## Stream Statistics and the Three Rate Layers

Statistics command:

```text
AT+STREAMSTAT
```

Response format:

```text
+OK=H2B,<bytes>,B2H,<bytes>,WOP,<count>,NOP,<count>,BP,<count>,MAXTX,<depth>,MAXRX,<depth>,ENT,<count>,UDISC,<count>
```

Field meanings:

- `H2B`: total Host -> BLE bytes
- `B2H`: total BLE -> Host bytes
- `WOP`: BLE write operation count
- `NOP`: notify count
- `BP`: backpressure count
- `MAXTX`: maximum Host -> BLE queue depth
- `MAXRX`: maximum BLE -> Host queue depth
- `ENT`: data-mode entry count
- `UDISC`: unexpected disconnect count

All throughput evaluation should be split into three layers:

- `PC -> dongle`
  Use `H2B / WOP`. This only proves that host bytes entered the dongle and were submitted as BLE writes.
- `peer notify -> dongle -> PC`
  Use `B2H / NOP`. This only proves that peer notify traffic came back through CDC.
- `peer application actually received and confirmed`
  Use Python end-to-end echo statistics. Do not treat `H2B` or a serial tool's `Bps` display as end-to-end proof.

In other words:

- A good `H2B` rate does not prove that the peer application accepted full frames.
- A high `B2H` rate does not prove that the peer is sending frame-mirrored echoes.

## End-to-End Stress Test

Dedicated script:

- `python/examples/e2e_stream_test.py`

What it does:

- Scans for the requested BLE target name and connects by device name
- Tries `AT` resynchronization first when the control plane is out of sync; falls back to `+++` recovery if needed
- Sends fixed `190B` frames in data mode
- Adds `seq`, `monotonic_ms`, and `payload_len` in the frame header

Supported modes:

- `blind_rate`
  Sends at a fixed rate without waiting for echoes
- `echo_gated`
  Stops pushing new frames once the unconfirmed frame count exceeds the limit

Recommended commands:

```powershell
python .\python\examples\e2e_stream_test.py --port COM13 --target-name <BLE_NAME> --duration 60 --mode blind_rate --hz 20 --frame-size 190
python .\python\examples\e2e_stream_test.py --port COM13 --target-name <BLE_NAME> --duration 60 --mode echo_gated --hz 20 --frame-size 190 --max-outstanding 1
```

Reported outputs:

- `tx_frames`
- `rx_frames`
- `lost_frames`
- `reordered_frames`
- `tx_bps`
- `rx_bps`
- `avg_rtt_ms`
- `max_rtt_ms`

How to interpret results:

- If `H2B` is good but `rx_frames = 0` while `B2H > 0`, the peer is sending notify data but not frame-mirrored echoes.
- If `blind_rate` drops frames but `echo_gated` stabilizes, the local path is mostly fine and the bottleneck is likely peer-side processing or echo behavior.
- If both modes remain unstable, go back to firmware scheduling or peer protocol debugging.

## Known Limits

- Single connection only
- `ConnIndex` is fixed to `0`
- In AT mode, `+IND=RECV` is suitable only for printable ASCII
- Use data mode for arbitrary binary streams
- On data-mode exit, control-plane recovery takes priority over preserving every queued BLE -> Host tail byte

## Repository Entry Points

- `apps/nrf52840_ble_at_host/`
  Firmware source
- `firmware/nrf52840_ble_at_host/`
  Published firmware archive
- `python/ble_at.py`
  Python helper
- `python/examples/`
  Scan, loop, and stress examples
- `scripts/build_nrf52840_ble_at_host.ps1`
  Official build entry
- `scripts/flash_nrf52840_ble_at_host.ps1`
  Official flash entry
- `scripts/publish_nrf52840_ble_at_host_firmware.ps1`
  Official publish entry

## Reference Material

Private local reference materials may exist outside version control for protocol or module lookup, but they are not part of the public project entry points.
