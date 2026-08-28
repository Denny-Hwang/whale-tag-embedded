# 07. 테스트 인프라와 분석 중 발견한 이슈

## 1. 단위 테스트 (`tests/`, `Test.mk`)

- 프레임워크: **Unity** (ThrowTheSwitch, git 서브모듈 `tests/lib/Unity`)
- `make test`로 실행. CI(`.github/workflows/code_unit_tests.yml`)가 push마다 수행
- pigpio를 링크하지 않으므로 하드웨어를 만지는 모듈은 `tests/stubs/`의 no-op 스텁으로 대체
  (recovery, led_ctrl, launcher, burnwire, power, rtc)

테스트 스위트 12개 (총 93건):

| 스위트 | 대상 | 비고 |
|---|---|---|
| `state_machine.test.c` | 상태 머신 전이 전부 + 부유 감지 (25건) | 대부분 1000회 랜덤 fuzz — 압력/전압/카운터 임계 동작 검증. `UNIT_TEST` 전용 진입점 `__stateMachine_update_task()` 사용 |
| `recovery.test.c` | 회수 보드 프로토콜 계층 + **RX 스레드** (22건) | `recovery.c`를 TU에 직접 include해 static 파서까지 검증. pigpio 직렬 API를 스크립트 가능한 인메모리 포트로 대체 (`tests/fakes/pigpio.h`). TX 프레이밍, `'$'` 재동기화 파서, 타임아웃, PING/PONG, Argos 설정 검증, RTC 페이로드 패킹 + **실제 `recovery_rx_thread`를 띄워** NMEA→SHM 타임스탬프/트림, 96바이트 클램프, 전체-개행 패킷 언더플로 방지, PONG 생존 플래그 캐싱 검증 |
| `commands.test.c` | 명령 디스패처 (8건) | `commands.c`를 TU에 include해 static 응답 파이프 경로를 일반 파일로 돌리고, 페이크 서브커맨드 테이블로 디스패치 검증: ping/quit/logging 토글/수집 시작·중지/powerdown(FPGA opcode 0x0E)/서브커맨드 인자 전달/미지 명령·서브커맨드 도움말 |
| `config.test.c` | 설정 파서 (10건) | 실제 `config.o` 링크. 키별 파싱·별칭(P1/P2/T0/BT), 전압 ÷2·범위 검증, `strtotime_s` 접미사(무접미사=분!), 지정 시각, 오디오 샘플레이트/비트심도/필터 매핑, rec_* 키, 오류 코드, 임시 파일 `config_read` |
| `device/*.test.c` (5개) | **I2C 디바이스 드라이버** (22건) | `tests/fakes/pigpio.fake.c`의 **스크립트 가능한 I2C 레지스터 맵 페이크**에 대해 실제 드라이버 `.o` 링크. `keller4ld`(압력·수온 변환식, 상태 바이트 검증, 빅엔디언 파싱), `rtc`(리틀엔디언 32비트 카운터 get/set), `iox`(설정/출력 레지스터 read-modify-write, 핀 검증), `max17320`(이중 주소 라우팅, 전압/전류/온도/SoC 변환, **FET 제어가 COMM_STAT 다른 비트를 보존**하는 회귀 테스트), `ltr329als`(wake 시 측정 주기 기록 회귀, 측정/ID 레지스터) |
| `aprs.test.c` | 콜사인 파서 (2건) | |
| `timing.test.c` | `get_next_time_of_day_occurance_s` (2건) | 지정 시각 방출 정확성과 직결 |
| `str.test.c` | `strtobool`, `strtoquotedstring` (2건) | `recovery message "..."` 파싱 |

이 과정에서 실 결함 1건을 추가 발견·수정: `keller4ld.c`가 수신 버퍼를 `char`(signed)로
선언해 0x80 이상 바이트가 부호 확장되는 문제 — ARM(char=unsigned)에서는 우연히
동작했지만 이식성 결함이라 `uint8_t`로 수정 (x86 테스트가 잡아낸 사례).

**남은 커버리지 공백**: 스레드 기반 센서 계층(sensors/audio.c, ecg.c, imu.c 등 —
FPGA/SHTP/실시간 의존)과 recovery CSV 기록 경로(`/data` 필요)는 미테스트.

## 2. 하드웨어 수락 테스트 (`src/cetiHWTest/`)

조립된 태그에서 운영자가 수동 실행하는 ANSI TUI 프로그램입니다. **하드웨어를 직접 만지지
않고**, 실행 중인 `cetiTagApp`의 공유 메모리/세마포어에 붙어 검증합니다:

1. FIFO로 `mission pause` 전송 (상태 머신 정지)
2. 배터리 → 오디오(5초 창의 채널별 진폭/RMS) → 압력 → ECG → IMU → 온도 → 조도 →
   인터넷 연결 → 회수(현재 스텁: 무조건 FAIL) 순서로 9개 테스트
3. 결과를 `/data/test_result_<epoch_ms>.txt`에 기록 후 `mission resume`

## 3. 분석 중 발견한 의심 버그 목록

역공학 과정에서 소스를 직접 확인해 검증한 항목들입니다. 심각도 순으로 정리합니다.
(라인 번호는 최초 분석 시점 `main` @ `1e3507a` 기준)

> ✅ 표시 항목은 후속 PR들에서 **수정 완료**되었습니다. 22번(자격 증명 하드코딩)만
> 팀 차원의 프로비저닝 방식 결정이 필요해 코드 변경 없이 문서화로 남겼습니다.

### 심각 — 미션 동작에 영향

1. ✅ **번와이어 타임아웃 영속화 조건 반전** — `state_machine.c:353`
   `__finalize_burnwire_time()`이 `if (access(파일, F_OK) != -1)` 즉 "**파일이 이미 있을
   때만**" 첫 잠수 시각을 기록했습니다. 주석("아직 기록되지 않았다면")과 구버전
   (`tmp/state_machine.c`는 `== -1`)에 비춰 조건이 반전된 회귀였습니다. 결과: 새 배포에서
   첫 잠수 시 타임아웃 기준 시각 재설정·영속화가 일어나지 않아, 재부팅 시 타임아웃이
   부팅 시각 기준으로 다시 계산되고, NTP 재시도 루프도 영원히 멈추지 않았습니다.
   → `== -1`로 수정.

2. ✅ **부유(floating) 감지 무력화 (버그 3건)** — `state_machine.c:139~167`
   ① `float_start_detected`를 세운 직후 무조건 `__reset_float_detection()`을 호출해
   플래그가 유지될 수 없었음 → 부유 조건이 아닐 때만 리셋하도록 `else if`로 수정.
   ② 롤 오차 평균이 피치 합으로 계산됨 → 롤 합으로 수정. ③ 라디안인 오일러 값을
   도→라디안으로 재변환해 도 상수와 비교 → 라디안→도 변환(`*180/π`)으로 수정.
   이후 **`FLOAT_DETECTION 1`로 활성화**되었습니다 (아래 §5 참조).

3. ✅ **회수 보드 기동 타임아웃/재시도 로직 오류** — `recovery.c:748~755`
   - `if (s_recovery_hardware_start_time_us >= 10초)` — 경과 시간이 아니라 절대
     monotonic 타임스탬프를 비교해 부팅 10초 후엔 항상 참 → 경과 시간
     (`get_monotonic_time_us() - 시작시각`) 비교로 수정.
   - `recovery_restart_count++`가 포기(else) 분기에만 있어 재시작 경로에서 증가하지 않음
     → 재시작 분기에서 증가하도록 수정. 이제 최대 5회 재시작 후 `THREAD_ERR_HW`를
     반환해 무한 블로킹이 불가능합니다 (반환값도 `WT_RESULT` 대신 launcher가 기대하는
     `THREAD_*` 마스크로 정정).

4. ✅ **ECG 초기화 오류가 기록되지 않음** — `launcher.c:394`
   ECG 분기에서 `ecg_result` 대신 `pressure_result`를 검사 → `ecg_result`로 수정.

5. ✅ **`imu reset` 명령이 오디오 시작에 바인딩** — `subcommands/cmd_imu.c:13`
   `.parse = audioCmd_start` → `imuCmd_reset`으로 수정.

6. ✅ **NMEA 수신 버퍼 경계 미검사** — `recovery.c:845~854`
   페이로드 길이(최대 255)를 96바이트 `nmea_sentence`에 검사 없이 복사했고, 공백 트림
   루프가 `uint8_t` 길이를 바닥 없이 감소시켜 언더플로 가능했음 → 버퍼 크기로 클램프
   + 0 바닥이 있는 트림 루프로 수정.

### 중간 — 자원 정리/부수 기능 오류

7. ✅ **오디오 페이지 세마포어 이름 오타** — `sensors/audio.c:412`
   `sem_audio_page`를 `AUDIO_BLOCK_SEM_NAME`으로 열어 블록/페이지 세마포어가 같은 커널
   객체를 가리켰음 → `AUDIO_PAGE_SEM_NAME`으로 수정.

8. ✅ **조도 정리 경로가 ECG SHM을 unlink** — `sensors/light.c:198`
   `shm_unlink(ECG_SHM_NAME)` → `LIGHT_SHM_NAME`으로 수정.

9. ✅ **BMS NV 덮어쓰기 루프가 센티널 포함** — `battery.c:128`
   배열 길이를 `sizeof`로 계산해 `{.name=NULL}` 센티널까지 순회 → STATUS(0x000)
   레지스터에 0을 쓰는 부작용 → `battery_verify()`와 동일하게 센티널에서 멈추도록 수정.

10. ✅ **`max17320_disable_discharging()`이 COMM_STAT 다른 비트를 클로버** —
    `max17320.c:270`. `value |= DISCHARGE_OFF`를 계산해 놓고 상수만 써서 CHARGE_OFF
    래치가 풀릴 수 있었음 → `value`를 쓰도록 수정.

11. ✅ **Argos 빌드에서 메시지 길이 불일치** — `cmd_recovery.c` vs `recovery.c`
    CLI는 67자까지 받지만 `recovery_message()`는 24자 초과를 거부(전송은 실패).
    길이 한도를 `recovery.h`의 `RECOVERY_BOARD_MAX_MSG_LENGTH`(APRS 67 / Argos 24)로
    일원화하고 CLI가 이를 사용하도록 수정 (기존 CLI의 off-by-one 스택 버퍼 오버플로
    가능성도 함께 제거).

### 경미 — 표기/잔재

12. ✅ `cmd_burnwire.c:14` — `burnwire off` 응답이 "Turned burnwire on" → "off"로 수정.
13. ✅ `cmd_audio.c` — `forceOverflow`/`simulateOverflow`는 설명뿐 아니라 **핸들러
    바인딩 자체가 교차**되어 있었음 → 이름·설명·핸들러를 일치시켜 수정.
14. ✅ `recovery.c` 잔재 정리 — GPS CSV 헤더를 실제 행 필드 4개(`Timestamp [us], RTC
    Count, Notes, GPS`)에 맞춤; 어디서도 읽지 않는 dead 보드 상태 모델
    (`s_recovery_board_model`/`RecoveryBoardState`) 제거; `return` 뒤 도달 불가 코드
    3곳 제거; CSV `fopen` NULL 미검사 수정.
15. ✅ `tmp/state_machine.{c,h}` — 빌드에 포함되지 않는 구버전 사본 제거
    (`.gitignore`의 `tmp/` 항목과도 정합).
16. ✅ `debian/changelog` — 애플리케이션 버전과 동기화한 `2.5-1` 항목 추가 (기존
    항목의 날짜 역전은 이력이므로 수정하지 않음).
17. ✅ `.deb` `Depends`에 `libflac12` 추가 (타깃 Raspberry Pi OS Bookworm 기준).
18. ✅ `build/Dockerfile` — dangling `\` 종결 + apt 리스트 정리(`rm -rf
    /var/lib/apt/lists/*`) 추가.
19. ✅(부분) `ceti-tag-data-capture.service` — `ExecStartPre`를 절대 경로
    (`/usr/bin/systemctl`)로 수정. `/data` 마운트에 대한 `RequiresMountsFor`는
    **의도적으로 추가하지 않음**: 데이터 파티션이 손상돼도 앱은 기동해야 번와이어
    방출 등 미션 안전 로직이 동작하기 때문 (`nofail` 마운트와 일관).
20. ✅ `ltr329als.c` — `als_wake()`에서 측정 주기 레지스터(`ALS_REG_MEAS_RATE`)에
    `ALS_MEAS_RATE_DEFAULT`를 명시적으로 기록 (파워온 기본값과 동일 — 동작 불변,
    설정 명시화).
21. ✅ `cetiTag.h` 주석의 FIFO 워터마크 계산을 실제 값(16KB/50%)으로 정정.
22. ⚠ (미변경 — 운영 결정 필요) Wi-Fi PSK(`Talk2Whales`)·`pi` 비밀번호(`ceticeti`)가
    이미지에 평문 하드코딩. 폐쇄 필드망 전제의 의도된 트레이드오프로 보이나, 자격
    증명 교체나 빌드 시 주입(예: 환경 변수/시크릿 파일) 전환은 팀의 프로비저닝
    워크플로 결정이 필요해 코드 변경 없이 남겨둠.

## 4. 최근 개발 흐름 (git 히스토리 테마)

2025-10 이후 `v2_5` 라인의 주요 흐름:

1. **상태 머신 대개편** (#117, 2025-11): PREDEPLOY/FLOATING/LOW_POWER_BURN/SHUTDOWN 상태
   추가, 스레드 매니저 도입, LED 컨트롤러 스레드 신설, 저장공간 감시, 상태 변화 시에만
   서브시스템 재구성
2. **Argos 회수 보드 통합** (#119, 2025-12): 프로토콜을 `libCetiRecovery` 서브모듈로
   분리, Argos ID/주소/키/변조 설정 IPC, 보드 RTC 시간 동기화
3. **회수 보드 기동 타이밍** (#121/#122, 2026-01): 보드 전원 인가를 core_init으로 옮겨
   병렬 부팅, ping 재시도 루프 (단, 위 이슈 3의 버그 포함)
4. **배포 후 버그픽스**: 2026-01-13 배포에서 발견된 타이밍 버그, quit 명령 버그 등

## 5. 수정 현황과 남은 작업

우선순위가 높았던 이슈 1~12는 모두 수정되어 반영되었습니다 (단위 테스트 통과,
수정 파일 전체 컴파일 검증).

### FLOAT_DETECTION 활성화 (완료)

이슈 2의 버그 3건 수정 후 `FLOAT_DETECTION 1`로 **활성화**했습니다. 함께 반영된 것:

- **FLOATING 케이스의 missing break 수정** — `__at_depth()`→DIVING 전이 뒤 break가 없어
  잠수+비직립 동시 조건에서 DIVING이 SURFACE로 즉시 덮어써지던 문제 (기능이 꺼져 있어
  잠복해 있던 버그)
- **스무딩 버퍼 워밍업 게이팅 추가** — 리셋 직후 이동평균이 0에 편향되어(합÷10, 버퍼
  대부분 0) 자세 오차가 커도 순간적으로 "직립"으로 판정되던 문제. 버퍼가 다 찰 때까지
  (10초) 부유 시작/리셋 판정을 유보하도록 수정
- **단위 테스트 5건 추가** — SURFACE→FLOATING(20분 홀드), 비부유 자세에서 오탐 없음,
  FLOATING→DIVING(break 회귀 방지), FLOATING→SURFACE(뒤집힘), RETRIEVE→SHUTDOWN(부유).
  테스트 페이크를 제어 가능하게 개선 (`fake_euler`, `fake_monotonic_offset_s`)

**동작 변화**: 분리된 태그가 수면에서 수직 자세(피치 -85°±10°)를 20분 유지하면
RECORD_SURFACE→RECORD_FLOATING으로 전이해 **Argos 송신을 시작**합니다 (기존에는 4일
타임아웃까지 송신하지 않음). RETRIEVE에서 부유 20분이 확인되면 SHUTDOWN으로 넘어가
Pi를 꺼서 전력을 아끼는데, 이때 BMS FET와 3V3 RF 레일은 유지되므로 **회수 보드 비컨은
계속 동작**합니다 (`launcher.c`의 `reboot(POWER_OFF)`는 Pi만 정지시키며, 완전 차단은
별도의 `powerdown` 명령 경로에서만 일어남).

**주의**: 원저자는 PR #107에서 "needs metrics verification before being enabled in
field"(필드 활성화 전 지표 검증 필요)라고 명시했습니다. 목표 피치 -85°는 흡착컵 등의
영향으로 경험적으로 정한 값이므로, **실제 배포 전 실물 태그의 부유 자세 실측 확인을
권장**합니다. 문제가 있으면 `state_machine.h`의 `FLOAT_DETECTION`을 0으로 되돌리면
됩니다.

### 남은 작업 제안

1. 실물 태그로 부유 자세(피치/롤) 실측 → `FLOAT_DETECT_TARGET_PITCH_DEG` 검증
2. ~~이슈 13 이후의 경미/잔재 항목 정리~~ — **완료** (13~21 수정, 22는 운영 결정
   사항으로 문서화)
3. ~~`recovery.c` 프로토콜 계층 단위 테스트 추가~~ — **완료**: 프로토콜 18건 +
   RX 스레드 4건. ~~`config.c`/`commands.c` 테스트~~ — **완료**: config 10건,
   commands 8건. ~~센서/디바이스 드라이버 테스트~~ — **완료**: I2C 드라이버 5종
   22건 (§1 참조). 스레드 기반 센서 계층(audio/ecg/imu)은 미착수
4. 자격 증명(이슈 22) 처리 방침 결정 — 빌드 시 주입 또는 배포 전 교체 절차 수립
