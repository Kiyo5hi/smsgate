---
status: implemented
created: 2026-04-09
updated: 2026-04-09
owner: claude-opus-4-6
---

# RFC-0005: Notify Telegram on incoming voice call, then hang up

## Motivation

The bridge already forwards SMS to Telegram. Phone calls to the same
SIM currently do nothing visible on the bridge side — the caller hears
endless ringback and the user has no idea anyone tried to reach them.
Since voice is physically impossible on this hardware (no speaker/mic
welded to SPK/MIC pads, see RFC-0008), the best we can do is:

1. Detect that a call is ringing in
2. Read the caller's number
3. Push a Telegram notification to the user
4. Hang up so the caller gets a busy/disconnect instead of ringing
   forever

This turns every call attempt into an actionable Telegram alert. It is
a small, contained feature that exercises the same URC-drain loop we
already use for SMS, so the cost is mostly glue code plus a new module.

## Dependencies

- **Hard: RFC-0007 (testability).** This feature introduces a new URC
  parser (`+CLIP: "<number>",<type>,...`) and a stateful dedupe window
  (multiple `RING` URCs per call). Both should be unit-tested host-side
  against canned URC strings. The `CallHandler` class should take
  `IModem&` and `IBotClient&` via constructor from day one.
- **Soft: RFC-0001 (TLS).** Not a blocker for this specific feature,
  but the same bot token ships through `sendBotMessage` for the call
  notification, so do not ship this before 0001 for the same exfil
  reasons SMS forwarding already has.

## Current state

- `MODEM_RING_PIN` is `#define`'d in `utilities.h` (GPIO 33 on the
  T-A7670X variant used) and `pinMode`'d to `INPUT_PULLUP` at the top
  of `setup()` in `main.cpp`, but nothing ever reads it. The hardware
  signal exists; the firmware ignores it.
- `AT+CLIP` is not enabled. `RING` URCs are emitted by the modem
  anyway but without caller ID.
- TinyGSM on A76xx inherits `TinyGsmCalling` (see
  `TinyGsmClientA76xx.h:82` and `TinyGsmCalling.tpp`), which exposes
  `callAnswer()` / `callNumber(number)` / `callHangup()`. A76xx does
  **not** override `callHangupImpl`, so `modem.callHangup()` resolves
  to the base implementation at `TinyGsmCalling.tpp:72-75`, which
  sends `ATH`. The SIM7600 specialization overrides it to `AT+CHUP`,
  but A76xx does not inherit from SIM7600 — it has its own base in
  `TinyGsmClientA76xx.h`, and that base does not touch the hangup
  path. This is verified authoritatively from the upstream source;
  there is nothing left to check at implementation time. Both `ATH`
  and `AT+CHUP` are valid hangup commands on A76xx, so
  `modem.callHangup()` is the default. We still fall back to
  `modem.sendAT("+CHUP")` on `callHangup()` failure as a defensive
  belt-and-braces measure, not because the dispatch is uncertain.

## Plan

### Modem configuration (in `setup()` / `SmsHandler` init path)

After `+CNMI` is configured, add:

```
modem.sendAT("+CLIP=1");   // enable caller line identification presentation
modem.waitResponse();
```

Don't bother with `+CRC=1` — the extended `+CRING: VOICE` URC is nice
to have but not required for this feature, and enabling it just
doubles the number of per-ring URCs we have to dedupe.

### URC handling (in the `loop()` drain)

Extend the existing `SerialAT.readStringUntil('\n')` loop in `main.cpp`
(post-0007: the `SmsHandler` / `CallHandler` dispatch) to recognize
two additional line shapes:

- `RING` — the line is ringing. Bare URC, no caller info. Emitted
  every ~3 seconds by the modem until the call ends.
- `+CLIP: "<number>",<type>,"",,"",0` — carries the caller number.
  Emitted once per `RING` when `+CLIP=1` is set, interleaved with or
  directly after each `RING`.

The number field may be empty for withheld / anonymous callers
(`+CLIP: "",128,...`). Handle that as "unknown caller" rather than
silently dropping the event.

### Dedupe (the important bit)

One inbound call produces multiple `RING` URCs until the caller hangs
up or until we `AT+CHUP`. We want **one** Telegram notification per
call, not one per ring. State machine, stored on the `CallHandler`
instance:

- `idle` → on first `+CLIP`, capture the number, transition to
  `ringing`, and queue the notification+hangup
- `ringing` → ignore subsequent `RING`/`+CLIP` for the same call. Also
  start a "call ended" timer: if no `RING` is seen for > N seconds
  (say 6s — just over two ring periods), transition back to `idle` so
  the next real call is not suppressed
- on successful `modem.callHangup()`, transition immediately to `idle`
  without waiting for the timer

Do *not* key dedupe on the phone number alone — two back-to-back calls
from the same number should produce two notifications.

### Action

On the `idle → ringing` edge:

1. Build a message: `"📞 Incoming call from <humanReadablePhoneNumber(number)>"`.
   Reuse the existing phone formatter from `sms.cpp` (post-0007 it
   lives in `sms_codec.h` and can be shared).
2. Call `botClient.sendMessage(msg)`. Use the same consecutive-failure
   counter semantics as `SmsHandler` — if Telegram is unreachable,
   don't burn the reboot budget on calls specifically.
3. Call `modem.callHangup()`. Check the return. If it returns false,
   fall back to `modem.sendAT("+CHUP")` + `waitResponse()`. If that
   also fails, log and move on — the call will time out naturally
   after ~30s.

### New module

`src/calls.{h,cpp}`:

```cpp
// calls.h (sketch)
class CallHandler {
 public:
  CallHandler(IModem &modem, IBotClient &bot);
  void configure();                     // enables +CLIP=1
  void onUrcLine(const String &line);   // called from main loop
  void tick(uint32_t nowMs);            // drives the idle-return timer
};
```

Free functions in `calls_codec.h` for the parser bits so they can be
unit tested without instantiating the class:

```cpp
struct ClipUrc { String number; bool valid; bool withheld; };
bool parseClipLine(const String &line, ClipUrc &out);
bool isRingLine(const String &line);
```

### Hardware path is out of scope

This RFC deliberately does **not** attach an ISR to `MODEM_RING_PIN`.
The URC-drain loop already runs often enough to pick up `+CLIP`
within ~50ms of it being emitted, which is fast enough for a
human-timescale event. Using the hardware interrupt is RFC-0006's
job and is conditional on the user moving to battery power; as long
as we are USB-powered, polling is simpler and adequate.

## Test plan

### Unit tests (via RFC-0007)

- `parseClipLine` well-formed: `+CLIP: "+8613912345678",145,"",,"",0`
  → `{number: "+8613912345678", valid: true, withheld: false}`
- `parseClipLine` withheld: `+CLIP: "",128,"",,"",0`
  → `{number: "", valid: true, withheld: true}`
- `parseClipLine` malformed: truncated, missing quotes, extra commas
- **Parser tolerance note.** The `+CLIP` parser must tolerate
  variation in the trailing fields across firmware versions — e.g.
  some A76xx builds emit only `+CLIP: "<number>",<type>` with no
  trailing empty strings, others append extra fields. Count commas
  leniently, treat anything past the first two fields as optional,
  and skip to `\r\n`. Do **not** strict-match the field count.
- `isRingLine` happy path and false positives (`RINGING`, `RING!`
  etc.). Note that `RING!` is adversarial input — real modems do not
  emit `RING!`, this is a fuzz-style negative test case making sure
  the matcher doesn't accept prefix garbage. It is not a format the
  parser is expected to handle in the wild.
- `CallHandler` state machine with `FakeModem` and `FakeBotClient`:
  - single call: one `+CLIP` + three `RING` → one `sendMessage` call,
    one `callHangup()` call
  - withheld caller: notification says "unknown"
  - two back-to-back calls from the same number (separated by
    `tick()` past the idle-return timeout) → two notifications
  - Telegram send fails: no crash, `callHangup()` still called,
    failure counter on `IBotClient` increments
  - `callHangup()` fails: fallback `+CHUP` is sent

### Manual tests on hardware

- Call from a known number with caller ID on → TG message contains
  formatted number
- Call from `*67` / withheld → TG says "unknown caller"
- Call, let it ring 3 times, the bridge should hang up before the 4th
- Two calls in quick succession from the same number → two TG
  messages, not one
- Send an SMS during a ringing call → SMS is still forwarded (URC
  drain should handle both event types in the same loop pass)

## Notes for handover

- The T-A7670X SIM variant (`A7670G-LLSE`) is confirmed voice-capable
  in terms of the AT stack — `AT+CLIP`, `AT+CHUP`, and `RING` URCs all
  work regardless of whether audio hardware is wired up. We don't
  touch audio here, only signalling.
- If `callHangup()` returns false intermittently in the field, prefer
  `modem.sendAT("+CHUP")` as the default hangup path rather than
  toggling per call. Simpler state machine.
- Do not enable `+CRC=1` unless the dedupe state machine turns out to
  need it. `+CRING: VOICE` vs `+CRING: DATA` distinction is only
  useful if we ever want to behave differently on data calls, which
  we don't.
- Future extension: selectively accept calls from an allow-list (e.g.
  "if the caller is <family member>, answer briefly with a TTS
  message"). Parked in RFC-0008 as rejected, because there is no
  speaker on the board. Do not re-open.
