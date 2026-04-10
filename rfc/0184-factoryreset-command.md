---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0184: `/factoryreset` command — wipe NVS and reboot

## Motivation

Recovering a misconfigured device currently requires physical access
(flash erase via `pio run -t erase`). A remote operator should be able
to wipe all persisted settings and reboot to defaults from Telegram.

## Plan

### Two-step confirmation

`/factoryreset` alone returns a warning with instructions to type
`/factoryreset confirm` to proceed. This guards against accidental
invocation.

`/factoryreset confirm` calls `clearNvsFn_()` then `rebootFn_(chatId)`.

### `IPersist::clearAll()`

New pure-virtual method. Clears all keys in the "tgsms" NVS namespace:
- `RealPersist::clearAll()` — calls `prefs_.clear()`
- `FakePersist::clearAll()` — resets `lastUpdateId_`, clears
  `replyTargets_`, and clears `blobs_`; increments `clearAllCalls_`
  counter for test assertions.

### `TelegramPoller`

New setter `setClearNvsFn(std::function<void()>)`. Handler:
```cpp
if (lower == "/factoryreset") {
    bot_.sendMessageTo(chatId,
        "⚠️ This will erase ALL persisted NVS settings and reboot.\n"
        "Aliases, block list, labels, timezone, and all runtime\n"
        "configuration will return to firmware defaults.\n\n"
        "Type /factoryreset confirm to proceed.");
    return;
}
if (lower == "/factoryreset confirm") {
    bot_.sendMessageTo(chatId, "🗑 NVS cleared — rebooting now...");
    if (clearNvsFn_) clearNvsFn_();
    if (rebootFn_)   rebootFn_(chatId);
    return;
}
```

### `main.cpp`

```cpp
telegramPoller->setClearNvsFn([]() { realPersist.clearAll(); });
```

## Notes for handover

- After clearAll() + reboot, the device comes up with firmware-default
  settings (UTC+8, no fwdTag, forwarding ON, blockMode ON, etc.) — all
  the pre-RFC-0182/0183 defaults.
- No NVS data survives. This includes reply-target routing, update_id
  watermark, lifetime counters, and debug log.
- Only one fn setter needed — the existing `rebootFn_` handles the
  actual reboot; `clearNvsFn_` is just the NVS wipe.
