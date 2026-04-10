---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0055: /ntp command for on-demand NTP resync

## Motivation

NTP is synced once at boot. Over long uptimes the clock may drift, or the
initial sync may have failed silently. `/ntp` lets the user force a resync
and confirm the new time without requiring a reboot.

## Plan

**`src/telegram_poller.h`** — add setter and member:
```cpp
void setNtpSyncFn(std::function<void()> fn) { ntpSyncFn_ = std::move(fn); }
std::function<void()> ntpSyncFn_;
```

**`src/telegram_poller.cpp`** — add handler before `/ping`:
```cpp
if (lower == "/ntp") {
    bot_.sendMessageTo(u.chatId, "Syncing NTP...");
    ntpSyncFn_();
    if (time(nullptr) > 8 * 3600 * 2)
        bot_.sendMessageTo(u.chatId, "✅ NTP synced: YYYY-MM-DD HH:MM UTC");
    else
        sendErrorReply(u.chatId, "NTP sync failed (clock still invalid).");
}
```

**`src/main.cpp`** — wire after poller is constructed:
```cpp
telegramPoller->setNtpSyncFn([]() { syncTime(); });
```

**`src/telegram.cpp`** — register `/ntp` command.

## Notes for handover

`syncTime()` blocks for up to ~5s (500ms polls until `time() > 16h`).
This is acceptable for an on-demand operator command. The WDT reset inside
`syncTime()` prevents watchdog trips during the wait.
