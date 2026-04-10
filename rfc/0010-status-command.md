---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0010: `/status` Telegram bot command for device health reporting

## Motivation

The bridge runs unattended on hardware that has no screen, no LEDs
visible to the user, and no web dashboard. Today the only way to know
whether the device is healthy is to check serial output over USB or
wait for something to break. The existing `/debug` command shows a
ring buffer of recent SMS parse events, which is useful for diagnosing
encoding issues but says nothing about WiFi signal, cellular signal,
heap pressure, uptime, or throughput.

A `/status` command lets the owner check device health from Telegram
at any time, without a serial connection. This is especially useful
after the device has been running for days: "Is it still connected?
Is memory leaking? Is the modem registered? How many messages has it
handled?"

## Current state

- **`/debug`** is the only bot command. It dumps `SmsDebugLog::dump()`
  (last 20 SMS diagnostic records). Registered in
  `registerBotCommands()` (`telegram.cpp` line 64) and dispatched in
  `TelegramPoller::processUpdate()` (`telegram_poller.cpp` line 66).
- **Serial logging** prints signal quality, registration status, WiFi
  connection, and heap only at boot or on specific events. None of
  this is accessible from Telegram.
- **Counters that exist today**: `TelegramPoller::pollAttempts_` (int,
  accessible via `pollAttempts()`), `TelegramPoller::lastUpdateId_`
  (int32_t, accessible via `lastUpdateId()`),
  `SmsHandler::consecutiveFailures_` (int, accessible via
  `consecutiveFailures()`), `ReplyTargetMap::occupiedSlots()` (size_t,
  O(N) scan), `SmsDebugLog::count()` (size_t).
- **Counters that do NOT exist yet**: total SMS forwarded, total SMS
  forward failures, total SMS replies sent. `SmsHandler` resets
  `consecutiveFailures_` to zero on every success but never
  accumulates a lifetime total.

## Plan

### 1. Data sources and where they live

| Metric | Source object | API today | New work needed |
|---|---|---|---|
| Uptime | `millis()` | Global Arduino | None; format in the handler |
| WiFi RSSI | `WiFi.RSSI()` | Global Arduino/ESP32 | None |
| Modem signal quality (CSQ) | `modem.getSignalQuality()` | `TinyGsm` global in `main.cpp` | Expose via new `IModem` method or pass as a callback (see below) |
| Free heap | `ESP.getFreeHeap()` | Global ESP32 | None; call directly in the handler |
| Reply-target occupancy | `replyTargets.occupiedSlots()` | Already public | None |
| SMS forwarded / failed | `SmsHandler` | Does NOT exist | Add `smsForwarded_` and `smsFailed_` counters |
| Poll attempts | `TelegramPoller::pollAttempts()` | Already public | None |
| Last update_id | `TelegramPoller::lastUpdateId()` | Already public | None |
| Modem registration status | `modem.getRegistrationStatus()` | `TinyGsm` global in `main.cpp` | Same access path as CSQ — callback or new `IModem` method |
| Last reboot reason | `esp_reset_reason()` | ESP-IDF global | None; call directly |

### 2. Wiring: how TelegramPoller gets the data

The `/status` handler runs inside `TelegramPoller::processUpdate()`,
which already handles `/debug`. The problem is that TelegramPoller
does not (and should not) hold references to every object in the
system. We have two clean options:

**Option A: StatusFn callback (recommended).** Add a single
`std::function<String()>` callback to TelegramPoller, set from
`main.cpp` where all objects are in scope. The callback captures
references to `smsHandler`, `replyTargets`, `modem`, etc. and builds
the formatted status string. This follows the same pattern as
`ClockFn` and `AuthFn` -- a lambda set from the composition root.

```cpp
// telegram_poller.h
using StatusFn = std::function<String()>;
void setStatusFn(StatusFn fn) { statusFn_ = std::move(fn); }
```

```cpp
// main.cpp (sketch)
telegramPoller.setStatusFn([&]() -> String {
    return buildStatusMessage(smsHandler, replyTargets,
                              telegramPoller, modem);
});
```

This keeps TelegramPoller decoupled from SmsHandler, TinyGsm, WiFi,
and ESP internals. The `/status` dispatch in `processUpdate()` just
calls `statusFn_()` and sends the result.

**Option B: Struct passed through.** Define a `DeviceStatus` struct,
add a `StatusProvider` interface, implement it in `main.cpp`. More
formally testable but heavier for a single read-only query. Overkill
for now.

Go with Option A.

### 3. New SMS counters on SmsHandler

Add two private counters and public accessors:

```cpp
// sms_handler.h
int smsForwarded() const { return smsForwarded_; }
int smsFailed() const { return smsFailed_; }

private:
    int smsForwarded_ = 0;
    int smsFailed_ = 0;
```

Increment `smsForwarded_` in `forwardSingle()` on success and in
`insertFragmentAndMaybePost()` when a completed concat group is
posted. Increment `smsFailed_` in `noteTelegramFailure()`. These are
non-persistent RAM counters -- they reset on reboot, which is fine;
the `/status` output will include the uptime so the user can
contextualize the numbers.

### 4. Modem CSQ and registration status

`TelegramPoller` has no access to the `TinyGsm modem` global. Two
approaches:

- **Preferred: include in the StatusFn closure.** The lambda in
  `main.cpp` calls `modem.getSignalQuality()` and
  `modem.getRegistrationStatus()` directly, since `modem` is in scope
  there. No changes to `IModem` needed. The AT commands (`AT+CSQ`,
  `AT+CREG?`) take <100ms and run inline -- acceptable for a
  user-initiated command that fires at most a few times per day.
- **Alternative: add to IModem.** Would make the values available to
  unit tests via `FakeModem`, but the `/status` handler itself is not
  unit-testable anyway (it calls `WiFi.RSSI()`, `ESP.getFreeHeap()`,
  etc.). Not worth the interface churn.

### 5. Reboot reason

ESP-IDF exposes `esp_reset_reason()` returning an `esp_reset_reason_t`
enum. Map the common values to short strings:

| Value | String |
|---|---|
| `ESP_RST_POWERON` | "power-on" |
| `ESP_RST_SW` | "software (ESP.restart)" |
| `ESP_RST_PANIC` | "panic/exception" |
| `ESP_RST_WDT` | "watchdog" |
| `ESP_RST_BROWNOUT` | "brownout" |
| other | "unknown (N)" |

Call once at the top of `buildStatusMessage()`. No need to cache it --
it doesn't change during a session.

### 6. Command dispatch in TelegramPoller

Extend the existing `/debug` branch in `processUpdate()`:

```cpp
// telegram_poller.cpp, inside processUpdate(), after auth gate
if (u.replyToMessageId == 0)
{
    // ... existing lower-case conversion ...
    if (lower == "/debug") { ... }

    if (lower == "/status")
    {
        if (statusFn_)
            bot_.sendMessage(statusFn_());
        else
            bot_.sendMessage(String("(status not configured)"));
        return;
    }

    // ... existing generic help message ...
}
```

Update the generic help/error message (line 80-82 of
`telegram_poller.cpp`) to mention `/status`:

```
"Reply to a forwarded SMS to send a response. "
"Use /debug for the SMS diagnostic log, /status for device health."
```

### 7. Register the command in Telegram UI

In `registerBotCommands()` (`telegram.cpp`), add a second entry to the
`commands` JSON array so Telegram's autocomplete shows both commands:

```cpp
JsonObject cmd1 = cmds.createNestedObject();
cmd1["command"] = "debug";
cmd1["description"] = "Show SMS diagnostic log";

JsonObject cmd2 = cmds.createNestedObject();
cmd2["command"] = "status";
cmd2["description"] = "Show device health and stats";
```

Bump the `DynamicJsonDocument` capacity from 256 to 384 to accommodate
the second entry. Update the success log line to
`"Bot commands registered: /debug, /status"`.

### 8. Message format

Compact, monospace-friendly, fits in a single Telegram message:

```
--- Device Status ---
Uptime: 3d 14h 22m
WiFi RSSI: -62 dBm
Modem CSQ: 18 (decent)
Registration: home
Free heap: 142,384 bytes
Reboot reason: software (ESP.restart)

--- SMS Stats ---
Forwarded: 47
Failed: 2
Consecutive failures: 0
Concat groups in flight: 1

--- Telegram ---
Reply-target slots: 47/200
Poll attempts: 14,208
Last update_id: 928,374,112

--- Debug Log ---
Entries: 12/20
```

CSQ interpretation (for the parenthetical label): 0-9 = "marginal",
10-14 = "ok", 15-19 = "good", 20-31 = "excellent", 99 = "unknown/no
signal". These match the 3GPP TS 27.007 convention.

### 9. Access control

Same as `/debug` -- the command runs inside `processUpdate()` which
has already passed the `AuthFn` gate. No additional authorization
needed. Unauthorized users never reach the command dispatch block.

### 10. Testing

**Host-testable paths:**

- The new `smsForwarded_` / `smsFailed_` counters on `SmsHandler`:
  extend the existing native tests to assert that successful forwards
  increment `smsForwarded()` and Telegram failures increment
  `smsFailed()`.
- The `/status` command dispatch in `TelegramPoller`: set a fake
  `StatusFn` that returns a canned string, send a `/status` update
  through `processUpdate()`, assert that `FakeBotClient` received
  the canned string.
- The help message update: send a non-reply, non-command message and
  assert the error reply mentions `/status`.

**Not host-testable (hardware-dependent):**

- The actual `buildStatusMessage()` function in `main.cpp` calls
  `WiFi.RSSI()`, `ESP.getFreeHeap()`, `esp_reset_reason()`, and
  `modem.getSignalQuality()`. These are ESP32/TinyGSM globals that
  don't exist in the native env. Test manually on hardware.

**Manual hardware test:**

1. Flash, boot, wait for "online" banner.
2. Send `/status` in Telegram. Verify the response contains all
   nine sections with plausible values.
3. Wait 10+ minutes, send `/status` again. Verify uptime has
   advanced.
4. Send an SMS to the SIM, wait for the forward, then send `/status`.
   Verify "Forwarded: 1".
5. Send `/status` from an unauthorized chat. Verify no response.

## Notes for handover

- **Do not add persistent (NVS) counters.** The flash wear budget is
  already tight from the reply-target ring buffer and the update_id
  watermark. RAM counters that reset on reboot are sufficient for a
  health check; lifetime stats can go in a future "metrics" RFC if
  anyone needs them.
- **The StatusFn closure captures by reference.** This is safe because
  all captured objects (`smsHandler`, `replyTargets`, `modem`, etc.)
  are file-scope statics in `main.cpp` with program lifetime. Do not
  refactor them into locals without updating the capture.
- **`modem.getSignalQuality()` sends `AT+CSQ` synchronously.** This
  blocks the main loop for ~50-100ms. Acceptable for a manually-
  triggered command, but do not call it on every `tick()`. Same
  applies to `modem.getRegistrationStatus()` (`AT+CREG?`).
- **Message length.** The formatted status message is well under
  Telegram's 4096-character limit. If future metrics push it close,
  split into two `sendMessage` calls rather than truncating.
- **`build_src_filter` for native env.** No changes needed -- the
  StatusFn lives in `main.cpp` which is already excluded from the
  native build. The new `SmsHandler` counter accessors are in the
  header, which is already compiled in native.
- **Existing `consecutiveFailures()` accessor** is documented as
  "test-only" but is perfectly usable here. No need to add a second
  accessor.

## Review

**verdict: approved-with-changes**

**Reviewer:** claude-sonnet-4-6, 2026-04-09

### Issues

- **BLOCKING — `setStatusFn` is a post-construction setter, not a constructor parameter.** Every other optional dependency on `TelegramPoller` uses the `setXxx` pattern (`setDebugLog`), which is consistent, but the RFC never notes the hazard: if `tick()` fires before `setStatusFn` is called (possible if the main loop starts before `setup()` finishes all wiring), a `/status` command during that window falls through to the "(status not configured)" fallback silently. The RFC should either (a) mandate the call order explicitly, or (b) promote `StatusFn` to a constructor parameter like `ClockFn` and `AuthFn` are. Given how tightly `StatusFn` couples to all the objects in `main.cpp`, option (b) is cleaner and consistent with the existing constructor signature style. The current draft leaves this unaddressed.

- **BLOCKING — `modem.getSignalQuality()` and `modem.getRegistrationStatus()` are called on the raw `TinyGsm modem` global from inside the `StatusFn` closure, but `TelegramPoller::tick()` may be mid-loop at a point where a `+CMTI` or `RING` URC is actively being drained from `SerialAT`.** Both `AT+CSQ` and `AT+CREG?` issue real AT commands synchronously and call `waitResponse` internally, which reads from `SerialAT`. If a `+CMTI` URC line arrives during that `waitResponse` read, TinyGSM will eat it (log `### Unhandled:` in debug mode) and the SMS will be silently lost — exactly the class of bug the CLAUDE.md "do not call `modem.maintain()`" trap warns about. The RFC acknowledges the 50–100 ms blocking time but frames it only as a latency concern; it completely misses the URC-eating hazard. The fix is to add `getSignalQuality()` and `getRegistrationStatus()` as virtual methods on `IModem` and call them from a dedicated, clearly-bounded point in the loop (e.g. immediately after `telegramPoller.tick()` returns and before the WiFi check), not from within a callback that fires inside `processUpdate`. Alternatively, cache the last-seen CSQ/registration values updated at the existing boot-time polling site and refresh them lazily (e.g. at most once per 60 s, outside any AT-response window). Either way, calling modem AT commands from inside a Telegram HTTP response handler is the wrong layering.

- **BLOCKING — `smsFailed_` counter semantics conflict with `consecutiveFailures_` reset logic.** The RFC says to increment `smsFailed_` inside `noteTelegramFailure()`. But `consecutiveFailures_` is reset to zero on every success in `forwardSingle` and `insertFragmentAndMaybePost`. After a reboot triggered by 8 consecutive failures, `smsFailed_` would reset to 0 along with everything else. That is the stated intent ("RAM counters that reset on reboot"). But the RFC also says to increment `smsFailed_` in `noteTelegramFailure()`, yet `noteTelegramFailure()` is also called on partial-concat Telegram failures where `consecutiveFailures_` is bumped but the group is put back for retry. A single concat group that triggers 3 retries before success would inflate `smsFailed_` by 3 even though zero messages were permanently lost. The RFC should clarify whether `smsFailed_` counts "Telegram attempts that returned false" (current proposal) or "messages permanently undeliverable." The former is misleading to the end user; the latter requires a different increment site.

- **NON-BLOCKING — `DynamicJsonDocument` capacity bump from 256 to 384 in `registerBotCommands`.** The current ArduinoJson v6 usage in `telegram.cpp` uses `JSON_OBJECT_SIZE` elsewhere and the doc budget is already tight in several places. A second `setMyCommands` entry with a longer description string ("Show device health and stats" = 26 chars vs "Show SMS diagnostic log" = 23 chars) will serialize to roughly 110 additional bytes. 384 bytes total is correct and sufficient, but the RFC should note to verify with `measureJson()` in a test rather than guessing, given that ArduinoJson v6 includes string storage in `JSON_OBJECT_SIZE` calculations only when the string is not a `const char *` literal.

- **NON-BLOCKING — `concatKeyCount()` is not mentioned in the status message format but `concatGroups in flight` is.** The accessor already exists on `SmsHandler` as `concatKeyCount()` (test-only, per header comments). The RFC correctly identifies it as usable for status. However it should note that calling `concatKeyCount()` from the `StatusFn` closure is safe because it is a const method on an object captured by reference, with no AT-command side effects. This is the one modem-free counter that needs no special handling.

- **NON-BLOCKING — `esp_reset_reason()` availability.** The RFC states this is an "ESP-IDF global" and calls it directly. In the Arduino ESP32 framework (arduino-esp32), `esp_reset_reason()` is declared in `esp_system.h` which is pulled in transitively via `esp_arduino_versions.h`. It is available and works correctly at runtime. However, the RFC does not mention that the `#include <esp_system.h>` directive may need to be added to `main.cpp` explicitly if the transitive include chain changes across ESP32 Arduino core versions. Flag this as a "verify the include is present" note during implementation, not a design issue.

- **NON-BLOCKING — The `/status` command dispatch is proposed as a new `if (lower == "/status")` block inserted between the `/debug` block and the generic error.** The existing code in `processUpdate()` falls through to `sendErrorReply(...)` for any non-reply, non-`/debug` message. A user who sends `/status` while `statusFn_` is null gets both the `(status not configured)` message AND silently returns — the `return` in the RFC's snippet is correct, but the draft code snippet should make this explicit to avoid confusion during review.

- **NON-BLOCKING — The `setDebugLog` precedent is a post-construction optional setter, but `StatusFn` is semantically different.** `debugLog_` is truly optional (the code falls back gracefully). `StatusFn` is also optional as designed, but the on-hardware use case always has it set. The RFC should acknowledge that "status not configured" is a development-only code path and that the integration test (step 2 of the manual test plan) verifies the happy path only. A unit test asserting the "(status not configured)" fallback would close this gap.

- **NON-BLOCKING — Reply-target occupancy uses `O(N)` scan.** The RFC documents this correctly in the "Current state" table. However, the status message format shows "Reply-target slots: 47/200", implying a display of occupied / total. Since `/status` is at most a few calls per day, the O(200) scan is entirely acceptable. No action needed — just confirming this is correctly characterized.

- **NON-BLOCKING — `smsFailed_` increment site in `insertFragmentAndMaybePost`.** The RFC says to increment on Telegram failure during concat completion. However, `insertFragmentAndMaybePost` already has special retry logic: on failure it puts the fragments back and returns false so the caller leaves the SIM slot in place. Incrementing `smsFailed_` here would count the same multi-part message once per failed attempt, not once per permanently failed message. This is the same ambiguity as the BLOCKING item above, but specifically for the concat path.

### Summary

The RFC is well-structured, the motivation is solid (unattended hardware, no screen, no dashboard), and the `StatusFn` callback pattern is the right architectural choice — it keeps `TelegramPoller` decoupled while letting `main.cpp` assemble the full picture from objects already in scope. The implementation plan is sequenced correctly and the test coverage section is honest about what can and cannot be exercised on host. Two blocking issues must be resolved before implementation: (1) the post-construction wiring order must be formalized or `StatusFn` promoted to a constructor parameter, and (2) the AT-command calls inside the `StatusFn` closure create a real URC-eating hazard that mirrors the banned `modem.maintain()` pattern — the RFC must prescribe a safe call site (cached values or a dedicated out-of-band refresh) rather than calling `getSignalQuality()` and `getRegistrationStatus()` from inside the Telegram HTTP response processing path. The `smsFailed_` counter semantics also need clarification before the increment sites can be coded correctly.

## Code Review

**verdict: approved-with-changes**

**Reviewer:** claude-sonnet-4-6, 2026-04-09

Files reviewed: `src/sms_handler.h`, `src/sms_handler.cpp`, `src/telegram_poller.h`,
`src/telegram_poller.cpp`, `src/telegram.cpp`, `src/main.cpp`,
`test/test_native/test_telegram_poller.cpp`, `test/test_native/test_sms_handler.cpp`.

### Issues

**BLOCKING — `smsFailed_` header comment contradicts actual increment semantics.**
`sms_handler.h` lines 98-100 document `smsFailed_` as "messages permanently
undeliverable (all retries exhausted, or single-part Telegram failure after
reboot-recovery)." The actual code increments it in `noteTelegramFailure()`
(`sms_handler.cpp` line 316), which is called on every Telegram send attempt
that returns false — including transient failures on a concat group that is
retained in RAM and will be retried on the next `+CMTI`. A two-part SMS whose
completion post fails three times before succeeding would show `smsFailed_ = 3`
with `smsForwarded_ = 1`, implying three messages were permanently lost when
none were. The test `test_smsFailed_increments_on_telegram_failure` correctly
documents this "attempt-level" behavior, making the test accurate but the
header comment wrong. The inverse gap also exists: if a concat group is silently
evicted by the LRU or TTL evictors (`sms_handler.cpp` lines 45-59,
`evictLruUntilUnderCaps`), `smsFailed_` is never incremented even though the
message is permanently undeliverable. Either (a) rename `smsFailed_` to
`telegramSendFailures_` and update the header comment and the status message
label to "Send failures:", or (b) track evictions and count those, too, but
update the header to be explicit about what is and is not counted. Option (a)
is the lower-cost fix.

**NON-BLOCKING — Loop order is correct, but the safety comment in `loop()` is
slightly misleading.** `main.cpp` lines 491-509 order the blocks as: (1) URC
drain, (2) `callHandler.tick()`, (3) 30s CSQ/reg cache refresh, (4)
`telegramPoller->tick()`. The inline comment at lines 493-495 says "This block
runs AFTER the URC drain and BEFORE the TelegramPoller tick" — accurate. The
comment at `main.cpp` lines 311-315 (in `setup()`) also correctly documents
the ordering rationale for the StatusFn closure. No code change needed; just
noting the prior review's blocking URC-safety concern has been addressed
correctly.

**NON-BLOCKING — `TelegramPoller` constructor change is backward-compatible and
correct.** The 7th parameter `StatusFn status = nullptr` (`telegram_poller.h`
line 85) defaults to null. All pre-existing tests pass 6 arguments (clock +
auth), relying on the default. The new tests pass 7. No regression. The
`setStatusFn` setter proposed in the RFC was not implemented — instead
`StatusFn` was promoted to a constructor parameter, which is the cleaner
approach the prior review recommended.

**NON-BLOCKING — All symbol dependencies compile cleanly.** Checked:
`SmsDebugLog::count()` exists at `sms_debug_log.h` line 37;
`SmsDebugLog::kMaxEntries` at line 29; `SmsHandler::smsForwarded()` and
`smsFailed()` at `sms_handler.h` lines 101-102; `SmsHandler::consecutiveFailures()`
at line 92; `SmsHandler::concatKeyCount()` at line 95. The `main.cpp` StatusFn
closure uses all of these correctly. `#include <esp_system.h>` is present at
`main.cpp` line 26, covering `esp_reset_reason()`.

**NON-BLOCKING — Heap allocation of `TelegramPoller` is null-safe.** The
`new TelegramPoller(...)` at `main.cpp` line 316 is inside `setup()`, after
the early-return guard at line 302-306 (`setupTelegramClient` failure). The
`loop()` call at line 508 is guarded by `if (telegramPoller)`. The StatusFn
lambda itself checks `if (telegramPoller)` at line 410 before dereferencing
it for `pollAttempts()` / `lastUpdateId()`. No null-deref risk. However,
`new` on an ESP32 will abort on allocation failure rather than returning
null (the default `new` handler calls `abort()`), so the null check in the
lambda is defensive but not protective against OOM — this is acceptable for a
process-lifetime singleton on a device with ~150 KB free heap.

**NON-BLOCKING — `String` return from `StatusFn` is correct, no dangling
reference.** `telegram_poller.cpp` line 85 calls `bot_.sendMessage(statusFn_())`
— the temporary `String` returned by `statusFn_()` is a value that lives for
the duration of the full-expression. `sendMessage` takes `const String &`, which
binds to the temporary. The temporary is destroyed after the call returns.
No dangling reference.

**NON-BLOCKING — New test coverage is adequate but misses one edge case.** The
three new `TelegramPoller` tests cover: StatusFn called and result forwarded to
bot; nullptr fallback; help message mentions `/status`. Adequate for the happy
paths. One untested edge: `/STATUS` in uppercase. The lower-casing loop at
`telegram_poller.cpp` lines 58-65 handles this, but no test exercises it.
Low-priority omission given the explicit case-fold code.

**NON-BLOCKING — `smsFailed_` counter not incremented on LRU/TTL eviction.**
As noted in the blocking item above, eviction paths (`evictExpiredLocked`,
`evictLruUntilUnderCaps`, the per-key-cap drop at `sms_handler.cpp` line 147)
silently drop in-flight concat groups without touching `smsFailed_`. This
means the counter undercounts for the "permanently lost" interpretation. If
the "attempt-level" interpretation is adopted (option (a) above), this is not
a bug — evictions are not send attempts. Document whichever interpretation is
chosen so the status message label is accurate.

### Summary

The implementation resolves both blocking issues raised in the prior plan
review: `StatusFn` was promoted to a constructor parameter (not a post-hoc
setter), and modem AT commands are called from a 30s cache-refresh block in
`loop()` that runs before `telegramPoller->tick()`, not from inside the
Telegram HTTP response handler. The `esp_system.h` include is present; all
symbol dependencies exist; the heap-allocation null-safety pattern is
followed. One new blocking issue is introduced: the `smsFailed_` header
comment claims "permanently undeliverable" semantics but the code implements
"Telegram send attempt failures," and the counter never fires on LRU/TTL
evictions. This is a documentation bug that will mislead users reading the
status output — fix by either relabeling the counter or correcting the
implementation. All other issues are non-blocking cosmetic or coverage gaps.
