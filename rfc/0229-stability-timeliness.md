---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0229: Stability and timeliness improvements

## Motivation

Three concrete gaps were identified:

1. **`SerialAT.readStringUntil('\n')` blocks up to 1 second per partial line.**
   The Arduino Stream default timeout is 1000 ms. When a URC arrives
   character-by-character (normal at 115200 baud, ~8.7 ms per 100 chars),
   the very last call—when the stream is empty but no '\n' has been seen
   yet—will stall the main loop for the full 1-second timeout before
   returning a partial string. Under SMS burst conditions this multiplies
   into multi-second stalls.

2. **No periodic modem AT health check.**
   If the A76XX silently hangs (possible after days of uptime, or after a
   modem-firmware crash), the bridge continues running with no SMS reception
   and no warning. The consecutive-Telegram-failure → reboot path does NOT
   fire here because Telegram itself is still reachable via WiFi.

3. **3-second Telegram poll interval is too slow for interactive commands.**
   `/send`, `/reboot`, and similar commands feel sluggish with up to 3 s
   before the device even sees the message. 1 s polling at short-poll
   (`timeout=0`) costs ~90 KB/day — negligible.

## Plan

1. **`SerialAT.setTimeout(100)`** — set immediately after `SerialAT.begin()`
   in `setup()`. 100 ms is far longer than any realistic inter-character gap
   at 115200 baud (0.087 ms per char), so no valid URC line will be cut short.
   It prevents 1-second hangs on empty-stream reads.

2. **Modem AT health check** — a periodic `modem.testAT(3000)` call in
   `loop()`, fired at most every `kModemCheckIntervalMs` = 5 minutes. If
   `testAT` returns false three consecutive times, call `ESP.restart()` and
   emit a Telegram alert first (best-effort, WiFi may be up).

3. **`kPollIntervalMs` reduced from 3000 → 1000 ms** — no other changes to
   the polling path; short-poll (`kPollTimeoutSec = 0`) is preserved.

## Notes for handover

The `testAT` call sends `AT` and waits for `OK`. It is safe to call from
`loop()` as long as no other AT transaction is in flight. The 5-minute
interval ensures it never fires during an active SMS receive/send cycle;
the `inAtTransaction` guard (if needed) is not required here because the
check fires on a 5-minute timer, not on every loop tick.
