# 01. 프로젝트 개요와 저장소 구조

## 1. 프로젝트 배경

[Project CETI](https://www.projectceti.org/)는 향유고래(sperm whale)의 의사소통을 해독하려는
연구 프로젝트입니다. 이 저장소는 고래 몸에 흡착 부착되는 **데이터 수집 태그(Whale Tag)** 의
임베디드 컴퓨터(Raspberry Pi)에 들어가는 소프트웨어 전체를 담고 있습니다.

태그의 임무 수명주기는 대략 다음과 같습니다.

1. **부착 전(PREDEPLOY)** — Wi-Fi가 켜져 있어 운영자가 SSH로 점검/설정.
2. **기록(RECORD_DIVING / RECORD_SURFACE)** — 고래에 붙어 잠수/부상을 반복하며
   오디오·생체·환경 데이터를 `/data`에 기록. 잠수 중에는 무선을 끄고 전력을 아낌.
3. **방출(BRN_ON / LOW_POWER_BURN)** — 설정된 시간 초과, 배터리 저전압, 저장공간 부족 등
   조건이 되면 **번와이어(burnwire)** 에 전류를 흘려 부착 기구를 부식·절단, 태그가 떠오름.
4. **회수(RETRIEVE)** — 수면에 떠서 GPS 위치를 Argos 위성(구버전은 APRS VHF)으로 송신,
   연구팀이 회수.
5. **종료(SHUTDOWN)** — 최소 전력 상태로 시스템 전원 차단.

## 2. 하드웨어 세대와 브랜치

`README.md`에 따르면 하드웨어 버전별로 브랜치/태그가 나뉩니다.

| 브랜치 | 하드웨어 |
|---|---|
| `v0` | Raspberry Pi Zero W + Octoboard 사운드카드 |
| `v2` | 2022년 1월 배포 목표 MVP. Pi Zero W + 커스텀 보닛 3장, FPGA가 수중청음기 구동 |
| `v2_2` | Pi Zero **2** W 메인 컴퓨터 + 전기/기계 개선 |
| `main` | 현재 필드 배포 중인 하드웨어 대상 (현 시점 v2.5 세대 코드) |

현재 `main`의 코드(v2.5.1)가 가정하는 하드웨어 구성:

- **메인 컴퓨터**: Raspberry Pi Zero 2 W (arm64, 쿼드코어, RAM 512MB)
- **FPGA** (Xilinx, slave-serial로 Pi가 비트스트림 로드): AD7768-4 오디오 ADC 제어,
  32KB FIFO 버퍼링, Pi와의 CAM(제어) 브리지, LED 제어, 전원 차단 시퀀스
- **오디오**: 수중청음기 3채널, AD7768-4 24-bit ΣΔ ADC (기본 96kHz/16-bit 운용)
- **센서**: Keller 4LD 압력/수온, LTR-329ALS 조도, BNO086 IMU, ADS1219 기반 ECG,
  MAX17320 2-셀 배터리 게이지/보호 IC, I2C RTC(0x68)
- **회수 보드**: STM32 기반 별도 보드. GPS 수신 + Argos 위성 송신(현 빌드) 또는
  APRS VHF 송신(구 빌드), UART로 Pi와 통신
- **번와이어**: I/O 익스팬더(PCAL 계열, I2C 0x21) 핀으로 스위칭되는 방출 장치

## 3. 저장소 최상위 구조

```
whale-tag-embedded/
├── Makefile                  # 최상위 빌드 오케스트레이션 (Docker → QEMU → sdcard.img)
├── README.md                 # 빌드/설치 방법 (영문)
├── LICENSE                   # MIT (Project CETI)
├── build/                    # 이미지 빌드 도구
│   ├── Dockerfile            #   빌드 컨테이너 (debian:bookworm)
│   ├── rpi-image             #   Python 스크립트: 이미지 다운로드/파티션/마운트/chroot 실행
│   ├── setup_image.sh        #   OS 커스터마이징 (chroot 내부 실행)
│   ├── make_dpkg.sh          #   Debian 패키지 빌드 (chroot 내부 실행)
│   ├── install_packages.sh   #   .deb 설치 (chroot 내부 실행)
│   └── logo.txt              #   빌드 완료 ASCII 아트
├── overlay/                  # 루트 파일시스템에 그대로 복사되는 파일들
│   ├── etc/bash.bashrc       #   ro/rw 루트 상태 표시 프롬프트, ro/rw 알리아스
│   └── usr/lib/raspberrypi-sys-mods/
│       ├── firstboot         #   교체된 첫 부팅 스크립트 (파티션 확장, 호스트명 등)
│       └── custom_bash_history.txt  # 운영자 치트시트 (pi 계정 .bash_history로 심어짐)
├── packages/
│   └── ceti-tag-data-capture/    # 핵심 패키지 (아래 4절)
└── tmp/                      # ⚠ state_machine.c/.h 의 오래된 사본 (빌드에 미포함, 잔재)
```

> 참고: `README.md`에는 `ceti-tag-set-hostname` 패키지가 언급되지만 현재 트리에는 존재하지
> 않습니다. 호스트명 설정 기능은 `overlay/usr/lib/raspberrypi-sys-mods/firstboot`의
> `sethostname()`으로 흡수되었습니다 (호스트명 = `wt-` + CPU 시리얼 뒤 8자리).

## 4. `packages/ceti-tag-data-capture/` 구조

```
ceti-tag-data-capture/
├── Makefile              # gcc 빌드 (앱 2종: cetiTagApp, cetiHWTest)
├── Test.mk               # Unity 단위 테스트 빌드/실행
├── debian/               # Debian 패키징 (control, rules, service, postinst 등)
├── config/
│   ├── ceti-config.txt   # 배포 기본 설정 (임계 압력/전압, 타임아웃, 오디오, 콜사인)
│   ├── tag-info.yaml     # 태그별 메타데이터 템플릿 (센서 목록, 하이드로폰 배치)
│   └── top.bin           # FPGA 비트스트림 (149,516 바이트)
├── ipc/                  # 운영 스크립트 + 명령 FIFO 위치
│   ├── sendCommand       #   FIFO에 명령을 쓰고 응답을 읽는 래퍼
│   ├── tagWake.sh / tagSleep.sh   # BMS FET 켜기/끄기
│   ├── nvwrite.sh        #   MAX17320 비휘발성 레지스터 프로그래밍
│   └── flashRecovery.sh  #   stm32flash로 회수 보드 펌웨어 굽기
├── lib/libCetiRecovery/  # (git submodule) 회수 보드 UART 프로토콜 정의 헤더
├── src/
│   ├── cetiTagApp/       # 메인 데몬 (03~06 문서 참조)
│   └── cetiHWTest/       # 조립 후 하드웨어 수락 테스트 TUI (07 문서 참조)
├── FPGA_v2p1/            # FPGA Verilog 소스 (rtl/top.v, adc.v, cam.v 등)
└── tests/                # Unity 단위 테스트 + 스텁
```

## 5. 실행 환경 요약 (타깃 보드에서)

- 데몬 `cetiTagApp`은 **systemd 서비스**(`ceti-tag-data-capture.service`)로 부팅 시 자동
  시작되며 `Restart=always`(60초 간격)로 무조건 재시작됩니다.
- 루트 파일시스템은 **overlayroot(tmpfs)** 로 읽기 전용 — SD 카드 마모/전원 차단으로부터
  OS를 보호합니다. 모든 기록은 별도의 `cetiData` 파티션(라벨) → `/data` 마운트로 갑니다.
- 커널 부팅 옵션 `isolcpus=2,3`으로 CPU 2·3번을 격리해 두고, ECG 수집(코어 2)과
  오디오 SPI 수집(코어 3)에 전용 할당합니다.
- 운영자 인터페이스는 두 개의 명명 파이프(FIFO) `ipc/cetiCommand`/`cetiResponse`이며,
  `sendCommand <명령>` 스크립트로 사용합니다.

## 6. 버전 표기 주의

- Debian 패키지 버전: `2.3-1` (`debian/changelog`)
- 애플리케이션 버전 문자열: `"State Machine Simplified - V2.5.1"` (`src/cetiTagApp/_versioning.h`)

두 버전이 **동기화되어 있지 않으므로**, 필드 장비의 실제 코드 버전 확인은 syslog에 찍히는
애플리케이션 버전 문자열(+빌드 날짜)을 기준으로 해야 합니다.
