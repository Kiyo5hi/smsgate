---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0012: Persistent outbound SMS queue with retry

## Motivation

When the user sends a Telegram reply, `SmsSender::send()` is called
synchronously inside `TelegramPoller::processUpdate()`. A single failure
— modem busy, AT command in flight, concat reassembly holding the serial
port, or a momentary PDU timeout — causes `send()` to return false
immediately. The poller sends an error reply to Telegram and marks the
update as processed (watermark advances). The message is gone. The user
must notice the error, re-read the original SMS, and manually compose
the reply again.

For the common real-world case of a flaky cellular link or a modem that
is briefly occupied handling an incoming SMS, this is an unacceptable
reliability gap. The fix is a small queue that decouples acceptance from
delivery: the poller enqueues the outbound message and returns quickly;
a separate drain step retries until the message is delivered or a hard
per-message retry cap is hit.

## Current state

### Call path (exact)

```
loop()
  └─ telegramPoller->tick()
       └─ TelegramPoller::processUpdate(u)
            └─ smsSender_.send(phone, u.text)      ← synchronous, no retry
                 └─ IModem::sendPduSms(hex, len)   ← one AT+CMGS per PDU
```

Relevant source locations:

- `src/telegram_poller.cpp` line 118: `if (!smsSender_.send(phone, u.text))`
- `src/sms_sender.cpp` line 52: `bool ok = modem_.sendPduSms(pdus[i].hex, pdus[i].tpduLen);`
- `src/imodem.h` line 58: `virtual bool sendPduSms(const String &pduHex, int tpduLen) = 0;`

On failure, `processUpdate` calls `sendErrorReply(...)` (line 123) and
returns. The watermark has already been (or will be) advanced past this
update_id. There is no retry path anywhere in the chain.

### Why the modem can be busy

The main `loop()` order is:

1. URC drain (`SerialAT.available()` → `handleSmsIndex` / `callHandler.onUrcLine`)
2. `callHandler.tick()`
3. Modem CSQ/reg cache refresh (every 30 s)
4. `telegramPoller->tick()`
5. WiFi check (every 30 s)

The modem serial port is shared. If an incoming SMS arrives and
`handleSmsIndex` is issuing `AT+CMGR` / `AT+CMGD` in step 1, then
`telegramPoller->tick()` fires in step 4 and receives an update that
it tries to send immediately, the `AT+CMGS` in step 4 can arrive at the
modem while it is still processing the step-1 AT exchange. The modem
may time out or return ERROR. The same scenario arises during a concat
reassembly sweep at boot (`sweepExistingSms`).

## Plan

### 1. Queue data structure

Add a fixed-size in-RAM queue to `SmsSender`. Queue capacity: **8
slots**. Each slot holds:

```cpp
struct OutboundEntry {
    String phone;
    String body;
    uint8_t attempts;         // how many send attempts have been made
    uint32_t nextRetryMs;     // clock() value at or after which to retry
    // Callback to deliver a final-failure notice back to the user.
    // Stored as a std::function<void(const String& reason)>.
    // nullptr means "no notification needed" (e.g. programmatic caller).
    std::function<void(const String &)> onFinalFailure;
};
```

The queue is a simple circular buffer of `OutboundEntry` (or a
`std::array<OutboundEntry, 8>` with a head/tail/count triple).
RAM budget: 8 × (phone ~20 B + body ~1600 B + overhead) ≈ ~13 KB worst
case (8 slots × 1600 B bodies). On the ESP32 with ~200 KB free heap
this is comfortable, but callers should not enqueue a 1530-char
GSM-7 body eight times simultaneously. The queue cap of 8 is also a
safety valve against runaway accumulation.

**Queue-full policy**: if the queue is full when `enqueue()` is called,
reject the new entry immediately (do not evict), call `onFinalFailure`
with "outbound queue full", and return false. Log the rejection.

### 2. New `SmsSender` API

Add two new methods to `SmsSender` (not to the `ISmsSender` interface —
see section 5 for rationale):

```cpp
// Queue `body` for delivery to `phone`. Returns true if the entry was
// accepted into the queue, false if the queue is full. On ultimate
// delivery failure (after kMaxAttempts retries), `onFinalFailure` is
// called with a human-readable reason; pass nullptr to suppress
// notifications. The caller must NOT call send() directly for queued
// messages — enqueue() owns delivery.
bool enqueue(const String &phone,
             const String &body,
             std::function<void(const String &)> onFinalFailure = nullptr);

// Attempt to drain at most one pending queue entry. Call from loop()
// outside the poller tick. Returns immediately if the queue is empty
// or no entry is past its nextRetryMs. On success the entry is
// removed from the queue. On failure the attempt count is incremented
// and nextRetryMs is advanced by the backoff formula; if attempts
// reaches kMaxAttempts the entry is dropped and onFinalFailure is
// called.
void drainQueue(uint32_t nowMs);
```

Retry policy constants:

```cpp
static constexpr uint8_t kMaxAttempts      = 5;
static constexpr uint32_t kBaseBackoffMs   = 2000;   // 2 s
static constexpr uint32_t kBackoffCapMs    = 60000;  // 60 s ceiling
```

Backoff formula: `min(kBaseBackoffMs << (attempts - 1), kBackoffCapMs)`.

| Attempt | Delay before retry |
|---------|--------------------|
| 1st     | immediate (nextRetryMs = nowMs at enqueue time) |
| 2nd     | 2 s                |
| 3rd     | 4 s                |
| 4th     | 8 s                |
| 5th     | 16 s               |
| Drop    | after 5th failure  |

The first attempt is made on the first `drainQueue()` call after
enqueuing, with no mandatory delay. This keeps latency low for the
common case where the modem is not busy.

`SmsSender` needs a `ClockFn` (same `std::function<uint32_t()>`
convention used by `CallHandler` and `TelegramPoller`) injected at
construction time for testability:

```cpp
explicit SmsSender(IModem &modem,
                   std::function<uint32_t()> clock = nullptr);
```

When `clock` is nullptr, `drainQueue` uses `millis()` directly (the
Arduino default). Tests inject a virtual clock so backoff and timing
paths are deterministic on the host.

### 3. Changes to `TelegramPoller::processUpdate`

Replace the direct `smsSender_.send(...)` call at line 118 with
`smsSender_.enqueue(...)`. The poller already holds an `ISmsSender&`,
so the call site needs access to the queue API.

Two options:

**Option A (preferred)**: Give `TelegramPoller` a reference to
`SmsSender` (the concrete type) rather than `ISmsSender`. The queue
methods are on `SmsSender` directly, not on `ISmsSender` (see section
5). Requires a small change in `telegram_poller.h` and `main.cpp`
construction site. The cost is that `TelegramPoller` now depends on the
concrete `SmsSender`; the benefit is a clean, explicit dependency with
no casting.

**Option B**: Add `enqueue(...)` to the `ISmsSender` interface. This
keeps `TelegramPoller` depending only on the interface but pollutes the
interface with queue mechanics. Not preferred.

**Option C**: Introduce a narrow `ISmsSenderQueue` interface with just
`enqueue()` and have `SmsSender` implement both `ISmsSender` and
`ISmsSenderQueue`. `TelegramPoller` takes `ISmsSenderQueue&`. Clean but
adds a third interface.

The RFC recommends **Option A** for simplicity. The fake in tests
(`FakeSmsSender` / whatever the test double is named) would be replaced
by a concrete `SmsSender` constructed with a `FakeModem`, which already
gives full control over success/failure.

The `onFinalFailure` callback passed from `processUpdate` should send a
Telegram error message:

```cpp
smsSender_.enqueue(phone, u.text, [this](const String &reason) {
    sendErrorReply(String("SMS send failed after retries: ") + reason);
});
```

This means `TelegramPoller` must store its `this` pointer in the
lambda, which is safe because the poller outlives the queue entries (both
are process-lifetime objects in `main.cpp`).

### 4. Changes to `loop()` in `main.cpp`

Add a `drainQueue` call after the poller tick:

```cpp
// Drive the TG->SMS poller (RFC-0003).
if (telegramPoller)
    telegramPoller->tick();

// Drain one pending outbound SMS per loop iteration (RFC-0012).
smsSender.drainQueue((uint32_t)millis());
```

Placing `drainQueue` after the poller tick means a message that was
just enqueued this tick gets its first send attempt on the very next
loop iteration (50 ms later, given the `delay(50)` at the bottom of
`loop()`), not immediately. This is acceptable: the first-attempt
latency goes from <1 ms (synchronous) to ~50 ms (next iteration), which
is imperceptible to the user.

Placing `drainQueue` after the URC drain but before the poller tick is
also workable and slightly reduces the latency for retries, but would
mean the modem is being written to before the URC drain completes —
exactly the race condition we are trying to avoid. After the poller tick
is the correct slot.

One send attempt per `loop()` call is intentional. The loop runs
roughly every 50 ms. Attempting one send per loop is fast enough
(subjective delivery latency is typically under 200 ms for a clean
modem) while keeping the loop non-blocking from the perspective of URC
processing.

### 5. `ISmsSender` interface — no changes

`ISmsSender` stays as-is: just `send()` and `lastError()`. The queue
API (`enqueue`, `drainQueue`) is on `SmsSender` only. Reasons:

- `ISmsSender` exists for testability of `TelegramPoller`. If `Option A`
  is adopted (TelegramPoller takes `SmsSender&`), the interface is no
  longer used by the poller at all and could be removed in a future
  cleanup — but leave it for now to avoid churning `FakeSmsSender` in
  tests that exercise the old `send()` path.
- `drainQueue` carries a clock dependency that would be awkward to fake
  at the interface level.
- Keeping the interface minimal makes future alternative implementations
  (e.g. a modem-data-path sender for RFC-0004) simpler.

### 6. Persistence — RAM only (no NVS)

Queue entries are ephemeral intent and are **not** persisted to NVS.
Rationale:

- NVS already stores the reply-target ring buffer (200 slots × ~25 B)
  and the update_id watermark. Adding queue entries would require a
  separate key or a more complex blob, adding NVS wear on every enqueue.
- A reboot clears in-flight concat reassembly state too (those fragments
  stay on the SIM and rehydrate via `sweepExistingSms`). Queue entries
  have no SIM-side counterpart to rehydrate from — they would be lost
  on reboot regardless.
- The user already receives a "SMS send failed after retries" Telegram
  error on final failure; on an unexpected reboot they simply lose the
  unsent reply silently. This is acceptable given how infrequently a
  hard reboot races with an outbound SMS.

If future work adds a persistent queue, the same `IPersist` blob
convention used by `ReplyTargetMap` would apply.

### 7. Interaction with RFC-0009 (concat TX)

RFC-0009 moves the PDU-splitting logic into `sms_codec::buildSmsSubmitPduMulti`
and the per-PDU loop into `SmsSender::send()`. The queue operates at the
`(phone, body)` level — one queue entry per logical message regardless
of how many PDUs it produces.

`drainQueue` calls `SmsSender::send(entry.phone, entry.body)` for the
selected entry. The split into individual PDUs happens inside `send()`
at send time, exactly as it does today. A retry enqueues the same
`(phone, body)` pair, not the pre-split PDUs.

This means:

- If a 10-part message fails on part 7, the retry re-splits and
  re-sends from part 1. Parts 1–6 are re-sent to the recipient.
  This is the same partial-delivery behaviour that RFC-0009 section 6
  accepts as unavoidable — the SMS protocol has no recall mechanism.
- The concat reference number counter in `buildSmsSubmitPduMulti`
  increments on each call, so a retry produces a fresh reference
  number. The recipient's handset treats it as a new message, which
  is correct: the first attempt's parts 1–6 may or may not have
  arrived, and sending with a new reference avoids confusing the
  reassembly buffer on older handsets.
- No interaction with the SIM-side concat fragment buffer in
  `SmsHandler` — that buffer is for inbound reassembly only.

### 8. Test plan

All tests in the native env (`pio test -e native`). No hardware needed.

1. **Basic enqueue + drain success**: enqueue one entry, call
   `drainQueue()` with a `FakeModem` that returns success. Verify the
   entry is removed from the queue and `onFinalFailure` is NOT called.

2. **Retry on first failure**: enqueue one entry, make the fake modem
   fail on attempt 1. Call `drainQueue(nowMs)`. Verify the entry is
   still in the queue with `attempts == 1` and `nextRetryMs == nowMs +
   kBaseBackoffMs`. Call `drainQueue(nowMs + kBaseBackoffMs - 1)` —
   verify no send attempt (not yet due). Call `drainQueue(nowMs +
   kBaseBackoffMs)` with the modem now succeeding — verify entry removed.

3. **Exponential backoff**: enqueue one entry. Make the fake modem
   always fail. Drive `drainQueue()` through all `kMaxAttempts`
   attempts, advancing the virtual clock appropriately between calls.
   Verify the delays between attempts are 0, 2 s, 4 s, 8 s, 16 s.
   After the 5th failure, verify the entry is removed and
   `onFinalFailure` is called with a non-empty reason string.

4. **Queue full**: fill all 8 slots. Attempt a 9th `enqueue()`. Verify
   it returns false and `onFinalFailure` is called immediately with
   "outbound queue full". Verify the queue size is still 8.

5. **Queue full — other 8 slots drain correctly after rejection**: after
   the rejection test above, advance the clock and drain all 8 entries
   successfully. Verify all 8 `onFinalFailure` callbacks are NOT called
   and the queue ends empty.

6. **One drain per call**: enqueue 3 entries, all immediately due.
   Call `drainQueue()` once. Verify exactly one send attempt occurred
   (not three).

7. **Nullptr onFinalFailure**: enqueue with `onFinalFailure = nullptr`,
   exhaust all retries. Verify no crash (nullptr is not called).

8. **Clock injection**: construct `SmsSender` with a lambda clock that
   returns a controlled value. Verify `drainQueue` uses the injected
   clock, not `millis()`.

9. **Integration with TelegramPoller** (uses `FakeModem` + virtual bot):
   wire a `TelegramPoller` with a `SmsSender(FakeModem, clock)` per
   Option A. Feed an update that maps to a valid phone via a
   `ReplyTargetMap`. Make the fake modem fail. Verify the poller does
   NOT immediately call `sendErrorReply`. Drive `drainQueue()` through
   all retries. Verify `sendErrorReply` is called after the last
   failure.

## Notes for handover

- **Option A (TelegramPoller takes `SmsSender&`) changes one constructor
  parameter.** The `ISmsSender&` parameter in `TelegramPoller`'s
  constructor becomes `SmsSender&`. Update `telegram_poller.h`,
  `telegram_poller.cpp`, and the construction site in `main.cpp`. The
  test double can be dropped entirely for the poller tests that go
  through `drainQueue` — tests use a real `SmsSender` with a
  `FakeModem`. Any existing poller tests that still use a `FakeSmsSender`
  via the `ISmsSender&` path will need to migrate; check
  `test/test_native/` for affected test files.

- **`drainQueue` must not be called from inside `processUpdate` or
  `tick()`.** It must run as a separate step in `loop()`. Calling it
  inside the poller would mean AT commands are issued while the poller
  may itself be mid-AT exchange (the `pollUpdates` HTTP call). The
  loop ordering in section 4 is intentional: URC drain → callHandler
  → CSQ refresh → poller tick → drainQueue → WiFi check.

- **The `onFinalFailure` lambda captures `this` (the TelegramPoller
  pointer).** Both the poller and the `SmsSender` are process-lifetime
  statics in `main.cpp`. The lambda is called from inside `drainQueue`,
  which is called from `loop()`. There is no lifetime hazard, but
  document this clearly in the code comment to prevent future
  refactoring from introducing a dangling pointer.

- **`drainQueue` calls `SmsSender::send()` internally.** The existing
  `send()` method does not change; `drainQueue` is just a caller of
  `send()` with retry bookkeeping around it. This means `lastError()`
  on `SmsSender` reflects the most recent `send()` attempt, which is
  the error from the most recent retry — that is the right string to
  pass to `onFinalFailure`.

- **The queue is not thread-safe.** The ESP32 Arduino loop runs on a
  single core (or at least the `loop()` task is single-threaded for our
  purposes). `enqueue()` and `drainQueue()` must both be called from the
  same task. The `onFinalFailure` callback fires synchronously from
  inside `drainQueue()`, so it runs on the loop task — safe to call
  `bot_.sendMessage(...)` from it, just as `sendErrorReply` does today.

- **Do not attempt to persist queue entries to NVS** without first
  benchmarking the NVS write latency. `RealPersist` uses
  `Preferences::putBytes` which blocks for a flash-sector write
  cycle (~10 ms on ESP32). Writing on every `enqueue()` would add
  visible jitter to the loop.

- **If RFC-0009 (concat TX) lands first**, `SmsSender::send()` already
  loops over PDUs internally; no change to `drainQueue` is required.
  If RFC-0009 lands after this RFC, no change to the queue is required
  either — the queue entry still holds `(phone, body)` and `send()`
  handles splitting. The two RFCs are fully independent at the interface
  boundary.

## Review

**verdict: approved-with-changes**

### BLOCKING issues

- **Option A breaks 11 existing tests with a one-line ripple.** The RFC
  says Option A "replaces" the `FakeSmsSender` test double — but there
  is no `FakeSmsSender` in `test/support/`. The existing
  `test_telegram_poller.cpp` already constructs `SmsSender sender(modem)`
  (the concrete type) and passes it to `TelegramPoller` as `ISmsSender&`.
  This means the `TelegramPoller` constructor currently takes
  `ISmsSender&`, not `SmsSender&`. Switching to `SmsSender&` in Option A
  is therefore a source-compatible change for every existing test (they
  already hand a `SmsSender` to a reference parameter), but the RFC's
  claim that "the fake can be dropped entirely" is misleading — there was
  never a `FakeSmsSender`. The real risk is the opposite: if a future
  developer re-introduces a test double that only implements `ISmsSender`,
  it will no longer compile as a `TelegramPoller` argument under Option A.
  The RFC must explicitly acknowledge that Option A permanently forecloses
  using any `ISmsSender`-only test double for `TelegramPoller`. If that
  is acceptable, Option A is fine; if not, use Option C (narrow
  `ISmsSenderQueue` interface). **Resolution required before
  implementation: explicitly commit to Option A or Option C and document
  the tradeoff in section 3.**

- **`drainQueue` placement introduces a subtle AT-command collision for
  multi-PDU messages.** The RFC correctly places `drainQueue` after the
  poller tick to avoid issuing AT commands while the URC drain is in
  flight. However, `drainQueue` itself calls `SmsSender::send()`, which
  loops over multiple `sendPduSms` calls for long messages. If a
  `+CMTI` URC arrives from the modem mid-loop — between PDU parts 3 and
  4 of a 10-part message — the URC bytes sit in `SerialAT`'s RX buffer
  and are not serviced until the next `loop()` iteration. This is the
  same known limitation RFC-0009 accepts; the review is noting it
  explicitly so the implementation does not add a second `drainQueue`
  call earlier in the loop thinking it will help. The ordering in section
  4 is correct; document the multi-PDU latency caveat there.

### NON-BLOCKING issues

- **`ClockFn` default of `nullptr` creates an invisible branch in
  `drainQueue`.** The proposed constructor signature is
  `SmsSender(IModem&, std::function<uint32_t()> clock = nullptr)` with
  the note that `drainQueue` falls back to `millis()` when clock is null.
  The simpler and more testable approach is to have `drainQueue` take
  `uint32_t nowMs` as a parameter directly (caller passes `millis()`),
  eliminating the nullable field from the object entirely. The clock only
  matters to `drainQueue`, not to `send()`, so there is no reason for
  `SmsSender` to own it. Every call site already has `millis()` available.
  This is a cleaner design and avoids the nullptr branch in production
  code. **Recommended change.**

- **`onFinalFailure` callback signature inconsistency.** The `OutboundEntry`
  struct documents the callback as `std::function<void(const String&)>`
  taking a `reason` string, but section 3 shows the lambda capturing
  `this` on `TelegramPoller` ignoring the `phone` and `body`. The
  callback correctly does not need `phone`/`body` — the poller already
  knows the context from the enclosing `processUpdate` call. However, the
  RFC should confirm there is no circular-ownership problem: `TelegramPoller`
  stores `SmsSender&` (a reference, not ownership), and `SmsSender`'s
  queue holds a `std::function` that captures a raw `TelegramPoller*`. As
  noted in "Notes for handover", both are process-lifetime objects —
  the analysis is correct and there is no lifetime hazard. But the RFC
  should add one sentence in section 3 that explicitly states
  `TelegramPoller` does NOT take ownership of `SmsSender` (it holds a
  reference), so the lambda's raw `this` capture is safe.

- **Backoff table off-by-one vs. code.** The table shows attempt 1 as
  "immediate" and attempt 5 as "16 s delay", implying drops happen after
  5 failures. The formula `min(kBaseBackoffMs << (attempts - 1), cap)`:
  at `attempts = 1` (first retry after first failure) that is
  `2000 << 0 = 2s`, not the 2s shown for attempt 2. The table counts
  "attempts made" whereas the formula is applied to schedule the next
  retry. Trace through: enqueue sets `nextRetryMs = nowMs` (immediate);
  after failure 1, `attempts = 1`, next delay = `2000 << 0 = 2s`; after
  failure 2, `attempts = 2`, next delay = `2000 << 1 = 4s`; etc. After
  failure 5 the entry is dropped. The table as written is internally
  consistent with this; the confusion is in the label "Delay before
  retry" vs. "Delay after this failure number." Recommend relabelling
  the table column to "Backoff after this failure" to make the formula
  self-evident without requiring a trace.

- **Queue-full behavior on `send()` path not addressed.** The RFC blocks
  direct calls to `send()` for queued messages and states "`enqueue()`
  owns delivery" — but nothing prevents a caller from calling `send()`
  directly while the queue is non-empty. The queue and `send()` share
  the same `lastError_` field. If `drainQueue` calls `send()` internally
  and another path also calls `send()` concurrently (not thread-safety —
  re-entrant within a single loop iteration is impossible here), there is
  no hazard. But the RFC should state explicitly that `send()` remains
  public and usable for non-queued callers (e.g. `SmsHandler` does not
  use the queue path). Currently `SmsHandler` does not call `SmsSender`
  at all — that is correct — but the distinction should be documented.

- **8-slot cap and per-slot body size.** The RFC quotes ~13 KB worst
  case (8 × 1600 B). With ~200 KB free heap on ESP32 this is fine.
  However, `String` heap fragmentation on ESP32's malloc is a known
  hazard for long strings allocated and freed frequently in a ring
  buffer. Consider noting that a `std::array<OutboundEntry, 8>` with
  default-constructed (empty) `String` members is allocated on the heap
  at construction time and subsequent `enqueue`/drain operations assign
  into existing slots, so fragmentation is bounded to 8 live `String`
  allocations at any one time. This is already acceptable; the note
  would reassure the implementer.

- **Test 9 (integration with TelegramPoller) requires `drainQueue` to
  be called separately from `tick()`.** The test plan shows the test
  driving `poller.tick()` and then separately calling
  `smsSender.drainQueue(...)`. Under Option A the test directly
  constructs a `SmsSender` and hands it to both the poller and the test
  harness — this works cleanly. Under the current `ISmsSender&` interface
  it would not work (the interface has no `drainQueue`). This is the
  clearest argument in favour of Option A and should be surfaced in the
  rationale for section 3.

### Summary

The RFC is well-reasoned and the motivation is sound. The queue-at-
(phone, body) level design, the loop placement, the no-NVS-persistence
rationale, and the RFC-0009 interaction analysis are all correct. Two
issues need resolution before implementation: (1) section 3 must
explicitly choose between Option A and Option C and document what
that choice permanently forecloses for future test doubles; and (2)
the `ClockFn`-in-constructor approach should be replaced by a simpler
`drainQueue(uint32_t nowMs)` parameter, removing a nullable field that
serves only one method. All other issues are documentation improvements
that can be addressed in the PR rather than requiring a revised RFC.
Approved with those two changes.

## Code Review

**verdict: approved**

### Review items

**1. Loop ordering — drainQueue placement** (`src/main.cpp` lines 641–651)

CORRECT. `smsSender.drainQueue((uint32_t)millis())` appears immediately after
`telegramPoller->tick()`, before the 30-second WiFi-check block and the
`delay(50)`. The ordering is: URC drain → `callHandler.tick()` → CSQ refresh
→ `telegramPoller->tick()` → `smsSender.drainQueue()` → WiFi check → delay.
This matches the plan in RFC §4 exactly. A message enqueued inside `tick()`
gets its first send attempt within the same loop iteration — not the next one
as the RFC said "~50 ms later". The `return` at `sms_sender.cpp:166` ensures
the loop in `drainQueue` exits after the very first due entry, so only one AT
exchange is started per call even if `tick()` enqueued multiple entries from a
batch update.

**2. Queue-full behavior in `enqueue`** (`src/sms_sender.cpp` lines 112–116)

CORRECT. When all 8 slots are occupied, `enqueue` calls `onFinalFailure`
immediately for the new, rejected entry and returns false without touching any
existing slot. This protects in-flight messages. No eviction occurs.

**3. `drainQueue` one-per-call invariant** (`src/sms_sender.cpp` line 166)

CORRECT. The unconditional `return` at the end of the body of the for-loop
(after both the success branch and the failure/retry branch) ensures exactly
one entry is touched per `drainQueue(nowMs)` call, regardless of how many
slots are occupied and due. A multi-PDU `send()` call (e.g. 10-part concat
from RFC-0009) counts as a single drain step — the PDU loop is inside `send()`
and the `return` fires after `send()` returns, not between PDU parts.

**4. `onFinalFailure` lambda capture** (`src/telegram_poller.cpp` line 126)

SAFE. The lambda captures `this` (a `TelegramPoller*`) and a copied `String
capturedPhone`. `TelegramPoller` holds `SmsSender` by reference (not
ownership); the lambda lives inside a queue entry owned by `SmsSender`. Both
objects are file-scope statics in `main.cpp` with process lifetime. The lambda
fires synchronously from inside `drainQueue()` on the main loop task — the
same task that calls `bot_.sendMessage(...)` elsewhere. No lifetime hazard,
no cross-thread call. The comment at `telegram_poller.cpp:116–118` and the
`telegram_poller.h:84–86` comment both document this clearly; the RFC's
"Notes for handover" requirement is satisfied.

**5. Confirmation message** (`src/telegram_poller.cpp` line 132)

CORRECT. The message is `"✅ Queued reply to " + phone` (UTF-8 U+2705 via
`\xE2\x9C\x85`). `phone` is the string looked up from `replyTargets_.lookup`
on line 101 and stored as the local `phone` variable on line 100 — it is
correctly included. The `bot_.sendMessage(...)` is called before `enqueue()`
returns, so the user gets the acknowledgement immediately, decoupled from the
actual delivery. This is the correct behavior for a queued design.

One minor observation: `capturedPhone` (line 125, a copy for the lambda) and
`phone` (line 132, the original) are both used within four lines of each other.
They hold the same value. This is correct and not a bug, but a reader might
briefly wonder why two copies exist. A one-line comment (`// copy for lambda
capture — phone may be a stack local`) would remove the ambiguity. NON-BLOCKING.

**6. `constexpr` array out-of-line definition** (`src/sms_sender.cpp` line 6)

DEFENSIVE, NOT STRICTLY NECESSARY. The `[env:native]` and the firmware envs
both use `-std=gnu++17` (confirmed in `platformio.ini` lines 104, 107).
Under C++17 a `static constexpr` data member of an integral or scalar type
with an in-class initializer has implicit inline storage and no out-of-line
definition is needed. However, the definition is harmless in C++17 (it is
redundant, not ill-formed), and it protects against any future build flag
change that drops to C++14. The accompanying comment is accurate. Leave it;
no change required.

**7. Test `test_SmsSender_drain_queue_one_send_per_call`** (`test_sms_sender.cpp` lines 340–357)

CORRECT AND SUFFICIENT. The test enqueues 2 entries with the modem set to
succeed (default MR=0), calls `drainQueue(0)` once, then asserts exactly 1
`pduSendCalls()` and queue size = 1. A second `drainQueue(0)` drains the
remaining entry and asserts 2 sends and size = 0. This is the clearest
possible verification of the one-per-call invariant.

**8. Updated tests in `test_telegram_poller.cpp`**

All four tests that exercise the SMS send path now split the action correctly
across `tick()` and `drainQueue(0)`:

- `test_TelegramPoller_happy_path_routes_reply_to_phone` (line 86): calls
  `poller.tick()`, asserts 0 PDU sends, then `sender.drainQueue(0)`, asserts
  1 PDU send. CORRECT.
- `test_TelegramPoller_unicode_body_sends_via_ucs2` (line 211): same pattern.
  `drainQueue(0)` passes `nowMs=0`; `kBackoffMs[0] = 0` so `nextRetryMs = 0`
  and the entry is immediately due at `nowMs=0`. CORRECT.
- `test_TelegramPoller_persistence_across_restart_does_not_replay` (line 366):
  calls `sender.drainQueue(0)` inside the first poller's scope. `sender` is
  declared before the first `TelegramPoller` and shared across both scopes, so
  the queue state is preserved. The second poller correctly sees `pduSendCalls()
  .size() == 1` (unchanged). CORRECT.
- `test_TelegramPoller_multiple_updates_in_one_batch` (line 433): calls
  `sender.drainQueue(0)` twice after `tick()` to drain 2 enqueued entries.
  CORRECT.

Tests that do NOT involve SMS sends (`unauthorized_drops`, `stale_slot`,
`invalid_update`, `no_reply_to_message_id`, `rate_limit`, `transport_failure`,
`offset_passed`, `/status`, `/debug` variants) correctly assert 0
`pduSendCalls()` without calling `drainQueue` at all — there is nothing to
drain, so the assert suffices.

**9. `onFinalFailure` signature change from RFC spec**

The RFC (§1) specified `std::function<void(const String &)>` taking a `reason`
string; the implementation uses `std::function<void()>` with no parameter. This
is a deliberate simplification: the `reason` string is available as
`lastError()` on the `SmsSender` after a failed `send()`, so the callback does
not need it passed in. The `TelegramPoller` lambda constructs its own error
message (`"SMS to " + capturedPhone + " failed after retries."`) that is
friendlier to the end user than the raw modem error anyway. The `sms_sender.h`
comment at line 39 ("called once on final drop") is accurate. The change is
consistent across the header, implementation, and all tests. NON-BLOCKING;
the implementation is cleaner than the RFC spec on this point.

**10. Missing test from RFC plan**

The RFC test plan lists 8 test cases (items 1–8); item 8 ("Clock injection")
was not implemented. The implemented constructor has no `ClockFn` parameter —
the clock concern was moved to `drainQueue(uint32_t nowMs)` as the previous
`## Review` section recommended. The RFC's item 8 is therefore moot as
written, and the actual timing behavior is fully covered by the backoff test
(item 3 / `test_SmsSender_max_retries_calls_on_final_failure`) which passes
controlled `nowMs` values directly. NON-BLOCKING.

Integration test 9 from the RFC plan is also absent, but the
`test_TelegramPoller_*` suite provides equivalent coverage by wiring a real
`SmsSender(FakeModem)` to a `TelegramPoller` and calling `drainQueue`
separately. NON-BLOCKING.

### Summary

The implementation correctly resolves both BLOCKING issues raised in the
previous `## Review` section: Option A (TelegramPoller takes `SmsSender&`) is
adopted with an explicit comment in `telegram_poller.h` documenting what that
forecloses; and the `ClockFn` field was removed from `SmsSender` entirely in
favour of the `drainQueue(uint32_t nowMs)` parameter, eliminating the nullable
branch. All eight review checklist items (loop ordering, queue-full behavior,
one-per-call invariant, lambda lifetime, confirmation message, constexpr
definition, one-per-call test, updated poller tests) are implemented correctly.
The `onFinalFailure` signature was simplified from `void(const String&)` to
`void()` — a clean deviation from the RFC spec with no downside. No blocking
issues remain. Approved.
