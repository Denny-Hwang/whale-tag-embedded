# 03. 메인 애플리케이션(`cetiTagApp`) 아키텍처

소스 위치: `packages/ceti-tag-data-capture/src/cetiTagApp/`

`cetiTagApp`은 하나의 프로세스 안에서 **수집(acquisition) 스레드 무리 + 로깅 스레드 무리 +
상태 머신**을 돌리는 멀티스레드 데몬입니다. 스레드 간 데이터는 POSIX 공유 메모리와
세마포어로 주고받고, 외부(운영자·`cetiHWTest`)와는 명명 파이프(FIFO)와 같은 공유 메모리로
통신합니다.

## 1. 기동 순서 (main → 운용)

`launcher.c`의 `main()` 기준:

1. **로깅 초기화** — syslog(`openlog("CETI data capture")`)에 버전 문자열
   (`_versioning.h`, 현재 `V2.5.1`)과 빌드 일시 출력
2. **`core_init()`**:
   - `/proc/self/exe`로 자기 경로(`g_process_path`)를 해석 — 이후 모든 상대 경로의 기준
   - 설정 로드: `config/ceti-config.txt`(패키지 기본) → `/data/config/ceti-config.txt`
     (현장 오버라이드, 나중에 읽으므로 우선) 순서로 파싱
   - pigpio 초기화, **FPGA 비트스트림 로드** (`config/top.bin`, slave-serial 비트뱅잉)
   - MAX17320 BMS의 충/방전 FET 켜기 (태그 "깨우기")
   - 번와이어 초기화(핀 출력·OFF), **회수 보드 조기 전원 인가**(STM32가 부팅할 시간 확보)
   - 코어 스레드 3개 생성: `rtc_thread`(RTC 캐싱), `command_thread`(FIFO 명령 처리),
     `LEDCtrl_thread`(LED 상태 표시)
3. **`init_tag()` = `threadManager_init()`** — 각 센서 하드웨어 초기화
   (배터리→오디오→조도→IMU→회수→압력→ECG→시스템모니터 순). 실패 시 스레드별 에러 비트를
   `s_threads_in_error`에 기록하고, LED 점멸 코드로 표시 + 회수 보드 무선으로
   `"THREAD INIT ERR: %04Xh"` 송신
4. **상태 머신 루프** — 메인 스레드가 1초 주기로 `stateMachine_task()` 실행
   ([04 문서](04-state-machine.md))
5. **종료** — `g_exit`가 서면 루프 탈출, 수집 중지, 자원 정리. SHUTDOWN 상태로 종료된
   경우 `reboot(LINUX_REBOOT_CMD_POWER_OFF)`로 시스템 전원 차단 (실제 완전 차단은 FPGA가
   BMS에 마무리)

## 2. 스레드 지도

수집 스레드는 `launcher.c`의 테이블로 선언되고 `threadManager_start_acquisition()` /
`threadManager_stop_acquisition()`(g_stopAcquisition 플래그 + 30초 timed-join, 실패 시
cancel)으로 일괄 관리됩니다.

| 스레드 | 함수 | CPU 고정 | 우선순위 | 주기 |
|---|---|---|---|---|
| 오디오 수집 | `audio_thread_spi` | **3** (isolcpus) | SCHED_RR 최고 | 16KB SPI 블록당 ~28ms |
| 오디오 기록 | `audio_thread_writeFlac` | 0 | SCHED_RR 최저 | 페이지(~75초)마다 |
| ECG 수집 | `ecg_thread_getData` | **2** (isolcpus) | SCHED_RR 최고 | 1kHz 바쁜대기 |
| ECG 기록 | `ecg_thread_writeData` | 1 | SCHED_RR 최저 | 페이지(10초)마다 |
| ECG 리드오프 | `ecg_lod_thread` | - | 기본 | 1ms |
| IMU 수집 | `imu_thread` | 1 | 기본 | 20ms |
| IMU 기록 | `imu_log_thread` | - | 기본 | 1초 |
| 조도 | `light_thread` | 1 | 기본 | 1초 |
| 압력/수온 | `pressureTemperature_thread` | 1 | 기본 | 1초 |
| 배터리 | `battery_thread` | 1 | 기본 | 1초 |
| GPS/회수 수신 | `recovery_rx_thread` | 1 | 기본 | 수신 이벤트 |
| 시스템 모니터 | `systemMonitor_thread` | 0 | 기본 | 10초 |
| 배포설정 스냅샷 | `meta_log_thread` | - | 기본 | 시작 60초 후 1회 |
| (코어) RTC 캐시 | `rtc_thread` | - | 기본 | 초 경계 정렬 폴링 |
| (코어) 명령 처리 | `command_thread` | 0 | 기본 | FIFO 블로킹 + 100ms |
| (코어) LED | `LEDCtrl_thread` | - | 기본 | 250ms |
| (메인) 상태 머신 | `stateMachine_thread` (메인) | - | 기본 | 1초 |

설계 의도: **수집(실시간, 최고 우선순위, 격리 코어)** 과 **디스크 기록(최저 우선순위)** 을
분리하고, 그 사이를 큰 더블 버퍼 공유 메모리로 연결해 SD 카드 지연이 샘플 손실로 이어지지
않게 합니다.

## 3. 공유 메모리 / 세마포어 (`cetiTag.h`)

모든 이름은 POSIX(`/dev/shm/...`)이고, 생산자(수집 스레드)가 `shm_open(O_CREAT)`+`mmap`으로
만들고 소비자(기록 스레드, `cetiHWTest`)가 읽기 전용으로 붙습니다.

| 도메인 | SHM 이름 | 구조 | 세마포어 |
|---|---|---|---|
| 오디오 | `/audio_shm` | `CetiAudioBuffer` 더블 버퍼, 페이지당 3ch×14,401,536B ≈ **82.4MiB 총합** | `/audio_block_sem`(블록마다), `/audio_page_sem`(페이지마다) |
| ECG | `/ecg_shm` | 2페이지 × 10,000샘플(32B) ≈ 640KB | `/ecg_sample_sem`, `/ecg_page_sem` |
| IMU | `/imu_report_buffer_shm` | 2페이지 × 340리포트(34B) ≈ 23KB | `/imu_report_sem`, `/imu_page_sem` |
| 배터리 | `/battery_shm` | `CetiBatterySample` 1개 | `/battery_sem` |
| 조도 | `/light_shm` | `CetiLightSample` 1개 | `/light_sem` |
| 압력 | `/pressure_shm` | `CetiPressureSample` 1개 | `/pressure_sem` |
| 회수/GPS | `/recovery_shm` | NMEA 문장 1개(96B) | `/recovery_sem` |

`cetiTag.h`가 데몬과 `cetiHWTest`가 공유하는 단일 계약 헤더입니다.

상태 머신은 `g_pressure`(압력 SHM)와 `shm_battery`(배터리 SHM)를 세마포어 없이 직접
읽습니다 (단일 워드 수준 읽기라 실용적으로 허용한 설계).

## 4. 명령 인터페이스 (FIFO IPC)

- 파이프: `/opt/ceti-tag-data-capture/ipc/cetiCommand`(쓰기), `cetiResponse`(읽기).
  `debian/postinst`가 생성.
- 사용법: `sendCommand <명령...>` (내부적으로 `echo "$*" > cetiCommand && cat cetiResponse`)
- `command_thread`가 파이프에서 한 줄을 읽어 `commands.c`의 테이블로 디스패치. 모르는
  명령이면 전체 도움말을 응답 파이프로 출력.

### 최상위 명령 (`commands.c`)

| 명령 | 동작 |
|---|---|
| `ping` | `pong` 응답 (생존 확인) |
| `quit` | 로깅 중지 + 수집 중지 + 프로세스 종료 (systemd가 60초 후 재시작) |
| `powerdown` | FPGA에 전원 차단 시퀀스 위임 — Pi 정지 후 FPGA가 BMS 레지스터를 써서 완전 차단. 충전기를 물려야 다시 깨어남 |
| `startDataAcq` / `stopDataAcq` | 수집 스레드 일괄 시작/중지 |
| `startLogging` / `stopLogging` | 파일 기록만 켜고/끄기 (`g_stopLogging`) |
| `mission ...` | `pause` / `resume` / `restart` / `setState <상태명\|번호>` — 상태 머신 제어 |
| `audio ...` | `start` / `stop` / `reset` / `sampleRate (48\|96\|192)` |
| `battery ...` | `cellV (0\|1)` / `current` / `reset`(온도 래치 해제) / `verify`(NV 레지스터 검증) |
| `burnwire on/off` | 번와이어 수동 제어 |
| `imu reset` | (⚠ 현재 버그로 오디오 시작에 바인딩됨 — 07 문서 참조) |
| `fpga ...` | `config [경로]`(비트스트림 재로드) / `reset` / `version` |
| `recovery ...` | 회수 보드 제어 — [06 문서](06-recovery.md) |
| `network off` | 네트워킹 비활성화 |

구세대 평면 명령(`bwOn`, `checkCell_1` 등)은 `ENABLE_LEGACY_COMMANDS 0`으로 컴파일에서
제외되어 있습니다 (시딩된 `.bash_history` 치트시트에는 아직 남아 있음).

## 5. 설정 파일 (`utils/config.c`)

형식: `key = value` 한 줄씩, `#` 주석. 알 수 없는 키/값은 경고만 하고 계속 진행.

| 키 (별칭) | 배포 기본값 | 의미 |
|---|---|---|
| `surface_pressure` (`P1`) | 0.3 | 수면 판정 압력 [bar] (~3m) |
| `dive_pressure` (`P2`) | 0.5 | 잠수 판정 압력 [bar] (~5m) |
| `release_voltage` (`V1`) | 6.6 | 방출 트리거 팩 전압 [V] — **내부적으로 ÷2 해서 셀당 저장**, 유효범위 6.2~8.4 |
| `critical_voltage` (`V2`) | 6.200001 | 위기 전압 [V] (동일하게 ÷2) |
| `timeout_release` (`T0`) | `4d` | 배포 타임아웃 → 번와이어. 접미사 `s/m/h/d` (⚠ 접미사 없으면 **분** 단위) |
| `burn_interval` (`BT`) | `20m` | 번와이어 통전 지속 시간 |
| `time_of_day_release` | (주석 처리) | 지정 시각(UTC `hh:mm`) 방출 |
| `audio_sample_rate` | 96 | kHz. 48/96/192 (0이면 750Hz 저전력) |
| `audio_bitdepth` | 16 | 16 또는 24 (현 FPGA 비트스트림은 16-bit 운용) |
| `audio_filter` | wideband | `sinc5` 또는 `wideband` |
| `rec_enabled` | true | 회수 보드 사용 여부 |
| `rec_tx_on_whale` | (미기재=false) | 고래 부착 중에도 송신 허용 여부 |
| `rec_freq` | 145.050 | APRS 주파수 [MHz] (134.0~174.0) |
| `rec_callsign` / `rec_recipient` | J75Z-2 / KC1QXQ-8 | APRS 콜사인 |

배포 시작 60초 후 `meta_log_thread`가 **유효 설정 스냅샷**을
`/data/data_config_<timestamp>.txt`로, `tag-info.yaml` 사본(+펌웨어 버전)을
`/data/data_tag_info_<timestamp>.yaml`로 남깁니다 — 수집 데이터와 설정을 항상 짝지을 수
있게 하는 장치입니다.

## 6. 시간 관리 (`utils/timing.c`)

- 부팅 시: NTP 동기화 시도(`ntp_adjtime`) → 성공하면 시스템 시각을 RTC와 회수 보드에
  밀어 넣음, 실패하면 RTC → 시스템 시각(`settimeofday`)
- `rtc_thread`가 I2C RTC(0x68)를 초 경계에 맞춰 폴링·캐싱 — 다른 스레드는 I2C를 건드리지
  않고 `getRtcCount()`만 호출
- 모든 데이터 행에 `Timestamp [us]`(시스템 epoch)와 `RTC Count`(RTC 초)가 함께 기록되어
  사후에 시계 오차를 보정할 수 있음

## 7. 시스템 모니터 (`systemMonitor.c`)

10초마다 `/data/data_systemMonitor.csv`에 기록: 전체+코어별 CPU 사용률, 추적 스레드
14개의 현재 CPU 배치, RAM/스왑 여유, 루트/오버레이/`/data` 여유 공간, 로그 크기,
CPU/GPU 온도. 장기 배포에서 성능 문제를 사후 진단하기 위한 블랙박스 역할입니다.

## 8. 에러 처리 철학

- 센서 실패는 **치명적이지 않게**: `acq/decay.c`의 지수 백오프(연속 5회 실패 후 주기를
  2배씩 늘림)로 죽은 I2C 센서가 버스를 계속 두드리지 않게 함
- 스레드 초기화 실패는 비트마스크로 모아 LED 점멸 코드 + 회수 무선으로 보고하고,
  가능한 서브시스템만으로 계속 운용 (BMS·오디오 실패만 "치명"으로 분류)
- 프로세스 전체가 죽으면 systemd가 60초 후 재시작하고, 번와이어 타임아웃 기준 시각은
  `/data/burnwire_timeout_start_time_s.csv`로 재부팅을 견디게 설계 (⚠ 관련 버그는 07 문서)
