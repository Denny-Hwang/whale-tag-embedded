# 03. Main Application (`cetiTagApp`) Architecture

Sources: `packages/ceti-tag-data-capture/src/cetiTagApp/`

`cetiTagApp` is a multi-threaded daemon running **a fleet of acquisition threads + a fleet
of logging threads + the mission state machine** in one process. Threads exchange data via
POSIX shared memory and semaphores; the outside world (operators, `cetiHWTest`) talks to
it through named pipes (FIFOs) and the same shared memory.

## 1. Startup sequence (main → running)

Based on `main()` in `launcher.c`:

1. **Logging init** — version string (`_versioning.h`, currently `V2.5.1`) and build
   date/time go to syslog (`openlog("CETI data capture")`)
2. **`core_init()`**:
   - Resolves its own path via `/proc/self/exe` (`g_process_path`) — the base for all
     relative paths
   - Config load: `config/ceti-config.txt` (packaged defaults) then
     `/data/config/ceti-config.txt` (field override; parsed second, so it wins)
   - pigpio init, **FPGA bitstream load** (`config/top.bin`, slave-serial bit-banging)
   - Enables the MAX17320 BMS charge/discharge FETs ("wakes" the tag)
   - Burnwire init (pin output, OFF), **early power-up of the recovery board** (gives the
     STM32 time to boot)
   - Creates 3 core threads: `rtc_thread` (RTC caching), `command_thread` (FIFO command
     handling), `LEDCtrl_thread` (LED status)
3. **`init_tag()` = `threadManager_init()`** — per-sensor hardware init (battery → audio →
   light → IMU → recovery → pressure → ECG → system monitor). Failures set per-thread
   error bits in `s_threads_in_error`, shown as an LED blink code and transmitted over the
   recovery radio as `"THREAD INIT ERR: %04Xh"`
4. **State machine loop** — the main thread runs `stateMachine_task()` at 1 Hz
   ([doc 04](04-state-machine.md))
5. **Exit** — when `g_exit` is set, the loop unwinds, acquisition stops, resources are
   cleaned up. If the exit came from the SHUTDOWN state, the process calls
   `reboot(LINUX_REBOOT_CMD_POWER_OFF)` (the FPGA finishes the full power cut via the BMS)

## 2. Thread map

Acquisition threads are declared in a table in `launcher.c` and managed collectively by
`threadManager_start_acquisition()` / `threadManager_stop_acquisition()`
(`g_stopAcquisition` flag + 30 s timed join, cancel on timeout).

| Thread | Function | CPU pin | Priority | Period |
|---|---|---|---|---|
| Audio acquisition | `audio_thread_spi` | **3** (isolcpus) | SCHED_RR max | ~28 ms per 16 KB SPI block |
| Audio logging | `audio_thread_writeFlac` | 0 | SCHED_RR min | per page (~75 s) |
| ECG acquisition | `ecg_thread_getData` | **2** (isolcpus) | SCHED_RR max | 1 kHz busy-poll |
| ECG logging | `ecg_thread_writeData` | 1 | SCHED_RR min | per page (10 s) |
| ECG leads-off | `ecg_lod_thread` | - | default | 1 ms |
| IMU acquisition | `imu_thread` | 1 | default | 20 ms |
| IMU logging | `imu_log_thread` | - | default | 1 s |
| Light | `light_thread` | 1 | default | 1 s |
| Pressure/temperature | `pressureTemperature_thread` | 1 | default | 1 s |
| Battery | `battery_thread` | 1 | default | 1 s |
| GPS/recovery RX | `recovery_rx_thread` | 1 | default | event-driven |
| System monitor | `systemMonitor_thread` | 0 | default | 10 s |
| Deployment-config snapshot | `meta_log_thread` | - | default | once, 60 s after start |
| (core) RTC cache | `rtc_thread` | - | default | second-boundary polling |
| (core) Command handling | `command_thread` | 0 | default | FIFO blocking + 100 ms |
| (core) LED | `LEDCtrl_thread` | - | default | 250 ms |
| (main) State machine | `stateMachine_thread` (main) | - | default | 1 s |

Design intent: separate **acquisition (real-time, max priority, isolated cores)** from
**disk writing (min priority)**, connected by large double-buffered shared memory, so SD
card latency never costs samples.

## 3. Shared memory / semaphores (`cetiTag.h`)

All names are POSIX (`/dev/shm/...`); the producer (acquisition thread) creates them with
`shm_open(O_CREAT)`+`mmap`, consumers (logging threads, `cetiHWTest`) attach read-only.

| Domain | SHM name | Structure | Semaphores |
|---|---|---|---|
| Audio | `/audio_shm` | `CetiAudioBuffer` double buffer, 3 ch × 14,401,536 B per page ≈ **82.4 MiB total** | `/audio_block_sem` (per block), `/audio_page_sem` (per page) |
| ECG | `/ecg_shm` | 2 pages × 10,000 samples (32 B) ≈ 640 KB | `/ecg_sample_sem`, `/ecg_page_sem` |
| IMU | `/imu_report_buffer_shm` | 2 pages × 340 reports (34 B) ≈ 23 KB | `/imu_report_sem`, `/imu_page_sem` |
| Battery | `/battery_shm` | single `CetiBatterySample` | `/battery_sem` |
| Light | `/light_shm` | single `CetiLightSample` | `/light_sem` |
| Pressure | `/pressure_shm` | single `CetiPressureSample` | `/pressure_sem` |
| Recovery/GPS | `/recovery_shm` | one NMEA sentence (96 B) | `/recovery_sem` |

`cetiTag.h` is the single contract header shared by the daemon and `cetiHWTest`.

The state machine reads `g_pressure` (pressure SHM) and `shm_battery` (battery SHM)
directly, without taking the semaphores (word-sized reads; a pragmatic design choice).

## 4. Command interface (FIFO IPC)

- Pipes: `/opt/ceti-tag-data-capture/ipc/cetiCommand` (write) and `cetiResponse` (read).
  Created by `debian/postinst`.
- Usage: `sendCommand <command...>` (internally
  `echo "$*" > cetiCommand && cat cetiResponse`)
- `command_thread` reads one line from the pipe and dispatches through the table in
  `commands.c`. Unknown commands print the full help to the response pipe.

### Top-level commands (`commands.c`)

| Command | Action |
|---|---|
| `ping` | replies `pong` (liveness) |
| `quit` | stop logging + stop acquisition + exit the process (systemd restarts it after 60 s) |
| `powerdown` | delegates the power-down sequence to the FPGA — after the Pi halts, the FPGA writes the BMS register to cut power fully. A charger is needed to wake the tag again |
| `startDataAcq` / `stopDataAcq` | start/stop all acquisition threads |
| `startLogging` / `stopLogging` | toggle file writing only (`g_stopLogging`) |
| `mission ...` | `pause` / `resume` / `restart` / `setState <name\|number>` — state machine control |
| `audio ...` | `start` / `stop` / `reset` / `sampleRate (48\|96\|192)` |
| `battery ...` | `cellV (0\|1)` / `current` / `reset` (clear temperature latches) / `verify` (check NV registers) |
| `burnwire on/off` | manual burnwire control |
| `imu reset` | reset the IMU |
| `fpga ...` | `config [path]` (reload bitstream) / `reset` / `version` |
| `recovery ...` | recovery board control — [doc 06](06-recovery.md) |
| `network off` | disable networking |

Legacy flat commands (`bwOn`, `checkCell_1`, …) are compiled out via
`ENABLE_LEGACY_COMMANDS 0` (the seeded `.bash_history` cheat sheet still mentions some).

## 5. Configuration file (`utils/config.c`)

Format: one `key = value` per line, `#` comments. Unknown keys/values produce warnings only.

| Key (alias) | Shipped default | Meaning |
|---|---|---|
| `surface_pressure` (`P1`) | 0.3 | surface-detection pressure [bar] (~3 m) |
| `dive_pressure` (`P2`) | 0.5 | dive-detection pressure [bar] (~5 m) |
| `release_voltage` (`V1`) | 6.6 | release-trigger pack voltage [V] — **stored halved (per cell)**, valid 6.2–8.4 |
| `critical_voltage` (`V2`) | 6.200001 | critical voltage [V] (also halved) |
| `timeout_release` (`T0`) | `4d` | deployment timeout → burnwire. Suffixes `s/m/h/d` (⚠ bare numbers default to **minutes**) |
| `burn_interval` (`BT`) | `20m` | burnwire energized duration |
| `time_of_day_release` | (commented out) | fixed-time release (UTC `hh:mm`) |
| `audio_sample_rate` | 96 | kHz. 48/96/192 (0 = 750 Hz low-power) |
| `audio_bitdepth` | 16 | 16 or 24 (the current FPGA bitstream runs 16-bit) |
| `audio_filter` | wideband | `sinc5` or `wideband` |
| `rec_enabled` | true | recovery board enabled |
| `rec_tx_on_whale` | (absent = false) | allow transmitting while attached to the whale |
| `rec_freq` | 145.050 | APRS frequency [MHz] (134.0–174.0) |
| `rec_callsign` / `rec_recipient` | J75Z-2 / KC1QXQ-8 | APRS callsigns |

60 seconds into a deployment, `meta_log_thread` snapshots the **effective config** to
`/data/data_config_<timestamp>.txt` and a copy of `tag-info.yaml` (+ firmware version) to
`/data/data_tag_info_<timestamp>.yaml` — so recorded data can always be paired with its
configuration.

## 6. Timekeeping (`utils/timing.c`)

- At boot: try NTP sync (`ntp_adjtime`) → on success push system time into the RTC and the
  recovery board; on failure pull RTC → system clock (`settimeofday`)
- `rtc_thread` polls the I2C RTC (0x68) aligned to second boundaries and caches it — other
  threads only call `getRtcCount()` and never touch the bus
- Every data row records both `Timestamp [us]` (system epoch) and `RTC Count` (RTC
  seconds) so clock error can be corrected in post-processing

## 7. System monitor (`systemMonitor.c`)

Every 10 s, writes to `/data/data_systemMonitor.csv`: overall + per-core CPU usage,
current CPU placement of 14 tracked threads, RAM/swap headroom, free space of root /
overlay / `/data`, log sizes, CPU/GPU temperature. A flight recorder for post-hoc
performance diagnosis on long deployments.

## 8. Error-handling philosophy

- Sensor failure is **non-fatal**: the exponential backoff in `acq/decay.c` (after 5
  consecutive failures, the sampling period doubles per further failure) keeps a dead I2C
  sensor from hammering the bus
- Thread-init failures are collected in a bitmask, reported via LED blink code + recovery
  radio, and the tag keeps running with whatever subsystems work (only BMS and audio are
  classed "critical")
- If the whole process dies, systemd restarts it after 60 s; the burnwire timeout start
  time survives reboots via `/data/burnwire_timeout_start_time_s.csv`
