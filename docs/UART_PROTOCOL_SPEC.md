# UART Protocol Spec — STM32 ↔ ESP32 binary link

**Version:** 1.0 (phases 1–9 implemented)
**Physical layer:** 460800 baud, 8N1, no flow control
**Wiring:** STM32 PA2 (TX) → ESP32 GPIO18 (RX) · STM32 PA3 (RX) ←
ESP32 GPIO17 (TX) · GND ↔ GND (mandatory; both sides are 3V3)

The reference implementation lives in `Protocol/` (ESP32) and
`Application/{Inc,Src}` (STM32) — the four protocol files
(`crc16.{c,h}`, `packet.{c,h}`, `protocol_types.h`) are **byte-identical
on both MCUs** by design: any divergence is a defect.

---

## 1. Frame format

```
[0xAA] [len] [type] [seq_lo] [seq_hi] [payload...] [crc_lo] [crc_hi] [0x55]
```

| Field | Size | Meaning |
|-------|------|---------|
| header | 1 | always `0xAA` |
| len | 1 | payload length, 0..128 |
| type | 1 | message type (§3) |
| seq | 2 | per-stream sequence number, little-endian, wraps 65535→0 |
| payload | 0..128 | type-specific struct (§4) |
| crc | 2 | CRC-16/CCITT-FALSE over header+len+type+seq+payload, LE |
| footer | 1 | always `0x55` |

Max frame = 8 + 128 = **136 bytes**. All multi-byte payload fields are
little-endian (both cores are native LE — no swapping).

CRC-16/CCITT-FALSE: poly `0x1021`, init `0xFFFF`, no reflection, no
final XOR. Check values: `""→0xFFFF`, `"123456789"→0x29B1`.

## 2. Streaming rules

- The decoder is a **streaming state machine** (`packet_decode_byte`):
  feed one byte at a time; it resynchronises after garbage, CRC errors
  and framing loss by returning to `WAIT_HEADER`.
- **Telemetry is fire-and-forget.** A frame lost to a CRC error costs
  one sample period (≤ 1 s); the next frame replaces it, so a
  retransmission protocol would cost more than it saves.
- **Commands are acknowledged** (`MSG_ACK`/`MSG_NAK`, §5): rare,
  state-changing, must be confirmed. The ESP32 retries up to 3 times
  with a bounded timeout (500 ms per try interactive, 300 ms HTTP).
- Heartbeats are **not** acknowledged.
- Sequence spaces: ESP32 heartbeats count from 0; ESP32 commands count
  from `0x8000`; the STM32 uses separate counters for telemetry and
  its heartbeat. The ACK payload echoes `(seq,type)` of the acked
  frame, so streams cannot be confused.

## 3. Message types

| Type | Value | Direction | Payload |
|------|-------|-----------|---------|
| MSG_HEARTBEAT | 0x01 | both | 0 B |
| MSG_TELEMETRY | 0x02 | STM32→ESP32 | 20 B |
| MSG_ALARM | 0x03 | STM32→ESP32 | 8 B |
| MSG_ACK | 0x04 | both | 4 B |
| MSG_NAK | 0x05 | both | 4 B |
| MSG_CMD_SET_THRESHOLD | 0x10 | ESP32→STM32 | 8 B |
| MSG_CMD_SET_SAMPLE_RATE | 0x11 | ESP32→STM32 | 4 B |
| MSG_CMD_LED_ON | 0x12 | ESP32→STM32 | 2 B |
| MSG_CMD_LED_OFF | 0x13 | ESP32→STM32 | 2 B |
| MSG_CMD_BUZZER_OFF | 0x14 | ESP32→STM32 | 0 B |
| MSG_CMD_RESET_ALARM | 0x15 | ESP32→STM32 | 0 B |
| MSG_CMD_REBOOT | 0x16 | ESP32→STM32 | 0 B |
| MSG_CMD_GET_STATUS | 0x17 | ESP32→STM32 | 0 B |
| MSG_RESP_STATUS | 0x80 | STM32→ESP32 | 32 B |
| MSG_DEBUG_LOG | 0xFE | both (STM32 in practice) | 1..97 B |
| MSG_RESET | 0xFF | both | 1 B |

## 4. Payload structs (packed, `#pragma pack(1)`)

### MSG_TELEMETRY (20 B)
| Offset | Type | Field |
|--------|------|-------|
| 0 | float | temperature_c — filtered °C |
| 4 | float | current_a — filtered A (rectified) |
| 8 | float | vibration_g — **estimate** (§7) |
| 12 | u8 | alarm_bits (§6) |
| 13 | u8 | sensor_status (bit0 DS18B20 ok, bit1 MPU6050 ok, bit2 INA219 ok) |
| 14 | u16 | cpu_load_pct — 0..10000 = 0..100.00 % |
| 16 | u16 | free_heap_bytes / 16 |
| 18 | u16 | uptime_seconds (wraps 18.2 h) |

### MSG_ALARM (8 B)
| Offset | Type | Field |
|--------|------|-------|
| 0 | u8 | alarm_bit (one bit, §6) |
| 1 | u8 | state — 1 = raised, 0 = cleared |
| 2 | i16 | value_at_trigger — ×100 (two decimals) |
| 4 | u32 | STM32 FreeRTOS tick at trigger |

### MSG_ACK (4 B) / MSG_NAK (4 B)
| Offset | Type | Field |
|--------|------|-------|
| 0 | u16 | seq of the frame being acked/nak'd |
| 2 | u8 | type of that frame |
| 3 | u8 | reserved (0) |

NAK reason codes: 0 CRC fail · 1 unknown type · 2 bad length/value ·
3 out of memory.

### MSG_CMD_SET_THRESHOLD (8 B)
`{u8 id, u8 rsv, float value, u8 rsv2[2]}`
id: 0 temperature (°C, 0..125) · 1 current (A, 0..30) · 2 vibration
(estimated g, 0..10).

### MSG_CMD_SET_SAMPLE_RATE (4 B)
`{u16 period_ms, u8 rsv[2]}` — 50..1000 ms.

### MSG_CMD_LED_ON / OFF (2 B)
`{u8 id, u8 rsv}` — id 0 green · 1 yellow · 2 red · 3 all.
**Board mapping:** green = PC13 LED · red = PB1 relay (industrial
trip) · yellow = not wired (ack'd, logged, no-op).

### MSG_RESP_STATUS (32 B)
Filtered temp/cur/vib (3×4) · alarm_bits u8 · sensor_status u8 ·
cpu_load u8 (0..100) · task_states u8 (0 here) · free_heap/16 u16 ·
uptime u16 · current thresholds temp/cur/vib (3×4 float).

### MSG_DEBUG_LOG (1 + n)
`{u8 level, char msg[]}` — level 0 INFO / 1 WARN / 2 ERROR; msg is
NUL-free ASCII, up to 96 bytes.

### MSG_RESET (1 B)
`{u8 reset_reason}` — 0 PWRON · 1 external · 2 watchdog · 3 software ·
4 brownout · 5 panic.

## 5. Command exchange

```
ESP32                              STM32
  |--- MSG_CMD_x (seq=0x8000+n) --->|
  |                                  | apply, then
  |<-------- MSG_ACK (seq,type) -----|        success
  |   ... or ...
  |<-------- MSG_NAK (reason) -------|        rejected value
  |<--- (nothing) ------------------>|        silent: retry x3, give up
```

The STM32 ACKs *after* applying the change; on REBOOT it ACKs first,
then resets 100 ms later so the ACK can escape the wire.

## 6. Alarm bits

| Bit | Mask | Meaning | Auto-action on the STM32 |
|-----|------|---------|--------------------------|
| 0 | 0x01 | OVERTEMP | relay trip + buzzer ≤3 s |
| 1 | 0x02 | OVERCURRENT | relay trip + buzzer ≤3 s |
| 2 | 0x04 | VIBRATION | buzzer ≤3 s |
| 3 | 0x08 | SENSOR_FAIL (DS18B20) | — |
| 4 | 0x10 | COMM_FAIL (either side) | — |

Hysteresis: raise after ≥1.5 s above threshold, clear after ≥3 s below
`threshold × 0.95` (temp) / `× 0.9` (others). `MSG_CMD_RESET_ALARM`
acknowledges: clears all alarm state, releases the relay, silences the
buzzer; still-active conditions re-raise after the confirm window.

## 7. Units & honesty notes

- **Vibration is an estimate.** The SW-420 is a digital contact; the
  firmware reports debounced edges/s ÷ 50 as "g" (calibrate
  `VIB_EDGES_PER_G` in `app_telemetry.c`). The dashboard labels it
  "est. g"; the vibration threshold travels in the same units so
  values and limits stay comparable.
- ACS712 current is rectified (|A|) and EMA-filtered; the scaling
  constants live in `bringup_config.h` and are bench-uncalibrated.
- `value_at_trigger` is ×100 scaled in the alarm payload for
  temperature/current/vibration; sensor/comm alarms carry counts.

## 8. Heartbeat + comm-fail

- STM32 → ESP32 heartbeat: 1 Hz (independent of telemetry rate).
- ESP32 → STM32 heartbeat: 1 Hz.
- **Comm-fail:** 5 s without any valid frame flips the alarm on either
  side; recovery flips it off. The ESP32 additionally escalates
  (§`task_watchdog.c`): 60 s silent → `MSG_CMD_REBOOT`; 120 s → ESP32
  self-reboot. Never-linked STM32s are exempt.
