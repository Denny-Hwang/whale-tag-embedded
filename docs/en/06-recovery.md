# 06. Recovery System — GPS · Argos · APRS · LEDs

## 1. What the recovery board is

A **separate STM32-based board** that makes the tag findable once it has detached from
the whale. It does three things:

1. Receives GPS → streams raw NMEA sentences to the Pi over UART
2. Transmits position/messages by radio — **Argos satellite** (current build) or **APRS
   VHF** (older board generation)
3. Monitors a critical-voltage threshold pushed down from the Pi

The board generation is a compile-time choice (`recovery.h`):

```c
#define RECOVERY_BOARD_TYPE RECOVERY_BOARD_TYPE_ARGOS   // current default
```

All APRS code remains behind `#if`s; the clear recent direction is the **APRS → Argos
satellite transition** (PR #119 "Argos Recovery Board Integration", Dec 2025).

## 2. Physical interface

| Item | Details |
|---|---|
| Power | gated by I/O expander (0x21) bit 2 (`3V3_RF_EN`) |
| Bootloader | bit 1 (`BOOT0`) — resetting with it high enters the STM32 system bootloader |
| UART | `/dev/serial0`, **115200 8N1** (pigpio serial) |
| Firmware flashing | `ipc/flashRecovery.sh` — stop the service → BOOT0/power sequence → `stm32flash` (~50 s) |

At boot, `core_init()` powers the board **very early** — so the STM32 boots in parallel
with the rest of the tag's initialization (the "recovery startup timing" work in commits
#121/#122). Later, `recovery_thread_init()` pings until it succeeds, restarting the board
between attempts (up to 5 restarts, then it gives up with a hardware error — the earlier
infinite-retry bug has been fixed, see doc 07 issue 3).

## 3. Serial protocol (the `lib/libCetiRecovery` submodule)

The protocol definition header **shared between the Pi and the recovery board** is
factored into a separate repository, `Project-CETI/libCetiRecovery`, pulled in as a
submodule (header-only).

**Framing**: `'$'` (0x24) start byte + 4-byte header + up to 255 bytes of payload,
little-endian, **no checksum** (a reserved byte is earmarked for a future CRC).

```
[key='$'] [type] [length] [reserved] [payload ...]
```

**Command blocks** (main ones):

| Range | Contents |
|---|---|
| 0x01–0x04 | State control: START (GPS + TX), STOP (idle), COLLECT_ONLY (GPS only), PROGRAM_ARRIBADA |
| 0x10–0x13 | Data/liveness: NMEA_PACKET (board→Pi), MESSAGE, PING, PONG |
| 0x20 | Critical voltage config (float) |
| 0x21–0x28 | APRS config: VHF power, frequency, callsign, comment, SSID, recipient, hostname |
| 0x29–0x2C | Argos config: ID, address, secret key, modulation (LDA2/VLDA4/LDK/LDA2L) |
| 0x30 | RTC time set `{yy,mm,dd,HH,MM,SS}` — pushed whenever the Pi successfully syncs NTP |
| 0x60–0x6C | Config queries (responses come back with the corresponding 0x2x **config** opcode) |

Reception is handled by `recovery_rx_thread` (the "gps acquisition" thread) with a
`'$'`-resynchronizing parser; NMEA packets are timestamped into the `/recovery_shm` shared
memory and appended verbatim to `/data/data_gps.csv`.

## 4. Recovery board power/mode vs. mission state

| Mission state | Recovery mode | Rationale |
|---|---|---|
| RECORD_DIVING | `sleep` (STOP) | GPS/radio are useless underwater → save power |
| RECORD_SURFACE | `gps_only` (COLLECT_ONLY) | log position but **never transmit while on the whale** |
| Everything else (START, PREDEPLOY, FLOATING, BRN_ON, LOW_POWER_BURN, RETRIEVE, SHUTDOWN) | `wake` (START) | detach/release/retrieve phases — beacon at full duty |

- On entering START, the tag transmits a boot announcement `"CETI <hostname> ready!"`.
- On APRS builds, every state transition updates the APRS comment to
  `"<hostname> <STATE>"`.
- The power rail itself is never cut by the state machine — only on init
  failure/disabled-config or a manual `recovery off`. Even in LOW_POWER_BURN/SHUTDOWN the
  board stays awake: **an abandoned tag needs its beacon most**.
- Message length limits: 67 chars for APRS, **24 chars for Argos** (shared limit
  `RECOVERY_BOARD_MAX_MSG_LENGTH` in `recovery.h`).
- On thread-init failure the tag radios its error bitmask (`"THREAD INIT ERR: %04Xh"`) —
  a pre-attachment wireless health check.

## 5. Operator commands (`sendCommand recovery ...`)

| Command | Action |
|---|---|
| `recovery on` / `off` | apply/cut 3V3 RF power |
| `recovery wake` / `sleep` | START / STOP mode |
| `recovery ping` | PING→PONG round trip |
| `recovery message "…"` | free-form TX |
| `recovery timesync` | push system UTC to the board RTC |
| `recovery address <8 hex \| ?>` | Argos address set/get |
| `recovery id <6 digits \| ?>` | Argos ID |
| `recovery modulation <LDA2\|VLDA4\|LDK\|LDA2L \| ?>` | Argos modulation |
| `recovery secret_key <32 hex \| ?>` | Argos secret key |
| (APRS builds) `setFrequency` / `setCallsign` / `setRecipient` | VHF config |

## 6. LED indications (`led_ctrl.c`)

Three LEDs (green/yellow/red) driven through the FPGA; a dedicated thread updates them on
a 250 ms tick.

| LED state | When | Pattern |
|---|---|---|
| FPGA control (default) | PREDEPLOY, RECORD_SURFACE, FLOATING | LEDs handed back to the FPGA + activity LED on |
| DIVE | RECORD_DIVING, RETRIEVE | **one 250 ms green blink every 10 s** (power saving) |
| BURN | BRN_ON | red→yellow→green 4 Hz rotating chase |
| SHUTDOWN | LOW_POWER_BURN, SHUTDOWN | all off |
| Error report | on init failure | yellow = clock, **red = critical error bit, green = warning bit**, shifted LSB-first (bit = failing acquisition thread), held 20 s, then back to the previous state |

So right after boot, reading the LED blink code in the field tells you which subsystem
failed to initialize (red = critical for BMS/audio failures).

## 7. APRS callsign utility (`aprs.c`)

A dependency-free module converting `"KC1TUJ-12"`-style strings to/from
`{callsign[7], ssid}` (1–6 alphanumerics + SSID 0–15). Still used for config
parsing/logging in the Argos build. One of the few unit-tested modules.
