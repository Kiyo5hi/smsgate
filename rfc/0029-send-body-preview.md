---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0029: `/send` confirmation includes body preview

## Motivation

The `/send` confirmation only shows the phone number. A typo in the body
is invisible until the recipient replies. Showing the first 30 characters
of the body lets the user catch mistakes immediately.

## Plan

In `telegram_poller.cpp`, replace the one-line confirmation:
```cpp
bot_.sendMessageTo(u.chatId, String("✅ Queued SMS to ") + phone);
```
with:
```cpp
String preview = body.substring(0, 30);
if (body.length() > 30) preview += "…"; // U+2026
bot_.sendMessageTo(u.chatId, String("✅ Queued to ") + phone + ": " + preview);
```

Result: `✅ Queued to +8613800138000: Hello world`

## Notes for handover

Only `src/telegram_poller.cpp` changed. Update the happy-path test to
match the new "Queued to" prefix and body content check.
