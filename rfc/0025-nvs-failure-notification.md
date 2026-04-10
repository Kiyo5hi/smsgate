---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0025: Telegram notification on NVS initialisation failure

## Motivation

`RealPersist::begin()` failure is currently logged only to serial:

```
RealPersist::begin failed; bidirectional TG->SMS disabled.
```

At this point the Telegram transport is fully functional — WiFi is connected
and `realBot.sendMessage()` works. But no Telegram alert fires. A remote
operator has no signal that persisted state (reply targets, block list, SMS log)
was silently dropped. The rich boot banner (RFC-0022) shows zero entries, which
looks identical to a fresh flash.

## Plan

Extend the failure branch with a best-effort `sendMessage`:

```cpp
if (!realPersist.begin())
{
    Serial.println("RealPersist::begin failed; bidirectional TG->SMS disabled.");
    realBot.sendMessage(String(
        "\xE2\x9A\xA0\xEF\xB8\x8F NVS init failed\n"
        "Persistent state (reply targets, block list, SMS log) is unavailable "
        "for this session. Consider erasing NVS: pio run -t erase"));
}
```

If the transport isn't ready the call returns false (no-op). The serial log
remains the backstop in that case.

## Notes for handover

- Only `src/main.cpp` changes — one `sendMessage` call.
- The NVS check is post-WiFi-setup, so the transport is available in the
  common case.
- No new tests: `main.cpp` is excluded from the native build.
