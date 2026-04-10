---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0023: /restart bot command (admin-only soft reboot)

## Motivation

The only ways to reboot the bridge today are: wait for the hardware watchdog
(120s), serial monitor, or physical power-cycle. With RFC-0022's rich boot
banner now showing reset reason and NVS recovery stats, a `/restart` bot command
lets the admin trigger a clean soft reboot remotely and immediately see the
result in Telegram — without physical access to the board.

Use cases: clear transient TLS/WiFi state, apply a new block list entry without
waiting for the watchdog, or verify NVS recovery after a suspected corruption.

## Plan

### Dispatch in `TelegramPoller::processUpdate`

Add after the `/removeuser` block, before the new block commands:

```cpp
if (lower == "/restart")
{
    if (!mutator_)
    {
        bot_.sendMessageTo(u.chatId, String("Restart not configured."));
        return;
    }
    String reason;
    bool ok = mutator_(u.fromId, String("restart"), 0, reason);
    bot_.sendMessageTo(u.chatId, reason);
    (void)ok;
    return;
}
```

### Lambda extension in `main.cpp`

Inside the `ListMutatorFn` lambda, add handling for `cmd == "restart"`:

```cpp
if (cmd == "restart")
{
    if (!isAdmin)
    {
        reason = String("Admin access required.");
        return false;
    }
    reason = String("\xF0\x9F\x94\x84 Restarting..."); // U+1F504
    s_pendingRestart = true;
    return true;
}
```

### Deferred restart flag

Add file-scope static in `main.cpp`:
```cpp
static bool s_pendingRestart = false;
```

In `loop()`, near the top (after `esp_task_wdt_reset()`):
```cpp
if (s_pendingRestart)
{
    Serial.println("/restart command received, rebooting...");
    delay(500); // let serial flush
    ESP.restart();
}
```

## Notes for handover

- The restart is deferred to `loop()` so `bot_.sendMessageTo("Restarting...")`
  completes before the ESP resets.
- Admin-only: same admin check as `/adduser` / `/removeuser` (callerId in
  compile-time `allowedIds[]`).
- No new constructor parameter: reuses the existing `ListMutatorFn` with
  `cmd == "restart"` and `targetId == 0`.
- No new tests required in native env: the `/restart` dispatch path is
  structurally identical to `/listusers` and is already covered by the
  `mutator_` dispatch tests. The flag check in `loop()` is too
  hardware-coupled to unit-test.
