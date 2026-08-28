# 07. 테스트 인프라와 분석 중 발견한 이슈

## 1. 단위 테스트 (`tests/`, `Test.mk`)

- 프레임워크: **Unity** (ThrowTheSwitch, git 서브모듈 `tests/lib/Unity`)
- `make test`로 실행. CI(`.github/workflows/code_unit_tests.yml`)가 push마다 수행
- pigpio를 링크하지 않으므로 하드웨어를 만지는 모듈은 `tests/stubs/`의 no-op 스텁으로 대체
  (recovery, led_ctrl, launcher, burnwire, power, rtc)

테스트 스위트 4개:

| 스위트 | 대상 | 비고 |
|---|---|---|
| `state_machine.test.c` | 상태 머신 전이 전부 | 479줄, 대부분 1000회 랜덤 fuzz — 압력/전압/카운터 임계 동작 검증. `UNIT_TEST` 전용 진입점 `__stateMachine_update_task()` 사용 |
| `aprs.test.c` | 콜사인 파서 | |
| `timing.test.c` | `get_next_time_of_day_occurance_s` (일/월 넘김) | 지정 시각 방출 정확성과 직결 |
| `str.test.c` | `strtobool`, `strtoquotedstring` | `recovery message "..."` 파싱 |

**커버리지 공백**: `recovery.c`(프로토콜 프레이밍·재동기화·질의 캐시)가 완전히 미테스트
— 아래 이슈 목록의 실사용 버그 다수가 이 모듈에 있습니다. `config.c`, `commands.c`,
센서 드라이버도 미테스트.

## 2. 하드웨어 수락 테스트 (`src/cetiHWTest/`)

조립된 태그에서 운영자가 수동 실행하는 ANSI TUI 프로그램입니다. **하드웨어를 직접 만지지
않고**, 실행 중인 `cetiTagApp`의 공유 메모리/세마포어에 붙어 검증합니다:

1. FIFO로 `mission pause` 전송 (상태 머신 정지)
2. 배터리 → 오디오(5초 창의 채널별 진폭/RMS) → 압력 → ECG → IMU → 온도 → 조도 →
   인터넷 연결 → 회수(현재 스텁: 무조건 FAIL) 순서로 9개 테스트
3. 결과를 `/data/test_result_<epoch_ms>.txt`에 기록 후 `mission resume`

## 3. 분석 중 발견한 의심 버그 목록

역공학 과정에서 소스를 직접 확인해 검증한 항목들입니다. 심각도 순으로 정리합니다.
(라인 번호는 `main` @ `1e3507a` 기준)

### 심각 — 미션 동작에 영향

1. **번와이어 타임아웃 영속화 조건 반전** — `state_machine.c:353`
   `__finalize_burnwire_time()`이 `if (access(파일, F_OK) != -1)` 즉 "**파일이 이미 있을
   때만**" 첫 잠수 시각을 기록합니다. 주석("아직 기록되지 않았다면")과 구버전
   (`tmp/state_machine.c`는 `== -1`)에 비춰 조건이 반전된 회귀입니다. 결과: 새 배포에서
   첫 잠수 시 타임아웃 기준 시각 재설정·영속화가 일어나지 않아, 재부팅 시 타임아웃이
   부팅 시각 기준으로 다시 계산되고, NTP 재시도 루프도 영원히 멈추지 않습니다.

2. **부유(floating) 감지 무력화** — `state_machine.c:157~167` 부근
   `float_start_detected`를 세운 직후 `if (float_start_detected) __reset_float_detection();`
   로 무조건 초기화해 플래그가 유지될 수 없습니다(`else`여야 할 자리). 추가로 롤 평균이
   피치 합으로 계산되고(`:154`), 도 단위 값을 라디안으로 변환해 도 상수와 비교합니다
   (`:139-140`). 현재는 `FLOAT_DETECTION 0`이라 전부 잠복 상태이며, 이 때문에
   `ST_RECORD_FLOATING`은 도달 불가 — 분리된 태그는 RECORD_SURFACE(GPS 수집만, **송신
   안 함**)에 머뭅니다. 회수 관점에서 중요한 제약입니다.

3. **회수 보드 기동 타임아웃/재시도 로직 오류** — `recovery.c:748~755`
   - `if (s_recovery_hardware_start_time_us >= 10초)` — 경과 시간이 아니라 **절대
     monotonic 타임스탬프**를 비교하므로 부팅 10초 후엔 항상 참.
   - `recovery_restart_count++`가 포기(else) 분기에만 있어 재시작 경로에서는 증가하지
     않음 → 최대 재시도 횟수에 도달할 수 없고, 보드가 응답하지 않으면
     `while (!__ping())` 루프가 초기화를 무한 블로킹할 수 있습니다.

4. **ECG 초기화 오류가 기록되지 않음** — `launcher.c:394`
   ECG 분기에서 `ecg_result` 대신 `pressure_result`를 검사합니다. ECG의 SHM/세마포어
   실패가 에러 비트마스크(LED 점멸/무선 보고)에 반영되지 않습니다.

5. **`imu reset` 명령이 오디오 시작에 바인딩** — `subcommands/cmd_imu.c:13`
   `.parse = audioCmd_start`로 되어 있어 `imu reset`을 치면 IMU 리셋 대신 오디오 수집이
   시작됩니다.

6. **NMEA 수신 버퍼 경계 미검사** — `recovery.c:845~854`
   페이로드 길이(최대 255)를 96바이트 `nmea_sentence`에 검사 없이 복사(`// TODO` 주석
   존재). 공백 트림 루프도 `uint8_t` 길이를 바닥 없이 감소시켜 언더플로 가능.

### 중간 — 자원 정리/부수 기능 오류

7. **오디오 페이지 세마포어 이름 오타** — `sensors/audio.c:412`
   `sem_audio_page`를 `AUDIO_BLOCK_SEM_NAME`으로 열어 블록/페이지 세마포어가 같은 커널
   객체를 가리킵니다 (페이지 대기자가 블록 포스트에 깨어남).

8. **조도 정리 경로가 ECG SHM을 unlink** — `sensors/light.c:198`
   `shm_unlink(ECG_SHM_NAME)` — `LIGHT_SHM_NAME`이어야 함. ECG 공유 메모리 이름이
   파괴되고 `/light_shm`은 누수.

9. **BMS NV 덮어쓰기 루프가 센티널 포함** — `battery.c:128` 부근
   배열 길이를 `sizeof`로 계산해 `{.name=NULL}` 센티널까지 순회 → 마지막에
   STATUS(0x000) 레지스터에 0을 쓰는 부작용.

10. **`max17320_disable_discharging()`이 COMM_STAT 다른 비트를 클로버** —
    `max17320.c:270` 부근. `value |= DISCHARGE_OFF`를 계산해 놓고 상수만 씀
    (CHARGE_OFF 래치가 풀릴 수 있음).

11. **Argos 빌드에서 메시지 길이 불일치** — `cmd_recovery.c` vs `recovery.c`
    CLI는 67자까지 받지만 `recovery_message()`는 24자 초과를 거부 — CLI 한도가 Argos
    전환 때 갱신되지 않음.

### 경미 — 표기/잔재

12. `cmd_burnwire.c:14` — `burnwire off` 응답이 "Turned burnwire on".
13. `cmd_audio.c:66-67` — `forceOverflow`/`simulateOverflow` 설명 교차.
14. `recovery.c` — GPS CSV 헤더는 1열(`GPS`)인데 실제 행은 4필드; sleep/gps_only가 같은
    내부 상태값을 기록; `return` 뒤 도달 불가 코드 3곳.
15. `tmp/state_machine.{c,h}` — 빌드에 포함되지 않는 구버전 사본이 커밋되어 있음.
16. `debian/changelog`(2.3-1) vs `_versioning.h`(V2.5.1) 버전 불일치, changelog 상단
    두 항목의 날짜 역전.
17. `.deb` `Depends`에 libFLAC 누락 (이미지 빌드가 우연히 충족).
18. `build/Dockerfile`이 dangling `\`로 끝남 (현재 BuildKit은 허용).
19. `ceti-tag-data-capture.service`의 `ExecStartPre=systemctl ...`이 절대 경로가 아니고,
    `/data` 마운트·pigpio에 대한 유닛 의존성이 없음.
20. `ltr329als.c` — 측정 주기 레지스터 기본값을 정의만 하고 쓰지 않음.
21. `cetiTag.h:50` 주석의 FIFO 워터마크 계산(8KB/25%)이 실제(16KB/50%)와 불일치.
22. Wi-Fi PSK(`Talk2Whales`)·`pi` 비밀번호(`ceticeti`)가 이미지에 평문 하드코딩.

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

## 5. 수정 우선순위 제안

필드 신뢰성 관점에서 고치는 순서를 정한다면:

1. 이슈 1 (번와이어 타임아웃 영속화) — 방출 시각 정확성·재부팅 내성의 핵심
2. 이슈 3 (회수 보드 무한 재시도) — 보드 불량 시 태그 전체가 기동 불능
3. 이슈 6 (NMEA 버퍼) — 메모리 안전
4. 이슈 2 (부유 감지) — 활성화 전 3개 버그 일괄 수정 필요
5. 이슈 5, 7, 8 — 명령/자원 정리 정합성
