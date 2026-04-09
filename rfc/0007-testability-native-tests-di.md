---
status: implemented
created: 2026-04-09
updated: 2026-04-09
owner: claude-opus-4-6
---

# RFC-0007: Testability — native host tests, interfaces, dependency injection

## Motivation

Every test today requires a hardware build+flash+monitor cycle:

```
save → "$PIO" run -e T-A7670X -t upload → wait → --raw --quiet monitor → read
```

That is roughly 40–60 seconds per iteration, plus manual work to
provoke real events (ask a friend to send an SMS, call the number, or
wait for a verification code). For the work in flight today this is
already too slow:

- RFC-0001 (TLS bundle) — needs a round trip against the real network
  anyway, but everything *around* it (the retry path, the draining of
  leftover bytes, the status-line parse) should be testable on host
- RFC-0002 (PDU decoder) — pure-function work against hex blobs. It is
  absurd to flash a board to iterate on an encoding algorithm
- RFC-0003 (Telegram → SMS) — long polling, `update_id` tracking,
  `reply_to_message_id` → phone number map, authorization. All
  stateful, all exactly the kind of thing where a unit test catches
  off-by-ones that board tests never will
- RFC-0005 (call notify) — URC parser + dedupe state machine. Same
  story

We have **≥4 features in flight that all touch the same SMS pipeline
and Telegram client**. Without a host test runner we cannot land them
in parallel without constant regression risk. This RFC unblocks the
others and is therefore the highest-priority item in the pipeline,
even though on its own it ships zero new user-visible behaviour.

## Current state

- `src/sms.cpp` declares `extern TinyGsm modem;` and uses the global
  modem directly. It calls `sendBotMessage()` (a free function from
  `telegram.h`) to deliver messages. Neither dependency is
  substitutable without editing source.
- `src/telegram.cpp` has a file-static `WiFiClientSecure telegramClient`
  and reads `TELEGRAM_BOT_TOKEN` / `TELEGRAM_CHAT_ID` at file scope
  directly from `secrets.h`. Neither is injectable.
- Helper functions in `sms.cpp` that should be easy to test
  (`decodeUCS2`, `parseCmgrBody`, `humanReadablePhoneNumber`,
  `timestampToRFC3339`, `isHexString`) are all `static` — file-local,
  so even if a native test could link against `sms.cpp`, it couldn't
  call them.
- There is no `test/` directory. `platformio.ini` has no `[env:native]`.

None of this is testable on host without massive shimming.

## Plan

### 1. PlatformIO native env

Add to `platformio.ini`:

```ini
[env:native]
platform = native
test_framework = unity
build_flags =
    -std=c++17
    -Isrc
    -Itest/support
    -DUNIT_TEST
build_src_filter = +<sms_codec.cpp> -<*>
test_build_src = no
```

The `build_src_filter` line is important: it explicitly opts only
`sms_codec.cpp` in, and everything else out, so the native env never
tries to compile `main.cpp`, `sms.cpp`, or `telegram.cpp` — all of
which pull in `TinyGsmClient.h` / `WiFi.h` and will not build on the
host. As later RFCs add host-buildable translation units (e.g. a PDU
decoder from RFC-0002, URC parsers from RFC-0005), add them to the
filter the same way: `+<sms_codec.cpp> +<pdu_codec.cpp> -<*>`.

Tests live under `test/native/`. PlatformIO 6.x picks up any file
matching `test/<filter>/test_*.cpp`, and the native env runs with
`"$PIO" test -e native -f native`. Reference the PlatformIO 6.x
Unit Testing docs for the exact layout if the behaviour ever drifts;
do not invent the layout from memory.

### 2. Arduino host stub

We need a minimal `Arduino.h` and a `String` implementation for host
builds. Two options:

**Option A: use `ArduinoFake` (Platformio library 1321)** — a
community-maintained mock of the Arduino API including `String`,
`Serial`, `millis`, GPIO, etc. Battle-tested, widely used.

- Pros: free coverage for `Serial.print*`, `millis`, `delay`, `String`
  semantics matching Arduino exactly; large user base catches edge
  cases we would miss
- Cons: external dep; it mocks a *lot* more than we use; its `String`
  is real Arduino `WString` which is harder to step through in a
  debugger than `std::string`

**Option B: hand-roll a minimal stub under `test/support/Arduino.h`** —
typedef `String` to a thin wrapper around `std::string` that
implements just the methods we actually touch: `length`, `substring`,
`indexOf` (two forms), `toInt`, `startsWith`, `trim`, `toLowerCase`,
`operator+=`, `operator+`, `c_str`, `charAt`. Stub `millis()` with a
testable `setMockMillis(ms)`. `Serial.print*` become no-ops (or
captured into a string for assertion).

- Pros: zero dependencies; the stub is 100 lines; every behaviour is
  under our control which makes debugging host failures trivial; we
  can expose seams (`setMockMillis`) that `ArduinoFake` doesn't
- Cons: we have to maintain it; subtle Arduino `String` quirks (COW,
  capacity growth, empty-on-failure returns) may differ from real
  hardware behaviour

**Recommendation: Option B first**, with the understanding that if we
ever hit a divergence from hardware behaviour we can pivot to
`ArduinoFake`. The surface area is small — the functions we actually
call on `String` fit in one printed page — and keeping the stub
dependency-free makes the host build trivially reproducible.

The stub goes in `test/support/Arduino.h` (+ `.cpp` for millis
mocking). Include order at the top of each test file:

```cpp
#include <Arduino.h>   // picks up the stub via -Itest/support
#include "sms_codec.h"
#include <unity.h>
```

### 3. Interfaces

Create `src/interfaces.h`:

```cpp
#pragma once
#include <Arduino.h>

// Anything the SMS / call handlers need from the modem, abstracted
// so a FakeModem can provide canned responses in tests.
class IModem {
 public:
  virtual ~IModem() = default;

  // Returns raw response text of the AT command, empty on timeout.
  // Thin-enough wrapper around TinyGsm::sendAT + waitResponse.
  virtual bool sendAtCommand(const String &cmd, uint32_t timeoutMs, String &response) = 0;

  // Convenience wrappers for common commands so callers don't have
  // to hand-format strings each time.
  virtual bool readSms(int index, String &rawCmgrResponse) = 0;
  virtual bool deleteSms(int index) = 0;
  virtual bool listAllSms(String &rawCmglResponse) = 0;

  // For RFC-0005.
  virtual bool callHangup() = 0;

  // For RFC-0003.
  virtual bool sendSmsUtf16(const String &number, const String &text) = 0;
};

// Anything the message-sending pipeline needs from "the bot", abstracted
// so a FakeBotClient can capture outgoing messages in a vector.
class IBotClient {
 public:
  virtual ~IBotClient() = default;
  virtual bool sendMessage(const String &text) = 0;
};
```

Real implementations (firmware only, not compiled for host):

- `src/tinygsm_modem_adapter.{h,cpp}` — `class TinyGsmModemAdapter : IModem`
  that wraps a `TinyGsm&`. The existing calls in `sms.cpp`
  (`modem.sendAT(...)` + `modem.waitResponse(5000UL, raw)`) move
  behind `sendAtCommand` / `readSms` / `deleteSms`. Zero behaviour
  change on hardware.
- `src/telegram_bot_client.{h,cpp}` — `class TelegramBotClient : IBotClient`
  that owns the `WiFiClientSecure`, takes the bot token / chat ID in
  its constructor (instead of reading `TELEGRAM_BOT_TOKEN` at file
  scope), and implements `sendMessage`.

Test implementations (under `test/support/`, only compiled for the
native env):

- `FakeModem` — constructor takes a lambda or a list of scripted
  `{input_prefix, output}` pairs. `sendAtCommand` looks up the input
  by prefix match and returns the canned output. Also exposes a
  `std::vector<String> commandsSent` for assertion.
- `FakeBotClient` — `sendMessage` pushes into a
  `std::vector<String> messagesSent` and returns a configurable bool
  (`setNextResult(true/false)` or a deterministic sequence via
  `setResultSequence({true, false, true})`).

**Interface evolution.** The interfaces above are the minimum surface
needed to test the SMS forwarding path. They will grow as later RFCs
land:

- RFC-0003 (Telegram → SMS) will extend `IBotClient` with a
  `pollUpdates(...)` method for long-polling Telegram updates, plus
  whatever persistence seam the `message_id → phone` ring buffer needs
  (probably an `IPreferences`).
- RFC-0005 (incoming call notify) may add a per-channel failure-counter
  accessor on `IBotClient` so the call path can distinguish "Telegram
  unreachable from the call notifier" from "Telegram unreachable from
  the SMS forwarder" without sharing a single global counter.

When these methods are added, fakes already in use by earlier-landed
RFCs (e.g. `FakeBotClient` as used by RFC-0005's test suite) will need
a one-line stub for the new method. That is expected and cheap. Do
**not** over-engineer the interface up front for hypothetical future
methods — the whole point of interfaces here is substitutability, not
prophecy. Each RFC extends the interface only as far as its own tests
need, and the review catches any missed fake stubs at the next PR.

**FakeModem mode tracking.** Once RFC-0002 lands, `+CMGR` responses
will differ between text mode (today, structured text with
`+CMGR: "REC UNREAD",...`) and PDU mode (hex PDU blob). `FakeModem`
should track the current `+CMGF` state by observing AT commands as
they are written through `sendAtCommand` (i.e. watch for
`+CMGF=0` / `+CMGF=1` in `commandsSent`) and return a different
canned `+CMGR` shape depending on the current mode. Alternatively,
scope each test to a single mode explicitly — e.g. `test_sms_handler_text_mode.cpp`
and `test_sms_handler_pdu_mode.cpp` — and have the fake assume that
mode unconditionally. **State the decision in the test file header**
so a reader of an individual test file knows which shape the fake
is handing back without having to read the fake's implementation.

### 4. Refactor `SmsHandler` into a class

`src/sms.h`:

```cpp
#pragma once
#include "interfaces.h"

class SmsHandler {
 public:
  SmsHandler(IModem &modem, IBotClient &bot);

  void handleSmsIndex(int idx);
  void sweepExistingSms();

  // Exposed so tests can observe and drive. Not called from main.
  int consecutiveFailures() const { return failures_; }

  // Reboot is injected as a callable so tests can assert it was
  // invoked without actually restarting the process.
  using RebootFn = void (*)();
  void setRebootHook(RebootFn fn) { reboot_ = fn; }

 private:
  IModem &modem_;
  IBotClient &bot_;
  int failures_ = 0;
  RebootFn reboot_ = &defaultReboot;   // default calls ESP.restart()
  static void defaultReboot();
};
```

The static helpers (`decodeUCS2`, `isHexString`, `parseCmgrBody`,
`humanReadablePhoneNumber`, `timestampToRFC3339`) move out of
`sms.cpp` into a new `src/sms_codec.{h,cpp}` as **free functions in
the `sms_codec` namespace**. They don't touch the modem or the bot,
so they are trivially testable without any fake.

Crucially, `sms_codec.h` must only depend on `<Arduino.h>` — not
`<TinyGsmClient.h>`, not `<WiFi.h>`. That is what lets the host
build compile it.

**Reboot injection.** The existing `ESP.restart()` call in
`handleSmsIndex` (which becomes `SmsHandler::handleSmsIndex`
post-refactor) is routed through a `RebootFn` function pointer
member on `SmsHandler` — **not** gated behind `#ifndef UNIT_TEST`
preprocessor guards. The production default is a `defaultReboot()`
free function that calls `ESP.restart()` and lives in a `.cpp` file
that is only compiled for the firmware env; the host build supplies
its own no-op default. Tests install their own hook via
`setRebootHook(...)`. This is preferred over `#ifndef UNIT_TEST`
because it lets the reboot path be exercised directly from host
tests (as a hook-observation assertion) rather than being a
no-compile branch that drifts silently.

### 5. Refactor `TelegramBotClient`

`src/telegram_bot_client.h`:

```cpp
#pragma once
#include "interfaces.h"
#include <WiFiClientSecure.h>

class TelegramBotClient : public IBotClient {
 public:
  struct Config {
    String botToken;
    String chatId;
    bool useInsecureTls;   // stopgap; flipped off once RFC-0001 lands
  };

  explicit TelegramBotClient(const Config &cfg);
  bool init();                          // opens first connection
  bool sendMessage(const String &text) override;

 private:
  Config cfg_;
  WiFiClientSecure client_;
  bool keepAlive();
};
```

The static ISRG cert blob stays in `telegram_bot_client.cpp` in its
current `PROGMEM` form and continues to be unused until RFC-0001
lands.

Tests for `TelegramBotClient` itself are **out of scope** for the
first cut — the seam is at `IBotClient`, not below it. If the HTTP
response parser ever becomes load-bearing, extract it to
`telegram_http.{h,cpp}` as pure functions and test those.

### 6. `main.cpp` becomes a composition root

After the refactor, `main.cpp` stops calling globals and instead
constructs the three objects once at startup:

```cpp
TinyGsm modem(SerialAT);
TinyGsmModemAdapter modemAdapter(modem);
TelegramBotClient botClient({TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID, /*insecure=*/true});
SmsHandler smsHandler(modemAdapter, botClient);

void setup() {
  // ... modem bringup ...
  botClient.init();
  smsHandler.sweepExistingSms();
}

void loop() {
  // ... URC drain, calls smsHandler.handleSmsIndex(idx) ...
}
```

Net effect: `main.cpp` owns lifetime, nothing else does. Any future
module (CallHandler for 0005, TelegramPoller for 0003) gets its
dependencies from the same place.

### 7. First batch of tests

Under `test/native/`:

**`test_sms_codec.cpp`** — pure functions, no fakes needed:
- `decodeUCS2`: empty input; plain ASCII passthrough (via
  `isHexString` guard); single UCS2 char; two UCS2 chars;
  surrogate pair (U+1F600 grinning face); malformed odd-length
  hex; malformed non-hex chars mid-string
- `parseCmgrBody`: well-formed response with Chinese sender and
  content; missing `OK` terminator; missing `+CMGR:` header;
  truncated header (q8 == -1)
- `humanReadablePhoneNumber`: 11-digit bare Chinese number;
  `+86`-prefixed 13-digit; short code (4-6 digits); international
  non-Chinese (`+1...`); empty
- `timestampToRFC3339`: well-formed CMGR timestamp; too-short input;
  end-of-year wraparound (24-01-01)

**`test_sms_handler.cpp`** — behaviour tests with `FakeModem` and
`FakeBotClient`:

- *Happy path.* `FakeModem` returns a canned `+CMGR` response for
  index 7. `FakeBotClient` returns `true`. Assert:
  - `botClient.messagesSent.size() == 1`
  - `messagesSent[0]` contains the formatted phone number and the
    decoded Chinese body
  - `modem.commandsSent` includes `"+CMGR=7"` and then `"+CMGD=7"`
  - `handler.consecutiveFailures() == 0`
- *Send failure keeps SMS.* `FakeBotClient` returns `false`. Assert:
  - `modem.commandsSent` includes `"+CMGR=7"` but **not** `"+CMGD=7"`
  - `handler.consecutiveFailures() == 1`
- *Send failure escalation to reboot.* Call `handleSmsIndex` 8 times,
  each with a failing `FakeBotClient`. Set a test reboot hook
  (`handler.setRebootHook(...)`) that increments a counter instead
  of calling `ESP.restart()`. Assert the counter == 1 after the 8th
  failure and that `consecutiveFailures` hit the cap.
- *Malformed CMGR is deleted.* `FakeModem` returns garbage for
  `+CMGR`. Assert `+CMGD` was sent (to avoid looping on a bad slot)
  and `sendMessage` was **not** called.
- *`sweepExistingSms` iteration.* `FakeModem` returns a
  `+CMGL="ALL"` response listing three indices. Assert
  `handleSmsIndex` is called for each (observed via `commandsSent`
  containing three distinct `+CMGR=` lines).

### 8. CI

Out of scope for this RFC. The goal is a fast local dev loop first;
GitHub Actions can come later as a one-line `pio test -e native`
job once the test suite exists.

## Cross-cutting expectation

**All of RFC-0001, RFC-0002, RFC-0003, and RFC-0005 will land on top
of 0007.** Each of them should:

- Take their dependencies (`IModem`, `IBotClient`, maybe a new
  `IPreferences` for NVS-backed state in 0003) via constructor
- Put any new pure logic (PDU decoder in 0002, URC parser in 0005,
  update_id tracker in 0003) in a `_codec.h` / `_parser.h` header
  that does **not** include hardware headers, so host tests can call
  it without instantiating a handler
- Ship at least one host test per new behaviour

This is non-negotiable for landing those RFCs. If work on any of them
starts before this RFC is `implemented`, the new code ends up
un-testable by construction and we end up doing the refactor twice.
The review agent should bounce any PR for 0001/0002/0003/0005 that
does not include native tests.

## Notes for handover

- **Biggest risk: the Arduino `String` stub.** Only stub what is
  actually used. Grep the `src/` tree for `String::` call sites at
  refactor time and stub exactly that set. Do not try to reimplement
  COW or the implicit numeric constructors. If a test needs something
  the stub doesn't have, add it to the stub right there in that PR —
  the stub is not a library, it is scaffolding.
- **Do not try to mock `WiFiClientSecure` at the bytes level.** That
  would mean reimplementing HTTP response parsing as a test
  scaffold, which is bigger than the code under test. The seam is at
  `IBotClient`, not below it. If you find yourself writing a
  `FakeWiFiClient` that returns canned HTTP bytes, stop — that's the
  wrong seam.
- **Do not try to mock `TinyGsm` at the AT level** beyond what
  `FakeModem` does (prefix-matched canned responses). A full
  AT-level fake has to understand `waitResponse`'s internal URC
  dispatching and would be brittle in the exact ways we would want
  the tests to be robust against. Keep `IModem` method-level.
- Test file layout: PlatformIO 6.x picks up files matching
  `test/<filter>/test_*.cpp` — put sources in `test/native/` and
  invoke with `"$PIO" test -e native -f native`. See the PlatformIO
  6.x Unit Testing docs for the authoritative syntax, and verify with
  `"$PIO" test --help` if behaviour changes in a future release.
  Do not invent layout conventions that aren't in the docs.
- `defaultReboot()` (the production implementation of the reboot
  hook) calls `ESP.restart()` and lives in a `.cpp` file that is
  only compiled for the firmware env (e.g. kept out of `build_src_filter`
  for `[env:native]`). The host build supplies its own no-op
  `defaultReboot()` symbol instead. Tests install their own hook via
  `setRebootHook`. Using `build_src_filter` rather than an
  `#ifndef UNIT_TEST` preprocessor guard keeps the single source file
  readable and avoids conditional-compilation drift.
- The `FakeModem` / `FakeBotClient` types live under `test/support/`
  (same place as the Arduino stub) and are only compiled for the
  native env. The refactor does **not** add any test-only code to
  `src/`.
- Keep the PR sequence small: (1) add the native env + Arduino stub +
  one trivial passing test so the loop works, (2) extract
  `sms_codec.{h,cpp}` and write pure-function tests, (3) introduce
  `IModem` / `IBotClient` and refactor `SmsHandler`, (4) add
  behaviour tests with fakes. Four reviewable PRs, not one big bang.
