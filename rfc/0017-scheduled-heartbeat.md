---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0017: Scheduled heartbeat for silent-failure detection

## Motivation

The bridge runs 24/7 and unattended. Today there is no proactive signal that it
is alive. The user learns of a failure only when they notice an expected SMS
never arrived — potentially hours after the device died.

Existing recovery mechanisms do not close this gap:

- The hardware watchdog (RFC-0015) reboots the device after a 120 s hang but
  does not send any Telegram notification after the reboot.
- The `/status` command requires the user to actively query the bot.
- The boot banner (`"🚀 Modem SMS to Telegram Bridge is now online!"`) fires on
  restart, so a reboot-loop scenario would generate bursts of banner messages,
  but a clean silent failure (WiFi dropout that holds indefinitely without
  triggering the watchdog, or a silently stuck TLS state that is not caught by
  the consecutive-failure counter) produces no notification at all.

A periodic heartbeat message — the same output as `/status`, sent automatically
on a configurable schedule — gives the user a simple liveness signal. If the
heartbeat stops arriving, the device needs attention. If it arrives with
degraded signal or error counts climbing, the user can act before an outage.

## Current state

All the ingredients exist:

- `IBotClient::sendMessage(text)` in `src/ibot_client.h` sends a Telegram
  message and returns bool. Used by `SmsHandler`, `CallHandler`, and the boot
  banner in `setup()`.
- The `StatusFn` lambda in `main.cpp` (wired into `TelegramPoller` as its 7th
  constructor parameter per RFC-0010) already assembles the full status string
  from all in-scope objects: uptime, CSQ, registration, heap, SMS counters,
  reply-target occupancy, etc. It is a `std::function<String()>` that returns
  a `String` with no AT-command side effects (it reads cached values, not live
  modem registers).
- The `loop()` already contains several periodic tasks driven by the
  `millis() - lastXxxMs > interval` idiom (30 s CSQ/registration cache
  refresh, 30 s transport-health check). Adding a heartbeat timer follows the
  exact same pattern.
- `millis()` wraps after 49.7 days. The existing periodic blocks all use the
  subtraction idiom `(uint32_t)(millis() - lastMs) >= interval` which handles
  wraparound correctly because the subtraction is performed in unsigned 32-bit
  arithmetic — the result is always the elapsed time modulo 2^32, regardless of
  whether `millis()` has wrapped.

No new source file is needed. The change is ~15 lines in `main.cpp`.

## Plan

### 1. Compile-time configuration

Add a define with a default of 21600 seconds (6 hours):

```cpp
// src/secrets.h (or via -DHEARTBEAT_INTERVAL_SEC=... in platformio.ini)
#ifndef HEARTBEAT_INTERVAL_SEC
#  define HEARTBEAT_INTERVAL_SEC 21600   // 6 hours; 0 = disabled
#endif
```

The default can be overridden per-environment in `platformio.ini`:

```ini
[env:T-A7670X]
build_flags = ${esp32dev_base.build_flags}
    -DHEARTBEAT_INTERVAL_SEC=21600
```

Or in `secrets.h` for a user-specific override that does not touch
`platformio.ini`:

```cpp
#define HEARTBEAT_INTERVAL_SEC 3600   // 1 hour
```

The value 0 explicitly disables heartbeats — no timer variable is declared and
no `sendMessage` call is ever issued. The `#if HEARTBEAT_INTERVAL_SEC != 0`
compile guard (see section 2) handles this cleanly.

### 2. Minimum-interval guard

Very short intervals would spam the admin chat. Add a hard floor of 300 seconds
(5 minutes) enforced at compile time:

```cpp
#if HEARTBEAT_INTERVAL_SEC != 0 && HEARTBEAT_INTERVAL_SEC < 300
#  error "HEARTBEAT_INTERVAL_SEC must be 0 (disabled) or >= 300 (5 minutes). \
Set a longer interval or define HEARTBEAT_INTERVAL_SEC=0 to disable."
#endif
```

Place this check immediately below the default-define block, before any other
use of the macro. The `!= 0` exemption makes the disabled case
(`HEARTBEAT_INTERVAL_SEC=0`) pass through cleanly.

### 3. Timer variable and first-heartbeat policy

Declare the timer variable inside a `#if` block in `main.cpp`, at file scope
alongside the other static variables:

```cpp
#if HEARTBEAT_INTERVAL_SEC != 0
static uint32_t lastHeartbeatMs = 0;   // RFC-0017
#endif
```

Initialising `lastHeartbeatMs` to `0` means the first heartbeat fires
`HEARTBEAT_INTERVAL_SEC` seconds after boot, **not immediately**. This is
intentional: the boot banner (`"🚀 Modem SMS to Telegram Bridge is now
online!"`) already acts as a "device just started" notification. Sending a
second full status message 1–2 seconds later would be redundant and slightly
alarming. The first heartbeat arrives after a full quiet interval, confirming
the device has been running stably.

`lastHeartbeatMs` is not persisted to NVS. On reboot it resets to 0 and the
first heartbeat fires after a full interval. This is correct: the boot banner
already tells the user the device restarted; waiting one full interval before
the first heartbeat is fine.

### 4. Heartbeat dispatch in `loop()`

Add the following block in `loop()`, immediately after `smsSender.drainQueue()`
and before the transport-health check. This placement ensures:

- The URC drain and `callHandler.tick()` have already run, so no AT-command
  collision.
- The CSQ/registration cache has been refreshed this loop iteration if it was
  due, so the heartbeat message reflects up-to-date signal information.
- The TelegramPoller tick has completed, so the TLS connection established
  during the poll is available for the heartbeat `sendMessage` call if the
  keep-alive is still live (both use `RealBotClient` which holds the shared
  `WiFiClientSecure`).

```cpp
#if HEARTBEAT_INTERVAL_SEC != 0
    // Periodic heartbeat (RFC-0017). Reuses StatusFn output — same content
    // as the /status command. Fires once per HEARTBEAT_INTERVAL_SEC starting
    // after the first full interval post-boot (boot banner already covers
    // the "just started" notification). Uses unsigned subtraction so
    // millis() wraparound at 49.7 days is handled correctly.
    if ((uint32_t)(millis() - lastHeartbeatMs) >=
        (uint32_t)HEARTBEAT_INTERVAL_SEC * 1000u)
    {
        lastHeartbeatMs = (uint32_t)millis();
        if (statusFn)
        {
            bool sent = realBot.sendMessage(statusFn());
            Serial.printf("[heartbeat] status message %s\n",
                          sent ? "sent" : "failed (will retry next interval)");
        }
    }
#endif
```

`statusFn` is the `std::function<String()>` built in `setup()` and passed to
`TelegramPoller`. Since it is declared in `setup()` and `loop()` shares the
same translation unit (`main.cpp`), it must be lifted to file scope so the
heartbeat block can reference it. In the current code it is a local variable
inside `setup()`. Promote it to a `static std::function<String()>` at file
scope (same lifetime as all other statics in `main.cpp`), then assign it in
`setup()` before passing it to `TelegramPoller`.

If `realBot.sendMessage` returns false (transport failure during the heartbeat
window), the block logs the skip and advances `lastHeartbeatMs` anyway. This
means the retry does not happen after a short back-off; the next attempt is
exactly one full interval later. This is acceptable: a missed heartbeat during
a connectivity failure is expected — the user will notice the heartbeat is
late, which is itself a signal. No retry queue is needed; the next scheduled
heartbeat will either succeed (confirming recovery) or also fail (confirming
the outage continues). Adding a retry queue would complicate the design
significantly for negligible benefit given that heartbeats are purely
informational.

### 5. Message format

The heartbeat message is the exact string returned by `statusFn()`. No prefix
or suffix is added — the `/status` output already contains all relevant
information (uptime, signal, SMS counters, etc.). A user who sees an
unprompted status message in the chat will understand it is a scheduled
heartbeat after the first occurrence; the uptime field makes it self-evident
that the message was automatic rather than a response to a user command.

If a visual distinction between manual `/status` replies and scheduled
heartbeats is desired in a future iteration, a single `"⏱ Scheduled heartbeat"` header line could be prepended. That is a cosmetic change and out of scope here.

### 6. Interaction with the millis() wraparound

The subtraction `(uint32_t)(millis() - lastHeartbeatMs)` is evaluated in
unsigned 32-bit arithmetic. When `millis()` wraps at 2^32 − 1 (after ~49.7
days of uptime), the difference is still the correct elapsed time in
milliseconds, because `(uint32_t)(small - large)` wraps correctly in two's
complement unsigned arithmetic. For example: if `lastHeartbeatMs = 0xFFFFF000`
and `millis() = 0x00000FFF`, the difference is `0x00000FFF - 0xFFFFF000 =
0x00001FFF` (8191 ms) — the correct elapsed time. The same pattern is already
used by the existing 30 s periodic blocks in `loop()` and by `CallHandler`.

The maximum expressible interval before a spurious fire is 2^32 ms ≈ 49.7
days. `HEARTBEAT_INTERVAL_SEC` values up to 49.7 × 86400 = 4,294,967 s
(~49.7 days) are representable. The default (21600 s) and any practical
user-configured value are well within this range.

### 7. `statusFn` promotion to file scope

Currently (post-RFC-0010 implementation) the `StatusFn` lambda is constructed
as a local variable inside `setup()` and passed as a constructor argument to
`TelegramPoller`. To make it accessible in `loop()` for the heartbeat block,
promote it to a file-scoped static:

```cpp
// main.cpp, file scope (alongside other static objects)
static std::function<String()> statusFn;   // set in setup(), read in loop()
```

In `setup()`, assign it before passing to `TelegramPoller`:

```cpp
statusFn = [&]() -> String {
    // ... existing StatusFn body unchanged ...
};
new TelegramPoller(realBot, smsSender, replyTargets, persist,
                   []() -> uint32_t { return (uint32_t)millis(); },
                   authFn,
                   statusFn);   // 7th parameter
```

The lambda captures `smsHandler`, `replyTargets`, `modem`, etc. by reference.
All captured objects are file-scope statics with process lifetime — the same
safety property noted in RFC-0010's "Notes for handover". Promoting `statusFn`
to file scope does not change the capture semantics.

### 8. Scope of changes

Files touched:

- **`src/main.cpp`** only:
  - Add `#ifndef HEARTBEAT_INTERVAL_SEC` default-define block.
  - Add `#if HEARTBEAT_INTERVAL_SEC != 0 && HEARTBEAT_INTERVAL_SEC < 300`
    compile-time guard.
  - Promote `statusFn` from local variable in `setup()` to file-scope static.
  - Add `#if HEARTBEAT_INTERVAL_SEC != 0` timer variable declaration.
  - Add the ~10-line heartbeat block in `loop()`.

No changes to `TelegramPoller`, `IBotClient`, `RealBotClient`, `SmsHandler`,
`IModem`, `IPersist`, any test file, or `platformio.ini`.

Estimated new lines of code: ~15 (excluding blank lines and comments).

### 9. Test approach

The heartbeat is pure `main.cpp` timer logic with no new class, no new
interface, and no hardware interaction beyond the existing `sendMessage` call.
`main.cpp` is excluded from the native test build (`build_src_filter` in
`[env:native]`) and cannot be exercised by host-native Unity tests.

No host tests are added. The expected behavior is documented here:

- At `t = 0` (boot): `lastHeartbeatMs = 0`. The interval is not yet elapsed;
  no heartbeat fires. The boot banner is sent instead.
- At `t = HEARTBEAT_INTERVAL_SEC × 1000 ms`: the subtraction condition fires.
  `realBot.sendMessage(statusFn())` is called. `lastHeartbeatMs` is set to
  the current `millis()` value regardless of the return value of `sendMessage`.
- At `t = 2 × HEARTBEAT_INTERVAL_SEC × 1000 ms`: second heartbeat fires.
- If `sendMessage` returns false at `t = N × interval`: the miss is logged.
  The next check fires at `t ≈ (N+1) × interval` — no retry, no back-off.
- At millis() wraparound (~49.7 days): the unsigned subtraction correctly
  computes the elapsed time; no spurious heartbeat fires.
- Manual verification on hardware: set `HEARTBEAT_INTERVAL_SEC=300` (5 min),
  flash, wait 5 min, verify a status message arrives in the Telegram chat.
  Verify uptime in the message is ~300 s. Disable WiFi for 5 min, verify the
  serial log shows `"failed (will retry next interval)"`. Re-enable WiFi, wait
  another 5 min, verify the heartbeat resumes.

## Notes for handover

- **`statusFn` promotion is the only structural change.** Everything else is
  additive. If the reviewer prefers to avoid touching the `TelegramPoller`
  construction call, an alternative is to store a reference or pointer to the
  `TelegramPoller`'s internal `statusFn_` member — but that breaks
  encapsulation. The cleanest path is the file-scope promotion described in
  section 7.

- **Do not call `statusFn()` before it is assigned.** `statusFn` is a
  `std::function<String()>` default-constructed to `nullptr`. The heartbeat
  block guards with `if (statusFn)` before calling it. `setup()` assigns
  `statusFn` before arming the watchdog and returning, so by the time
  `loop()` runs, `statusFn` is always valid. The `if (statusFn)` guard is
  defensive belt-and-suspenders.

- **`sendMessage` is not reentrant with itself.** The heartbeat block calls
  `realBot.sendMessage` from `loop()`, which is the same call path used by
  `callHandler.tick()` (for call notifications), the reboot logic in
  `SmsHandler`, and the `TelegramPoller` error replies. All of these are
  dispatched sequentially in a single-threaded `loop()`, so there is no
  concurrency hazard. The only collision scenario is if an SMS arrives and
  triggers a `sendMessage` inside `handleSmsIndex` in the same loop iteration
  that also fires the heartbeat. Since `handleSmsIndex` runs in the URC drain
  block (before the heartbeat block), both calls are sequential; the second
  call reuses the keep-alive connection established by the first.

- **Interval granularity is one `loop()` iteration (~50 ms).** The heartbeat
  fires on the first `loop()` iteration where the elapsed time exceeds the
  interval. The jitter is bounded by the maximum blocking time within a single
  loop iteration (~120 s in the worst case with a 10-part PDU send). In
  practice, jitter is 50–500 ms. This is inconsequential for a 6-hour
  heartbeat.

- **`HEARTBEAT_INTERVAL_SEC` is not exposed via `/status`.** The status message
  does not show the configured heartbeat interval. This is intentional — the
  status message is already information-dense and the heartbeat interval is a
  compile-time constant visible in `secrets.h` / `platformio.ini`. If the user
  forgets the configured interval, they can check the source; displaying it in
  every status message adds noise for little gain.

- **NVS wear.** `lastHeartbeatMs` is not persisted. On a 6-hour interval the
  first post-reboot heartbeat is delayed by up to 6 hours compared to a
  persisted design. This is acceptable: reboots are already announced by the
  boot banner. Persisting the last-heartbeat timestamp would add an NVS write
  once every 6 hours — negligible wear, but also negligible benefit.

- **The 300 s minimum is enforced at compile time, not runtime.** A
  `HEARTBEAT_INTERVAL_SEC=60` in `secrets.h` will produce a build error, not
  a runtime warning. This is intentional: a runtime guard would require
  clamping or silent override, both of which hide misconfiguration. The hard
  `#error` forces the developer to make a deliberate choice.

- **Interaction with RFC-0015 (hardware watchdog).** The heartbeat block
  appears in `loop()` after `esp_task_wdt_reset()` is called at the top of the
  function. The heartbeat's `sendMessage` call issues an HTTPS request, which
  may take 1–10 s. This is well within the 120 s watchdog window and does not
  require an additional `esp_task_wdt_reset()` inside the heartbeat block.

## Review

**verdict: approved-with-changes**

- NON-BLOCKING — **`statusFn` is currently an anonymous lambda, not a named local.** Section 7's code snippet shows `statusFn = [&]() -> String { ... }; new TelegramPoller(..., statusFn)` but in the current `main.cpp` the lambda is written inline as the 7th argument to `telegramPoller = new TelegramPoller(...)` (lines 454–569). The implementer must extract that entire inline lambda body, assign it to the new file-scope `static std::function<String()> statusFn`, and replace the constructor argument with the named variable. The RFC describes this correctly in prose (section 7) but the code snippet omits the `telegramPoller =` assignment on the `new TelegramPoller` line — a cosmetic inconsistency that could confuse a reader skimming the diff. Fix the snippet to match the real call site.

- NON-BLOCKING — **WDT budget accounting is optimistic.** The RFC states the heartbeat `sendMessage` "may take 1–10 s" and is "well within the 120 s watchdog window." That is true in isolation, but the heartbeat fires in the same `loop()` iteration that may also run `telegramPoller->tick()` (another HTTPS round-trip, up to ~10 s) and `smsSender.drainQueue()` (up to 10 PDU sends, each with an AT round-trip). In a worst-case degraded-connectivity scenario the cumulative blocking time in a single iteration could approach or exceed 120 s, triggering the WDT before the top-of-loop `esp_task_wdt_reset()` runs again. This is a pre-existing risk not introduced by this RFC, but the "no additional wdt_reset needed" claim in the Notes is only strictly true when the heartbeat fires alone. Consider either noting this caveat explicitly, or adding `esp_task_wdt_reset()` just before the `sendMessage` call as a belt-and-suspenders measure consistent with the pattern already used in `syncTime()`.

- NON-BLOCKING — **Message format gives no visual distinction from a manual `/status` reply.** Section 5 argues the uptime field makes the automated nature "self-evident," but in practice a user who receives an unprompted status block at an unexpected time may briefly think their `/status` command was answered late, or that someone else triggered it. The RFC already identifies the fix ("a single `⏱ Scheduled heartbeat` header line") and defers it as cosmetic — that deferral is acceptable, but the implementer should be aware that first-time users will likely find the distinction useful and this is low-effort to add now rather than in a follow-up.

- NON-BLOCKING — **The `#if HEARTBEAT_INTERVAL_SEC != 0` guard on the loop block is correct, but the `lastStatusRefreshMs` and `lastTransportCheck` timer blocks in the existing `loop()` use `millis() - lastXxxMs > intervalUL` with a plain `>` (not `>=`), while the RFC uses `>=`.** Both are correct for practical purposes (the difference is one millisecond), but using `>=` is the stricter and more correct form for "fire at least once per interval." No change required — noting for consistency awareness.

- NON-BLOCKING — **`HEARTBEAT_INTERVAL_SEC` default-define placement.** The RFC proposes adding the `#ifndef` default-define block and the `#error` guard in `src/main.cpp`. This is a reasonable choice given that all other compile-time constants for this project live in `secrets.h` or `platformio.ini`. Keeping it in `main.cpp` is fine, but consider placing the default-define in `secrets.h.example` as a commented-out example so new users know the knob exists. No code change required for this RFC — a follow-up to `secrets.h.example` is sufficient.

## Code Review

Reviewer: claude-sonnet-4-6, 2026-04-09. Checked against the ten questions in the review brief and against the prior `## Review` section's four open items.

**Verdict: approved — one NON-BLOCKING issue, all prior Review items resolved.**

---

### Checklist

**1. `HEARTBEAT_INTERVAL_SEC` define order — PASS.**
`main.cpp` lines 55–57: `#ifndef HEARTBEAT_INTERVAL_SEC` / `#define HEARTBEAT_INTERVAL_SEC 21600` appears first. The `#error` guard at lines 58–60 comes immediately after. Both precede any use of the macro. Correct.

**2. `lastHeartbeatMs` declaration scope — PASS.**
Declared at file scope (lines 149–151) under `#if HEARTBEAT_INTERVAL_SEC != 0`. The heartbeat block in `loop()` (lines 742–756) is wrapped in the identical `#if HEARTBEAT_INTERVAL_SEC != 0` guard. The variable is always in scope where it is used. Correct.

**3. `statusFn` lambda capture safety — PASS.**
The lambda body (lines 479–580) references: `millis()` (global function), `WiFi.RSSI()` (global object), `cachedCsq`, `cachedRegStatus`, `smsHandler`, `replyTargets`, `telegramPoller`, `smsDebugLog` (all file-scope statics with process lifetime), and `ESP.getFreeHeap()` / `esp_reset_reason()` / `ReplyTargetMap::kSlotCount` (globals / static member constants). No `setup()`-local variables are captured. The implicit capture list is `[]` (no capture clause at all — not `[&]` or `[=]`), so only names that are accessible at file scope can be referenced: this is correct and safe. Compiles cleanly.

**4. Heartbeat placement in `loop()` — PASS.**
Order in `loop()`:
1. `esp_task_wdt_reset()` — top of loop
2. URC drain (`while (SerialAT.available())`)
3. `callHandler.tick()`
4. CSQ/reg cache refresh (30 s timer)
5. `telegramPoller->tick()`
6. `smsSender.drainQueue()`
7. **Heartbeat block** (RFC-0017)
8. Transport-health check (30 s timer)

This matches the RFC's specified placement: after `drainQueue()`, before the transport-health check. Correct.

**5. `esp_task_wdt_reset()` position relative to `sendMessage` — PASS with observation.**
`esp_task_wdt_reset()` is at line 750, which is inside the `if (statusFn)` guard and before the `statusFn()` call and `realBot.sendMessage(msg)` call at lines 751–752. This correctly pets the watchdog immediately before the blocking HTTPS request.

Observation (NON-BLOCKING): the WDT reset is nested inside `if (statusFn)`. If `statusFn` were null (impossible in practice — it is always assigned in `setup()` before the watchdog is armed), the WDT reset would be skipped for this block. Since `lastHeartbeatMs` was already advanced unconditionally at line 747, the block would not re-trigger; the next top-of-loop WDT reset arrives within one loop iteration (~50 ms). No real-world risk.

**6. `lastHeartbeatMs` advances regardless of `sendMessage` result — PASS with observation.**
Line 747 (`lastHeartbeatMs = nowMs;`) executes unconditionally at the top of the `if` body, before `sendMessage` is called. The return value of `realBot.sendMessage(msg)` at line 752 is discarded with no assignment and no log output.

Observation (NON-BLOCKING): the RFC's plan (section 4 code snippet) showed capturing the return value as `bool sent` and logging `"[heartbeat] status message sent"` or `"failed (will retry next interval)"`. The RFC's Notes for Handover also states: "If `realBot.sendMessage` returns false … the block **logs** the skip." The implementation silently drops the result. Missed heartbeats are therefore invisible in the serial log unless the caller catches the WiFi-drop message from the transport-health block on the same iteration. Consider restoring the `Serial.printf` line from the RFC's plan snippet; the fix is one line and makes the log actionable when debugging connectivity issues.

**7. Overflow arithmetic — PASS.**
Expression: `(uint32_t)(nowMs - lastHeartbeatMs) >= (uint32_t)HEARTBEAT_INTERVAL_SEC * 1000u`

- `nowMs` and `lastHeartbeatMs` are both `uint32_t`; the subtraction is unsigned 32-bit and wraps correctly at the 49.7-day rollover.
- `(uint32_t)HEARTBEAT_INTERVAL_SEC` casts the `int` macro value to `uint32_t` before multiplying, so the multiplication `21600u * 1000u = 21,600,000u` is entirely in unsigned arithmetic and is well within the `uint32_t` range (~4.29 billion). No signed overflow before the multiply. Correct.

**8. `statusFn` null-checked — PASS.**
Line 748: `if (statusFn)` guards the entire send path. Correct.

**9. `TelegramPoller` constructor after `statusFn` promotion — PASS.**
Line 582–596: `telegramPoller = new TelegramPoller(..., statusFn)` passes the now-named file-scope variable as the 7th argument. The assignment of `statusFn` at line 479 precedes the constructor call. Correct.

**10. Native test build compatibility — PASS.**
`main.cpp` is excluded from the native test build via `build_src_filter` in `[env:native]`. The file-scope `statusFn` and `lastHeartbeatMs` are in `main.cpp` and are never seen by the host compiler. No impact on native tests.

---

### Prior `## Review` item disposition

| Prior item | Status |
|---|---|
| NON-BLOCKING — `statusFn` was anonymous lambda, not named local | **Resolved.** `statusFn` is now a file-scope `static std::function<String()>`, assigned in `setup()` before `TelegramPoller` construction. |
| NON-BLOCKING — WDT budget accounting; suggested adding `esp_task_wdt_reset()` before `sendMessage` | **Resolved.** `esp_task_wdt_reset()` added at line 750, immediately before the blocking `sendMessage` call. |
| NON-BLOCKING — No visual distinction between heartbeat and manual `/status` reply | **Resolved.** Implementation prepends `"⏱ Heartbeat\n"` to the status string (line 751), giving a clear visual distinction. |
| NON-BLOCKING — `>=` vs `>` consistency with existing timers | **Noted, not changed.** The heartbeat block correctly uses `>=`; existing timers use `>`. Acceptable. |
| NON-BLOCKING — `secrets.h.example` should document the knob | **Resolved.** `secrets.h.example` lines 50–51 add the commented-out `#define HEARTBEAT_INTERVAL_SEC 21600` example. |

---

### Open item from this review

- NON-BLOCKING — **Failed heartbeats are not logged.** `realBot.sendMessage()` return value is silently discarded at line 752. The RFC's plan section 4 and Notes for Handover both state the failure should be logged to the serial monitor. Restoring `Serial.printf("[heartbeat] status message %s\n", sent ? "sent" : "failed (will retry next interval)")` is a one-line fix that makes the log useful when diagnosing connectivity problems.
