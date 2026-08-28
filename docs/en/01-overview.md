# 01. Project Overview and Repository Layout

## 1. Background

[Project CETI](https://www.projectceti.org/) is a research initiative aiming to decode
sperm whale communication. This repository holds the entire software stack for the
embedded computer (a Raspberry Pi) inside the **data-collection tag (Whale Tag)** that is
attached to whales with suction cups.

The tag's mission life cycle is roughly:

1. **Pre-deployment (PREDEPLOY)** — Wi-Fi is up so an operator can inspect/configure over SSH.
2. **Recording (RECORD_DIVING / RECORD_SURFACE)** — attached to the whale, alternating
   between dives and surfacing while recording audio, biometric, and environmental data to
   `/data`. Radios are put to sleep while submerged to save power.
3. **Release (BRN_ON / LOW_POWER_BURN)** — when a configured timeout, low battery voltage,
   or low storage condition is met, current is driven through the **burnwire** to corrode
   and cut the attachment; the tag floats free.
4. **Retrieval (RETRIEVE)** — floating at the surface, the tag transmits its GPS position
   over the Argos satellite system (APRS VHF on older boards) so the team can recover it.
5. **Shutdown (SHUTDOWN)** — minimum-power state; the system powers itself off.

## 2. Hardware generations and branches

Per `README.md`, branches/tags map to hardware versions:

| Branch | Hardware |
|---|---|
| `v0` | Raspberry Pi Zero W + Octoboard sound card |
| `v2` | MVP targeted at Jan '22 deployment. Pi Zero W + 3 custom bonnets, FPGA drives the hydrophones |
| `v2_2` | Pi Zero **2** W main computer + electrical/mechanical changes |
| `main` | Hardware actively deployed in the field (currently the v2.5-generation code) |

Hardware assumed by the current `main` code (v2.5.1):

- **Main computer**: Raspberry Pi Zero 2 W (arm64, quad-core, 512 MB RAM)
- **FPGA** (Xilinx, bitstream loaded by the Pi via slave-serial): AD7768-4 audio ADC
  control, 32 KB FIFO buffering, CAM (control) bridge to the Pi, LED control, and the
  final power-down sequence
- **Audio**: 3 hydrophone channels, AD7768-4 24-bit ΣΔ ADC (run at 96 kHz/16-bit by default)
- **Sensors**: Keller 4LD pressure/temperature, LTR-329ALS light, BNO086 IMU, ADS1219-based
  ECG, MAX17320 2-cell battery gauge/protector, I2C RTC (0x68)
- **Recovery board**: separate STM32-based board. GPS receive + Argos satellite transmit
  (current build) or APRS VHF transmit (older build), talking to the Pi over UART
- **Burnwire**: release device switched by an I/O expander pin (PCAL family, I2C 0x21)

## 3. Top-level repository layout

```
whale-tag-embedded/
├── Makefile                  # Top-level build orchestration (Docker → QEMU → sdcard.img)
├── README.md                 # Build/install instructions
├── LICENSE                   # MIT (Project CETI)
├── build/                    # Image build tooling
│   ├── Dockerfile            #   Build container (debian:bookworm)
│   ├── rpi-image             #   Python tool: image download/partition/mount/chroot-run
│   ├── setup_image.sh        #   OS customization (runs inside chroot)
│   ├── make_dpkg.sh          #   Debian package build (runs inside chroot)
│   ├── install_packages.sh   #   .deb install (runs inside chroot)
│   └── logo.txt              #   Build-complete ASCII art
├── overlay/                  # Files copied verbatim onto the root filesystem
│   ├── etc/bash.bashrc       #   ro/rw root-state prompt, ro/rw remount aliases
│   └── usr/lib/raspberrypi-sys-mods/
│       ├── firstboot         #   Replacement first-boot script (partition grow, hostname, …)
│       └── custom_bash_history.txt  # Operator cheat sheet (seeded as pi's .bash_history)
├── packages/
│   └── ceti-tag-data-capture/    # The core package (section 4 below)
└── tmp/                      # ⚠ Stale copy of state_machine.c/.h (not built; leftover)
```

> Note: `README.md` mentions a `ceti-tag-set-hostname` package, but it no longer exists in
> the tree. Hostname setup was absorbed into `sethostname()` in
> `overlay/usr/lib/raspberrypi-sys-mods/firstboot` (hostname = `wt-` + last 8 chars of the
> CPU serial).

## 4. Layout of `packages/ceti-tag-data-capture/`

```
ceti-tag-data-capture/
├── Makefile              # gcc build (two apps: cetiTagApp, cetiHWTest)
├── Test.mk               # Unity unit-test build/run
├── debian/               # Debian packaging (control, rules, service, postinst, …)
├── config/
│   ├── ceti-config.txt   # Shipped deployment config (pressure/voltage thresholds, timeouts, audio, callsigns)
│   ├── tag-info.yaml     # Per-tag metadata template (sensor list, hydrophone placement)
│   └── top.bin           # FPGA bitstream (149,516 bytes)
├── ipc/                  # Operator scripts + command FIFO location
│   ├── sendCommand       #   Wrapper: write a command to the FIFO, read the response
│   ├── tagWake.sh / tagSleep.sh   # BMS FET enable/disable
│   ├── nvwrite.sh        #   MAX17320 nonvolatile register programming
│   └── flashRecovery.sh  #   Flash the recovery board firmware with stm32flash
├── lib/libCetiRecovery/  # (git submodule) recovery-board UART protocol definition header
├── src/
│   ├── cetiTagApp/       # Main daemon (see docs 03–06)
│   └── cetiHWTest/       # Post-assembly hardware acceptance test TUI (see doc 07)
├── FPGA_v2p1/            # FPGA Verilog sources (rtl/top.v, adc.v, cam.v, …)
└── tests/                # Unity unit tests + stubs
```

## 5. Runtime environment summary (on the target)

- The daemon `cetiTagApp` starts at boot as a **systemd service**
  (`ceti-tag-data-capture.service`) and is restarted unconditionally
  (`Restart=always`, 60 s backoff).
- The root filesystem is **read-only via overlayroot (tmpfs)** — protecting the OS from SD
  wear and power loss. All writes go to a separate `cetiData` partition (by label) mounted
  at `/data`.
- Kernel option `isolcpus=2,3` isolates CPUs 2 and 3, dedicated to ECG acquisition
  (core 2) and audio SPI acquisition (core 3).
- The operator interface is a pair of named pipes (FIFOs) `ipc/cetiCommand` /
  `cetiResponse`, used through the `sendCommand <command>` script.

## 6. Version caveat

- Debian package version: `2.3-1` (`debian/changelog`)
- Application version string: `"State Machine Simplified - V2.5.1"`
  (`src/cetiTagApp/_versioning.h`)

The two are **not synchronized**; to identify the code actually running on a field device,
rely on the application version string (+ build date) printed to syslog at startup.
