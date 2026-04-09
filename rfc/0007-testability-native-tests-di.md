---
status: implemented
created: 2026-04-09
updated: 2026-04-09
owner: claude-opus-4-6
---

# RFC-0007: Testability via native tests and dependency injection

## Motivation

The SMS pipeline has real edge cases (UCS2 decoding, CMGR parsing, phone
number formatting, consecutive-failure reboot threshold) that should be
testable without flashing hardware. Today they are not:

- `sms.cpp` borrows a global `TinyGsm modem` via `extern`, so any host
  build of the logic drags in the full TinyGSM library.
- `postSMSMessage` calls the free function `sendBotMessage`, which in
  turn owns a file-static `WiFiClientSecure`. No seam to inject a fake.
- The reboot path calls `ESP.restart()` directly.
- Pure helpers (`decodeUCS2`, `parseCmgrBody`, `humanReadablePhoneNumber`,
  `timestampToRFC3339`) are `static` inside `sms.cpp`, so even if they
  are Arduino-free they're unreachable from a test translation unit.

The fix is the usual one: extract pure helpers into their own file, put
the stateful bits behind interfaces, and wire them together in
`main.cpp`. Then add a PlatformIO `native` test env that only compiles
the pure file + tests + hand-rolled Arduino stub.

## Current state

- `src/main.cpp` is a composition root: builds a real modem adapter, a
  real Telegram bot client, and a `RebootFn`, then hands them to an
  `SmsHandler`.
- `src/sms_codec.{h,cpp}` holds the four pure helpers. Depends only on
  the Arduino `String` class (satisfied by the native stub on host).
- `src/imodem.h` declares `IModem` with the subset of TinyGSM used by
  the SMS pipeline: `sendAT(cmd)`, `waitResponse(timeout, out)`,
  `waitResponseOk(timeout)`.
- `src/ibot_client.h` declares `IBotClient` with `sendMessage(text)`.
- `src/sms_handler.{h,cpp}` owns the consecutive-failure counter, the
  `MAX_CONSECUTIVE_FAILURES = 8` constant, and the public
  `handleSmsIndex` / `sweepExistingSms` methods. Constructor takes
  `IModem&`, `IBotClient&`, `RebootFn`.
- `src/real_modem.h` is a header-only adapter wrapping the global
  `TinyGsm modem`. Only compiled into the firmware env.
- `src/telegram.{h,cpp}` exposes a `RealBotClient` class implementing
  `IBotClient::sendMessage`. Keeps the file-static `WiFiClientSecure`
  (it's an implementation detail, not a testing seam). The
  Content-Length drain loop is copied verbatim from the previous free
  function — do not regress it.
- `test/support/Arduino.{h,cpp}` is a hand-rolled subset stub covering
  ~18 `String` methods actually used by `sms_codec` and `sms_handler`.
- `test/support/fake_modem.h` and `test/support/fake_bot_client.h`
  implement the interfaces and record call sequences for assertions.
- `test/test_native/test_*.cpp` contains Unity tests:
  - `test_sms_codec.cpp`: UCS2 decode (ASCII, Chinese, surrogate pair,
    malformed odd, malformed non-hex, empty, whitespace),
    `parseCmgrBody` (happy path, missing OK, missing 8th quote, empty
    body), `humanReadablePhoneNumber` (11-digit, +86 13-digit, foreign,
    edge lengths), `timestampToRFC3339` (happy path, too short).
  - `test_sms_handler.cpp`: success deletes the SMS; failure keeps it
    and increments the counter; N consecutive failures triggers the
    injected `RebootFn` exactly once.
- `platformio.ini` has a new `[env:native]` with
  `build_src_filter = +<sms_codec.cpp> -<*>` so only the pure file is
  compiled into the host test binary.

## Plan

Implemented in this RFC. The remaining open work (if any) is listed in
"Notes for handover" below.

## Notes for handover

- The `IModem` interface intentionally does **not** expose all of
  TinyGSM. If a future RFC (e.g. bidirectional Telegram->SMS, RFC-0003)
  needs `sendSMS` or similar, extend `IModem` and the `RealModem`
  adapter together, and add a matching fake.
- The hand-rolled `String` stub is the minimum needed to compile the
  pure code and tests. If you extend `sms_codec` and hit a missing
  method, add it to the stub rather than pulling in a real Arduino-core
  shim — the goal is that host tests stay fast and hermetic.
- `RebootFn` is a `std::function<void()>` on host and a plain function
  pointer on device (both accept the lambda in main.cpp). If you ever
  need parameters, change it to a small concrete virtual interface —
  don't start threading captures through the pointer.
- The `MAX_CONSECUTIVE_FAILURES = 8` threshold is now a `static
  constexpr` on the class; if someone wants to make it runtime-tunable,
  extend the constructor rather than re-adding a preprocessor macro.
