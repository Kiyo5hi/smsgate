---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0015: Hardware watchdog timer for 24/7 reliability

## Motivation

The bridge runs continuously and unattended. When something goes wrong,
it must recover on its own — there is no operator present to press the
reset button.

The existing recovery mechanism is application-level: `SmsHandler` counts
consecutive Telegram POST failures and, after `MAX_CONSECUTIVE_FAILURES`
(8), calls the injected `RebootFn` (→ `ESP.restart()`). This is useful for
escaping stuck TLS sessions, WiFi association failures, and DNS errors.
It does not cover the following failure modes:

**Loop hang.** `loop()` contains several blocking or semi-blocking
operations:

- `SerialAT.readStringUntil('\n')` — blocks until `\n` or the stream
  timeout fires. Under normal operation the default stream timeout is
  1 second; if `SerialAT` gets into a corrupted state it may block much
  longer.
- `modem.waitResponse(10000)` / `modem.waitResponse(60000UL)` — the
  60-second form is used inside `sendPduSms` for `AT+CMGS` (modem PDU
  send confirmation). If the modem stops producing `\n`-terminated
  output (e.g. a partial response due to cellular signal loss mid-PDU),
  this call blocks for the full timeout. Multiple such calls in sequence
  could pin the loop for several minutes.
- `telegramPoller->tick()` issues an HTTPS exchange. Although the WiFi
  TCP stack has its own timeouts, a hung TLS state machine has been
  observed in the wild on ESP32 — it can block indefinitely without
  tripping the Telegram failure counter (because the counter only
  increments on counted POST *failures*, not on hangs before the POST
  returns).
- `syncTime()` in `setup()` busy-loops on `time(nullptr) < 8*3600*2`
  with 500 ms sleeps. If NTP is unreachable for an extended period this
  blocks `setup()` indefinitely.
- The modem power-on probe in `setup()` — `while (!modem.testAT())`
  with 10 ms sleeps — spins forever if the modem hardware is in a
  broken state.

**Heap exhaustion crash without clean restart.** If a C++ exception or
heap allocation failure triggers a panic, the ESP-IDF panic handler
prints a backtrace and halts. It does NOT call `ESP.restart()`.
Depending on the reset configuration the chip may halt at the panic or
reboot immediately, but this is outside application control. A hardware
watchdog ensures the chip comes back even if the software path to
`ESP.restart()` is never reached.

**Silent task hang.** A subtle deadlock — e.g. two AT exchanges
accidentally interleaved so each is waiting for the other's response —
would not trip any of the existing failure counters. The loop simply
stops making forward progress. The hardware watchdog is the only
mechanism that detects this class of failure.

None of these scenarios advance the `consecutiveFailures_` counter
in `SmsHandler`, so the existing soft-reboot path never fires.

## Current state

Recovery today is entirely application-level:

```
SmsHandler::noteTelegramFailure()
    consecutiveFailures_++
    if consecutiveFailures_ >= MAX_CONSECUTIVE_FAILURES (8):
        reboot_()   →   ESP.restart()
```

`reboot_` is the lambda defined in `main.cpp`:

```cpp
static SmsHandler smsHandler(
    realModem, realBot,
    []() {
        delay(1000);
        ESP.restart();
    },
    []() -> unsigned long { return millis(); });
```

`ESP.restart()` triggers a software reset (`ESP_RST_SW`), which is
already reported correctly by the `/status` command (RFC-0010 — the
`esp_reset_reason()` switch in the `StatusFn` lambda in `main.cpp`
handles `ESP_RST_WDT` too, so watchdog-triggered reboots will be
surfaced to the user with no additional code change).

There is no hardware or task watchdog configured anywhere in the
codebase today. The ESP-IDF default is to leave the task watchdog
disabled in Arduino builds.

## Plan

### 1. Required header

```cpp
#include <esp_task_wdt.h>
```

Add this include to `main.cpp` alongside the existing `<esp_system.h>`.

### 2. Initialise the watchdog in `setup()`

Call after all hardware initialisation is complete — specifically after
the modem power-on sequence, the SMS configuration AT commands, and the
WiFi / NTP setup — so the watchdog is not armed during the blocking
startup steps that cannot easily call `esp_task_wdt_reset()` (the modem
probe loop, the `waitResponse(100000UL)` "SMS DONE" wait, and the NTP
busy-loop in `syncTime()`). The right insertion point is immediately
before the `realBot.sendMessage("🚀 ...")` boot banner at the end of
`setup()`, after `sweepExistingSms()` has run:

```cpp
// Arm the hardware task watchdog. From this point on, loop() must call
// esp_task_wdt_reset() at least once every kWdtTimeoutSec seconds or
// the chip will hard-reset. The timeout is generous enough to cover the
// longest single blocking operation (AT+CMGS waitResponse 60 s) with
// headroom for back-to-back AT exchanges. See RFC-0015.
esp_task_wdt_config_t wdtCfg = {
    .timeout_ms = kWdtTimeoutSec * 1000u,
    .idle_core_mask = 0,     // don't watch the idle task
    .trigger_panic = false,  // trigger reset, not panic+halt
};
esp_task_wdt_init(&wdtCfg);
esp_task_wdt_add(NULL);      // subscribe the current (loop) task
```

Add the constant near the top of `main.cpp`:

```cpp
static constexpr uint32_t kWdtTimeoutSec = 120;  // 2 minutes
```

### 3. Feed the watchdog in `loop()`

Place a single `esp_task_wdt_reset()` call at the top of `loop()`,
before the URC drain:

```cpp
void loop()
{
    esp_task_wdt_reset();   // RFC-0015: keep the hardware watchdog alive

    // NOTE: do NOT call modem.maintain() here. ...
```

Placing the reset at the very top of `loop()` means the watchdog is
fed on every complete loop iteration. The loop body (URC drain →
`callHandler.tick()` → CSQ refresh → `telegramPoller->tick()` →
`smsSender.drainQueue()` → WiFi check → `delay(50)`) must complete
within `kWdtTimeoutSec` for the chip to stay alive.

### 4. Timeout value rationale

120 seconds is chosen as follows:

| Blocking operation | Max duration |
|--------------------|--------------|
| `modem.waitResponse(60000UL)` in `sendPduSms` | 60 s |
| `modem.waitResponse(10000)` × ~6 AT commands in SMS path | ~60 s |
| `telegramPoller->tick()` HTTPS exchange | ~10–20 s |
| `delay(50)` per iteration | 0.05 s |

The single worst case is one `sendPduSms` (60 s) back-to-back with one
set of SMS setup commands (60 s in aggregate), giving ~120 s. The 120 s
timeout sits at that ceiling.

If the codebase later adds longer blocking AT operations, either raise
`kWdtTimeoutSec` or insert an additional `esp_task_wdt_reset()` call
inside the blocking section.

### 5. Interaction with long AT operations

The `sendPduSms` path in `RealModem` uses `waitResponse(60000UL)` — a
60-second timeout. Under the 120 s WDT, a single call is safe. A
pathological sequence of two 60-second calls arriving back-to-back
(e.g. `sweepExistingSms` processing two PDU sends in the first loop
iteration after startup) would hit the limit exactly. Three consecutive
60-second waits would trip the watchdog.

**Recommendation**: keep `kWdtTimeoutSec = 120` for the initial
implementation and accept the theoretical edge-case at the limit. If
false-positive watchdog resets are observed in the monitor logs
(`Reboot reason: watchdog` in the `/status` output), the first
remediation is to raise `kWdtTimeoutSec` to 180. A deeper fix would be
to insert `esp_task_wdt_reset()` between the PDU chunks inside the
`for` loop in `RealModem::sendPduSms`, but that adds hardware
coupling to a class that is otherwise thin.

### 6. Interaction with `setup()` blocking sections

`setup()` contains several unbounded loops that must NOT be covered by
the watchdog:

- `while (!modem.testAT())` — spins until the modem responds.
- `waitResponse(100000UL, "SMS DONE")` — 100-second SMS stack
  initialisation wait.
- `syncTime()` — NTP busy-loop with no upper bound.
- The network registration loop (`while status == REG_SEARCHING...`).

The watchdog is intentionally armed **after** all of these complete (at
the very end of `setup()`, as described in section 2). No changes to
these loops are needed.

### 7. IDF v4 vs. IDF v5 API

The project uses `espressif32@6.11.0` which bundles ESP-IDF v5.x. The
`esp_task_wdt_init` API changed between IDF v4 and v5:

| IDF version | `esp_task_wdt_init` signature |
|-------------|-------------------------------|
| IDF v4      | `esp_task_wdt_init(uint32_t timeout_ms, bool panic)` |
| IDF v5      | `esp_task_wdt_init(const esp_task_wdt_config_t *config)` |

The struct-based form shown in section 2 is the IDF v5 API. If the
project ever pins to an older `espressif32` release that bundles IDF v4
(anything before `espressif32@6.0.0`), the call must be changed to:

```cpp
esp_task_wdt_init(kWdtTimeoutSec * 1000u, /*panic=*/false);
```

The `trigger_panic = false` choice applies to both forms: we want a
clean reset, not a panic + halt, so the chip comes back and the
`Reboot reason: watchdog` is visible via `/status` without requiring a
serial monitor to capture the panic trace.

### 8. Scope

This is a small, self-contained change:

- `src/main.cpp`: add `#include <esp_task_wdt.h>`, add the
  `kWdtTimeoutSec` constant, add the init block at the end of `setup()`,
  add the single `esp_task_wdt_reset()` at the top of `loop()`.
- No other files change.
- Estimated new lines of code: ~10.

No new interfaces, no new tests required (the watchdog is a hardware
mechanism; host-native tests run under MinGW without ESP-IDF and cannot
exercise it). The observable effect in tests is zero; the observable
effect in production is that the `/status` command will report
`Reboot reason: watchdog` when a hang is recovered, instead of
`unknown (...)`.

## Notes for handover

- **`esp_task_wdt.h` availability.** The header ships with ESP-IDF and
  is always available in the `espressif32` platform. It does not require
  any additional `lib_deps` entry in `platformio.ini`.

- **`esp_task_wdt_reset()` is cheap.** It writes a single register;
  calling it at the top of every `loop()` iteration (nominally every
  50 ms) has no measurable overhead.

- **`trigger_panic = false` vs. `true`.** Setting `trigger_panic = true`
  would cause the IDF to capture a backtrace before resetting — useful
  for debugging. The downside is that the board halts at the backtrace
  unless a serial monitor is attached. For a production bridge that
  must self-recover, `false` is the correct default. A
  `-DWDT_TRIGGER_PANIC` build flag could conditionally set this for
  debugging sessions.

- **The idle task is not subscribed.** `idle_core_mask = 0` means the
  IDF's idle task is not watched. Only the Arduino loop task (the task
  calling `esp_task_wdt_add(NULL)`) is subscribed. This avoids false
  WDT resets triggered by FreeRTOS internals during heavy WiFi/TLS
  processing on core 1.

- **`ESP_RST_WDT` is already handled.** The `esp_reset_reason()` switch
  in the `StatusFn` lambda (main.cpp, the `/status` command from
  RFC-0010) already has a `case ESP_RST_WDT: rebootReason = "watchdog"`
  branch. No change needed there.

- **False-positive watchdog resets are a signal, not noise.** If
  `/status` starts reporting `Reboot reason: watchdog` regularly, that
  is evidence of a real hang — investigate the last known operation in
  the serial log rather than simply raising `kWdtTimeoutSec` without
  understanding the root cause.

## Review

```
verdict: approved-with-changes
reviewer: claude-sonnet-4-6
date: 2026-04-09
```

This is a post-implementation review. The implementation is in
`src/main.cpp` and `src/sms_sender.cpp`. Two of the three issues from
the original pre-implementation review are resolved by the code; one
new BLOCKING issue was introduced by the implementation diverging from
the RFC plan.

### Verified correct

- **`#ifdef ESP_PLATFORM` guard in `sms_sender.cpp`** (`lines 4-6`,
  `56-58`): MinGW does not define `ESP_PLATFORM`, so the guard correctly
  excludes `esp_task_wdt_reset()` from the native host build. The
  include and the call are both inside the same guard. No issue.

- **`esp_task_wdt_reconfigure` vs. `esp_task_wdt_init`**: The
  implementation uses `esp_task_wdt_reconfigure(&wdtCfg)` (`main.cpp`
  line 584) rather than `esp_task_wdt_init` as the RFC Plan §2 showed.
  `esp_task_wdt_reconfigure` is the correct IDF v5 API — IDF v5
  deprecated `esp_task_wdt_init` and split initialisation from
  reconfiguration. The RFC §7 table documents the v5 struct-based API
  but names the wrong function; the implementation is more correct than
  the RFC text. No issue.

- **WDT init placement** (`main.cpp` lines 575-587): The init block is
  after the boot banner (`line 570`) AND after `sweepExistingSms()`
  (`line 573`). The prior pre-implementation review flagged the RFC
  text as misleading because it described init "before the boot
  banner". The implementation placed it correctly after both. Resolved.

- **`esp_task_wdt_reset()` at top of `loop()`** (`main.cpp` line 592`):
  This is literally the first statement in `loop()`, before the URC
  drain, before any other work. Correct.

- **`esp_task_wdt_reset()` inside `SmsSender::send()` `for` loop**
  (`sms_sender.cpp` line 57): Present and guarded with `#ifdef
  ESP_PLATFORM`. This resolves the prior BLOCKING issue about multi-part
  sends exceeding 120 s.

- **`esp_task_wdt_reset()` inside `syncTime()` while-loop**
  (`main.cpp` line 212`): Present. The call is the first statement in
  the loop body, before `delay(500)`, so each 500 ms sleep is
  individually within the 120 s budget. This resolves the prior
  BLOCKING issue about unbounded NTP waits from `loop()`.

- **`idle_core_mask = 0`**: Correct; excludes FreeRTOS idle task from
  watchdog monitoring to avoid false positives during heavy WiFi/TLS
  processing.

### BLOCKING issues

- **[BLOCKING] `trigger_panic = true` contradicts the RFC and breaks
  `/status` reporting.**
  The implementation sets `.trigger_panic = true` (`main.cpp` line 582).
  The RFC is unambiguous: Plan §2 specifies `trigger_panic = false`,
  §7 repeats `trigger_panic = false`, and the "Notes for handover"
  bullet explicitly justifies the choice: "we want a clean reset, not a
  panic + halt, so the chip comes back and the `Reboot reason: watchdog`
  is visible via `/status` without requiring a serial monitor."
  The implementation inverts this. With `trigger_panic = true`, the IDF
  captures a backtrace and then resets with reason `ESP_RST_PANIC`, NOT
  `ESP_RST_WDT`. The `/status` command switch (`main.cpp` lines 539-549)
  maps `ESP_RST_PANIC` to `"panic/exception"` and `ESP_RST_WDT` to
  `"watchdog"`. A watchdog-triggered recovery will therefore report as
  `"Reboot reason: panic/exception"` — indistinguishable from a genuine
  software crash. The RFC's stated goal ("the `/status` command will
  report `Reboot reason: watchdog` when a hang is recovered") is not
  met. Additionally, if a serial monitor is not attached, the backtrace
  output is discarded and the board halts waiting for the IDF panic
  handler to complete; whether the reset actually fires depends on the
  IDF `CONFIG_ESP_SYSTEM_PANIC` Kconfig setting. For a production
  unattended bridge, `trigger_panic = false` is the only safe choice.
  Resolution: change `.trigger_panic = true` to `.trigger_panic = false`
  in `main.cpp`.

### NON-BLOCKING issues

- **[NON-BLOCKING] `sendPduSms` per-call budget is mis-stated in §4.**
  The timeout table lists `modem.waitResponse(60000UL) in sendPduSms`
  as 60 s, but `sendPduSms` also issues `waitResponse(10000UL)` for
  the `>` prompt beforehand, making the single-call worst-case 70 s,
  not 60 s. The per-PDU `esp_task_wdt_reset()` in `SmsSender::send()`
  makes this a non-issue for correctness, but the table is still
  inaccurate and should be updated.

- **[NON-BLOCKING] RFC §2 insertion-point description remains stale.**
  The text still says "immediately before the
  `realBot.sendMessage("🚀 ...")` boot banner", but the implementation
  (correctly) placed it after both the banner and `sweepExistingSms()`.
  The RFC text should be updated to match.

- **[NON-BLOCKING] No `kWdtTimeoutSec` named constant in
  implementation.**
  The RFC Plan §2 suggested defining `static constexpr uint32_t
  kWdtTimeoutSec = 120` and using it as `kWdtTimeoutSec * 1000u`. The
  implementation hardcodes `.timeout_ms = 120000` directly. The
  behaviour is identical, but a named constant would make the intent
  clearer and the value easier to change. Low priority.

- **[NON-BLOCKING] No host-native test coverage, and that is correct.**
  The WDT mechanism cannot be exercised in the MinGW native env.
  `main.cpp` is excluded from the native build entirely. No tests are
  needed.

### Summary

The implementation correctly resolves the two blocking issues from the
pre-implementation review: `esp_task_wdt_reset()` is present inside
the `SmsSender::send()` PDU loop (guarded by `#ifdef ESP_PLATFORM`),
and inside `syncTime()`'s while-loop; and the WDT init block is placed
correctly after both `sweepExistingSms()` and the boot banner. The
`#ifdef ESP_PLATFORM` guard in `sms_sender.cpp` is correct and excludes
the ESP-IDF call from native tests. The use of `esp_task_wdt_reconfigure`
is correct for IDF v5 and is actually an improvement over what the RFC
planned. However, one new blocking issue was introduced: the
implementation sets `trigger_panic = true`, which contradicts the RFC's
stated rationale, causes watchdog-triggered recoveries to appear as
`"panic/exception"` instead of `"watchdog"` in `/status`, and may not
guarantee a self-recovery on boards where the IDF panic handler halts.
This single line must be changed to `trigger_panic = false` before the
implementation can be considered correct.
