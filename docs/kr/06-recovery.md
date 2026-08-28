# 06. 회수(Recovery) 시스템 — GPS · Argos · APRS · LED

## 1. 회수 보드란

태그가 고래에서 분리된 뒤 **바다 위에서 찾을 수 있게 해주는 STM32 기반 별도 보드**입니다.
역할은 세 가지:

1. GPS 수신 → 원시 NMEA 문장을 UART로 Pi에 전달
2. 위치/메시지 무선 송신 — **Argos 위성**(현재 빌드) 또는 **APRS VHF**(구세대 보드)
3. Pi가 내려준 위기 전압 임계값 감시

보드 세대는 컴파일 타임에 선택합니다 (`recovery.h`):

```c
#define RECOVERY_BOARD_TYPE RECOVERY_BOARD_TYPE_ARGOS   // 현재 기본
```

APRS 코드는 전부 `#if`로 남아 있고, 최근 개발 방향은 명확히 **APRS → Argos 위성 전환**
입니다 (2025-12 "Argos Recovery Board Integration" PR #119).

## 2. 물리 인터페이스

| 항목 | 내용 |
|---|---|
| 전원 | I/O 익스팬더(0x21) 비트 2 (`3V3_RF_EN`)로 게이팅 |
| 부트로더 | 비트 1 (`BOOT0`) — high로 리셋하면 STM32 시스템 부트로더 진입 |
| UART | `/dev/serial0`, **115200 8N1** (pigpio serial) |
| 펌웨어 굽기 | `ipc/flashRecovery.sh` — 서비스 정지 → BOOT0/전원 시퀀스 → `stm32flash` (~50초) |

부팅 시 `core_init()`이 **아주 이른 시점에** 보드 전원을 켭니다 — 태그의 나머지 초기화가
도는 동안 STM32가 부팅을 마치도록 (최근 커밋 #121/#122의 "recovery startup timing" 작업).
이후 `recovery_thread_init()`이 ping이 성공할 때까지 재시도/보드 재시작합니다.

## 3. 직렬 프로토콜 (`lib/libCetiRecovery` 서브모듈)

Pi와 회수 보드가 **공유하는 프로토콜 정의 헤더**(header-only)가 별도 저장소
`Project-CETI/libCetiRecovery`로 분리되어 서브모듈로 들어옵니다.

**프레이밍**: `'$'`(0x24) 시작 바이트 + 4바이트 헤더 + 최대 255바이트 페이로드,
리틀엔디언, **체크섬 없음**(예약 바이트가 향후 CRC 자리).

```
[key='$'] [type] [length] [reserved] [payload ...]
```

**명령 블록** (주요):

| 범위 | 내용 |
|---|---|
| 0x01–0x04 | 상태 제어: START(GPS+송신), STOP(휴면), COLLECT_ONLY(GPS만), PROGRAM_ARRIBADA |
| 0x10–0x13 | 데이터/생존: NMEA_PACKET(보드→Pi), MESSAGE, PING, PONG |
| 0x20 | 위기 전압 설정 (float) |
| 0x21–0x28 | APRS 설정: VHF 출력, 주파수, 콜사인, 코멘트, SSID, 수신자, 호스트명 |
| 0x29–0x2C | Argos 설정: ID, 주소, 시크릿 키, 변조 방식 (LDA2/VLDA4/LDK/LDA2L) |
| 0x30 | RTC 시각 설정 `{yy,mm,dd,HH,MM,SS}` — Pi가 NTP 동기화에 성공할 때마다 푸시 |
| 0x60–0x6C | 설정 조회 (응답은 대응하는 0x2x **설정 opcode**로 돌아옴) |

수신은 `recovery_rx_thread`("gps acquisition" 스레드)가 `'$'` 재동기화 파서로 처리하며,
NMEA 패킷은 타임스탬프를 붙여 `/recovery_shm` 공유 메모리에 넣고 `/data/data_gps.csv`에
원문 그대로 기록합니다.

## 4. 미션 상태와 회수 보드 전원/모드

| 미션 상태 | 회수 보드 모드 | 이유 |
|---|---|---|
| RECORD_DIVING | `sleep` (STOP) | 수중에서는 GPS/무선이 무의미 → 절전 |
| RECORD_SURFACE | `gps_only` (COLLECT_ONLY) | 위치는 기록하되 **고래에 붙은 채로 송신 금지** |
| 그 외 전부 (START, PREDEPLOY, FLOATING, BRN_ON, LOW_POWER_BURN, RETRIEVE, SHUTDOWN) | `wake` (START) | 분리·방출·회수 국면 — 비컨 최대 가동 |

- START 진입 시 `"CETI <호스트명> ready!"` 부팅 알림을 무선으로 송신.
- APRS 빌드에서는 상태 전이마다 APRS 코멘트를 `"<호스트명> <상태>"`로 갱신.
- 전원 레일 자체는 상태 머신이 끄지 않습니다 — 초기화 실패/설정 비활성/수동
  `recovery off`일 때만 차단. LOW_POWER_BURN/SHUTDOWN에서도 보드는 깨어 있는데,
  **버려지는 태그일수록 비컨이 살아 있어야 하기 때문**입니다.
- 메시지 길이 제한: APRS 67자, Argos **24자**.
- 스레드 초기화 실패 시 태그는 에러 비트마스크를 무선으로 쏩니다
  (`"THREAD INIT ERR: %04Xh"`) — 부착 전 무선 헬스체크 용도.

## 5. 운영 명령 (`sendCommand recovery ...`)

| 명령 | 동작 |
|---|---|
| `recovery on` / `off` | 3V3 RF 전원 인가/차단 |
| `recovery wake` / `sleep` | START / STOP 모드 |
| `recovery ping` | PING→PONG 왕복 확인 |
| `recovery message "텍스트"` | 임의 메시지 송신 |
| `recovery timesync` | 시스템 UTC를 보드 RTC로 푸시 |
| `recovery address <8자리 hex \| ?>` | Argos 주소 설정/조회 |
| `recovery id <6자리 숫자 \| ?>` | Argos ID |
| `recovery modulation <LDA2\|VLDA4\|LDK\|LDA2L \| ?>` | Argos 변조 방식 |
| `recovery secret_key <32자리 hex \| ?>` | Argos 시크릿 키 |
| (APRS 빌드) `setFrequency` / `setCallsign` / `setRecipient` | VHF 설정 |

## 6. LED 표시 (`led_ctrl.c`)

녹/황/적 3색 LED를 FPGA 경유로 제어하며, 전용 스레드가 250ms 틱으로 갱신합니다.

| LED 상태 | 언제 | 패턴 |
|---|---|---|
| FPGA 제어(기본) | PREDEPLOY, RECORD_SURFACE, FLOATING | LED를 FPGA에 되돌려 줌 + 활동 LED 켬 |
| DIVE | RECORD_DIVING, RETRIEVE | **10초에 1번 녹색 250ms** (절전) |
| BURN | BRN_ON | 적→황→녹 4Hz 회전 체이스 |
| SHUTDOWN | LOW_POWER_BURN, SHUTDOWN | 전체 소등 |
| 에러 리포트 | 초기화 실패 시 | 황색=클럭, **적색=치명 에러 비트, 녹색=경고 비트**를 LSB부터 점멸(비트 = 실패한 수집 스레드 번호), 20초 유지 후 원래 상태로 복귀 |

즉, 현장에서 부팅 직후 LED 점멸 코드를 읽으면 어떤 서브시스템 초기화가 실패했는지 알 수
있습니다 (BMS·오디오 실패 시 적색 = 치명).

## 7. APRS 콜사인 유틸 (`aprs.c`)

`"KC1TUJ-12"` 형태의 문자열 ↔ `{callsign[7], ssid}` 구조체 변환만 담당하는 의존성 없는
모듈입니다 (1~6자 영숫자 + SSID 0~15). Argos 빌드에서도 설정 파싱/로깅에 그대로
사용됩니다. 단위 테스트가 존재하는 몇 안 되는 모듈 중 하나.
