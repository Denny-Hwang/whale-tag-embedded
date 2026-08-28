# 07. Test Infrastructure and Issues Found During Analysis

## 1. Unit tests (`tests/`, `Test.mk`)

- Framework: **Unity** (ThrowTheSwitch, git submodule at `tests/lib/Unity`)
- Run with `make test`. CI (`.github/workflows/code_unit_tests.yml`) runs it on every push
- pigpio is not linked, so hardware-touching modules are replaced by no-op stubs in
  `tests/stubs/` (recovery, led_ctrl, launcher, burnwire, power, rtc)

Four test suites:

| Suite | Target | Notes |
|---|---|---|
| `state_machine.test.c` | every state machine transition | 479 lines, mostly fuzzed 1000 iterations — verifies pressure/voltage/counter thresholds. Uses the `UNIT_TEST`-only entry point `__stateMachine_update_task()` |
| `aprs.test.c` | callsign parser | |
| `timing.test.c` | `get_next_time_of_day_occurance_s` (day/month rollover) | directly relevant to time-of-day release accuracy |
| `str.test.c` | `strtobool`, `strtoquotedstring` | the parser behind `recovery message "..."` |

**Coverage gap**: `recovery.c` (protocol framing, resynchronization, query cache) is
entirely untested — most of the real bugs fixed below lived in this untested module.
`config.c`, `commands.c`, and the sensor drivers are also untested.

## 2. Hardware acceptance test (`src/cetiHWTest/`)

An operator-driven ANSI TUI run manually on an assembled tag. It **does not touch hardware
itself** — it attaches to the running `cetiTagApp`'s shared memory/semaphores:

1. Sends `mission pause` over the FIFO (pausing the state machine)
2. Runs 9 tests in order: battery → audio (per-channel amplitude/RMS over a 5 s window) →
   pressure → ECG → IMU → temperature → light → internet connectivity → recovery
   (currently stubbed to always FAIL)
3. Writes results to `/data/test_result_<epoch_ms>.txt`, then `mission resume`

## 3. Suspected bugs found during analysis

Each item was verified directly against the source during reverse engineering, ordered by
severity. (Line numbers refer to the original analysis baseline, `main` @ `1e3507a`.)

> Items marked ✅ (1–12) were **fixed** in the follow-up bug-fix PR. The remaining
> unfixed entries are the minor/leftover items from 13 on.

### Severe — affects mission behavior

1. ✅ **Burnwire timeout persistence condition inverted** — `state_machine.c:353`
   `__finalize_burnwire_time()` used `if (access(file, F_OK) != -1)`, i.e. it recorded the
   first-dive time **only when the file already existed**. Judging by the comment ("if one
   has not already been recorded") and the old copy (`tmp/state_machine.c` uses `== -1`),
   the condition was an inverted regression. Consequence: on a fresh deployment the first
   dive neither re-stamped nor persisted the timeout start time, so a reboot recomputed
   the timeout from boot time, and the NTP retry loop never stopped.
   → Fixed to `== -1`.

2. ✅ **Float detection neutralized (3 bugs)** — `state_machine.c:139–167`
   ① Immediately after setting `float_start_detected`, an unconditional
   `__reset_float_detection()` cleared it, so the flag could never persist → fixed to
   reset only when the float condition is not met (`else if`).
   ② The roll error average was computed from the pitch sum → fixed to the roll sum.
   ③ The Euler angles (already radians) were "converted" degrees→radians and compared
   against degree constants → fixed to radians→degrees (`*180/π`).
   The feature has since been **enabled via `FLOAT_DETECTION 1`** (see §5 below).

3. ✅ **Recovery board startup timeout/retry logic** — `recovery.c:748–755`
   - `if (s_recovery_hardware_start_time_us >= 10 s)` compared an absolute monotonic
     timestamp rather than an elapsed interval, so it was always true once the Pi had been
     up ≥10 s → fixed to compare elapsed time
     (`get_monotonic_time_us() - start_time`).
   - `recovery_restart_count++` sat in the give-up (else) branch, so the restart path
     never incremented it → moved to the restart branch. The board now gets at most 5
     restarts before init returns `THREAD_ERR_HW` instead of blocking forever (the return
     value was also corrected from a `WT_RESULT` to the `THREAD_*` mask the launcher
     expects).

4. ✅ **ECG init errors never recorded** — `launcher.c:394`
   The ECG branch tested `pressure_result` instead of `ecg_result` → fixed.

5. ✅ **`imu reset` bound to audio start** — `subcommands/cmd_imu.c:13`
   `.parse = audioCmd_start` → fixed to `imuCmd_reset`.

6. ✅ **NMEA receive buffer unbounded** — `recovery.c:845–854`
   The payload length (up to 255) was copied into the 96-byte `nmea_sentence` without a
   check (a `// TODO` admitted it), and the whitespace-trim loop decremented a `uint8_t`
   length without a floor (underflow) → fixed with clamping to the buffer size and a
   zero-floored trim loop.

### Medium — resource cleanup / auxiliary features

7. ✅ **Audio page semaphore name typo** — `sensors/audio.c:412`
   `sem_audio_page` was opened on `AUDIO_BLOCK_SEM_NAME`, making both semaphores the same
   kernel object → fixed to `AUDIO_PAGE_SEM_NAME`.

8. ✅ **Light cleanup unlinked the ECG SHM** — `sensors/light.c:198`
   `shm_unlink(ECG_SHM_NAME)` → fixed to `LIGHT_SHM_NAME`.

9. ✅ **BMS NV overlay loop included the sentinel** — `battery.c:128`
   The loop bound used `sizeof`, iterating over the `{.name=NULL}` sentinel and writing 0
   to the STATUS (0x000) register as a side effect → fixed to stop at the sentinel,
   matching `battery_verify()`.

10. ✅ **`max17320_disable_discharging()` clobbered other COMM_STAT bits** —
    `max17320.c:270`. It computed `value |= DISCHARGE_OFF` but wrote the bare constant,
    which could clear a latched CHARGE_OFF → fixed to write `value`.

11. ✅ **Message length mismatch on Argos builds** — `cmd_recovery.c` vs `recovery.c`
    The CLI accepted up to 67 chars while `recovery_message()` rejects >24 on Argos (the
    send just failed). The limit is now unified as `RECOVERY_BOARD_MAX_MSG_LENGTH` in
    `recovery.h` (APRS 67 / Argos 24) and used by the CLI (this also removed an
    off-by-one stack buffer overflow possibility in the old CLI truncation).

### Minor — cosmetic/leftovers

12. ✅ `cmd_burnwire.c:14` — `burnwire off` replied "Turned burnwire on" → fixed to "off".
13. `cmd_audio.c:66-67` — `forceOverflow`/`simulateOverflow` descriptions swapped.
14. `recovery.c` — the GPS CSV declares 1 header column (`GPS`) but writes 4 fields per
    row; sleep/gps_only record the same internal state value; 3 spots of unreachable code
    after `return`.
15. `tmp/state_machine.{c,h}` — a stale, non-built copy committed at the repo root.
16. `debian/changelog` (2.3-1) vs `_versioning.h` (V2.5.1) version mismatch; the top two
    changelog entries have inverted dates.
17. The `.deb` `Depends` omits libFLAC (satisfied incidentally by the image build).
18. `build/Dockerfile` ends on a dangling `\` (tolerated by current BuildKit).
19. `ceti-tag-data-capture.service`: `ExecStartPre=systemctl ...` is not an absolute path,
    and there are no unit dependencies on the `/data` mount or pigpio.
20. `ltr329als.c` — the measurement-rate default is defined but never written.
21. The FIFO watermark math in the `cetiTag.h:50` comment (8 KB/25%) disagrees with the
    actual values (16 KB/50%).
22. The Wi-Fi PSK (`Talk2Whales`) and `pi` password (`ceticeti`) are hardcoded in plain
    text in the image.

## 4. Recent development themes (git history)

Since Oct 2025 on the `v2_5` line:

1. **State machine overhaul** (#117, Nov 2025): PREDEPLOY/FLOATING/LOW_POWER_BURN/SHUTDOWN
   states added, thread manager introduced, LED controller thread added, storage
   monitoring, subsystems reconfigured only on state changes
2. **Argos recovery board integration** (#119, Dec 2025): protocol split into the
   `libCetiRecovery` submodule, Argos ID/address/key/modulation IPC, board RTC time sync
3. **Recovery board startup timing** (#121/#122, Jan 2026): board power moved into
   core_init for parallel boot, ping retry loop (which carried the bugs of issue 3 above)
4. **Post-deployment fixes**: timing bug from the 2026-01-13 deployment, quit command
   bug, etc.

## 5. Fix status and remaining work

The high-priority issues 1–12 have all been fixed and verified (unit tests passing, all
modified files compile-checked).

### FLOAT_DETECTION enabled (done)

After fixing the three bugs of issue 2, `FLOAT_DETECTION` was set to **1**. Landed
alongside it:

- **Missing `break` in the FLOATING case fixed** — after the `__at_depth()`→DIVING
  transition there was no `break`, so with dive + non-upright simultaneously true, DIVING
  was immediately overwritten by SURFACE (latent while the feature was off)
- **Smoothing-buffer warm-up gating added** — right after a reset the moving average was
  biased toward zero (sum÷10 with a mostly-zero buffer), so a large attitude error could
  momentarily read as "upright". Float start/reset evaluation is now deferred until the
  buffer refills (10 s)
- **5 new unit tests** — SURFACE→FLOATING (20-min hold), no false positive in a
  non-floating attitude, FLOATING→DIVING (break regression), FLOATING→SURFACE (flipped),
  RETRIEVE→SHUTDOWN (floating). The test fakes were made controllable (`fake_euler`,
  `fake_monotonic_offset_s`)

**Behavior change**: a detached tag holding the vertical float attitude (pitch −85°±10°)
at the surface for 20 minutes now moves RECORD_SURFACE→RECORD_FLOATING and **starts Argos
transmission** (previously it stayed silent until the 4-day timeout). In RETRIEVE, 20
minutes of confirmed floating moves to SHUTDOWN, powering the Pi down to save energy —
the BMS FETs and the 3V3 RF rail stay up, so **the recovery-board beacon keeps running**
(`launcher.c`'s `reboot(POWER_OFF)` halts only the Pi; a full power cut happens only via
the separate `powerdown` command path).

**Caveat**: the original author noted in PR #107 that float detection "needs metrics
verification before being enabled in field". The −85° target pitch was determined
empirically (suction cups affect it), so **verifying the real float attitude on physical
hardware before a deployment is recommended**. If a problem shows up, set
`FLOAT_DETECTION` back to 0 in `state_machine.h`.

### Remaining suggestions

1. Measure the real float attitude (pitch/roll) on a physical tag →
   validate `FLOAT_DETECT_TARGET_PITCH_DEG`
2. Clean up the minor/leftover items from 13 on (swapped descriptions, removing `tmp/`,
   version sync, `.deb` dependency, systemd unit dependencies, …)
3. Add unit tests for the `recovery.c` protocol layer — every bug fixed in this pass
   lived in that untested area
