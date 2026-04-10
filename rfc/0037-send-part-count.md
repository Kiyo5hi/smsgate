---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0037: Part count in `/send` confirmation

## Motivation

A long `/send` body is silently split into multiple SMS parts, each of which costs a
separate carrier charge. The user has no idea until their recipient sees fragmented
messages. Showing "2 parts" in the confirmation lets the user decide whether to shorten
the message before it goes out.

## Plan

### `src/sms_codec.h` / `sms_codec.cpp`

Add a thin wrapper:
```cpp
int countSmsParts(const String &body, int maxParts = 10);
// Calls buildSmsSubmitPduMulti("+1", body, maxParts) and returns .size().
```

### `src/telegram_poller.cpp`

After computing the body preview, append part count when >1:
```cpp
int parts = sms_codec::countSmsParts(body);
String confirmText = String("✅ Queued to ") + phone + String(": ") + preview;
if (parts > 1)
    confirmText += String(" (") + String(parts) + String(" parts)");
int32_t confirmId = bot_.sendMessageReturningId(confirmText);
```

Result: `✅ Queued to +8613800138000: Hello world… (3 parts)`

## Notes for handover

- `src/sms_codec.h` — `countSmsParts()` declaration
- `src/sms_codec.cpp` — 4-line implementation
- `src/telegram_poller.cpp` — include `sms_codec.h`, part count in confirmation
- 6 new tests in `test_sms_codec.cpp` (empty, short, 160/161 GSM-7, 70/71 UCS-2)
- 1 new test in `test_telegram_poller.cpp` (multi-part shows "parts" in message)
