---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0022: Rich startup notification with key stats and reboot reason

## Motivation

When the bridge reboots — whether from a hardware watchdog timeout, a power
cycle, a `ESP.restart()` after consecutive Telegram failures, or an OTA
flash — the only Telegram notification the user receives is:

```
🚀 Modem SMS to Telegram Bridge is now online!
```

This static string provides no actionable information. The user cannot tell:

- Why it rebooted (watchdog? power-cycle? software restart after 8 consecutive
  Telegram failures?).
- Whether NVS state recovered correctly (how many reply-target slots are live?
  how many SMS debug log entries survived?).
- Whether the user list or block list is populated (did the runtime config
  survive the reboot?).

After an unexpected reboot, the operator must send `/status` manually to get
this information. But `/status` is user-initiated and requires knowing the bot
is up.

All the data needed for a rich boot banner already exists at the point in
`setup()` where `sendMessage` is called:

- `s_resetReason` — captured at the top of `setup()` via `esp_reset_reason()`.
- `smsDebugLog.count()` — populated by `loadFrom(realPersist)` moments earlier.
- `replyTargets.occupiedSlots()` — populated by `replyTargets.load()`.
- `allowedIdCount + runtimeIdCount` — set earlier in `setup()`.
- `sBlockListCount + sRuntimeBlockListCount` — set earlier in `setup()`.

The `statusFn` lambda (assembled in `setup()`) already formats all of this.
The boot banner can reuse it directly.

## Current state

### Boot banner call site (`main.cpp`)

```cpp
realBot.sendMessage("🚀 Modem SMS to Telegram Bridge is now online!");
```

Called after NVS loaded, `statusFn` assigned, `telegramPoller` constructed.
The `statusFn` static is therefore fully initialized and callable at this point.

### Gap: user count and block list count not in `/status`

`statusFn` currently does not report user or block list counts. These should be
added to both the boot banner and `/status` simultaneously — the same data is
useful both at boot and on demand.

### `cachedCsq` at boot

`cachedCsq` is populated in `loop()` every 30 seconds. At the banner call site
in `setup()`, it is still zero. The boot banner can prime it with one
`modem.getSignalQuality()` call immediately before `sendMessage`. The modem is
idle at this point (post-registration, pre-`sweepExistingSms`).

## Plan

### 1. Add configuration section to `statusFn`

Inside the `statusFn` lambda in `main.cpp`, after the `--- Debug Log ---`
section, add:

```cpp
msg += "\n";
msg += "--- Configuration ---\n";
msg += "Users (compile+runtime): ";
msg += String(allowedIdCount + runtimeIdCount);
msg += "\n";
msg += "Block list (compile+runtime): ";
msg += String(sBlockListCount + sRuntimeBlockListCount);
msg += "\n";
```

`allowedIdCount`, `runtimeIdCount`, `sBlockListCount`, and
`sRuntimeBlockListCount` are all file-scope statics — safe to capture
implicitly by reference, same as the rest of the lambda.

### 2. Prime `cachedCsq` before the boot banner

Immediately before the `realBot.sendMessage(...)` boot banner call:

```cpp
// Prime cachedCsq so the boot banner shows a real signal quality value.
// The modem is idle here (post-registration, pre-sweepExistingSms).
cachedCsq = modem.getSignalQuality();
```

### 3. Replace the static boot banner with `statusFn` output

```cpp
// NEW: Rich boot notification. statusFn is assigned above; all NVS state loaded.
{
    String bootMsg = String("\xF0\x9F\x9A\x80 Bridge online\n"); // U+1F680 rocket
    if (statusFn)
        bootMsg += statusFn();
    else
        bootMsg += "(status not available)";
    realBot.sendMessage(bootMsg);
}
```

The resulting boot message:

```
🚀 Bridge online
--- Device Status ---
Uptime: 0d 0h 0m
WiFi RSSI: -65 dBm
Modem CSQ: 17 (good)
Registration: home
Free heap: 148240 bytes
Reboot reason: watchdog

--- SMS Stats ---
Forwarded: 0
Failed: 0
Consecutive failures: 0
Concat groups in flight: 0

--- Telegram ---
Reply-target slots: 47/200
Poll attempts: 0
Last update_id: 928374112

--- Debug Log ---
Entries: 10/20

--- Configuration ---
Users (compile+runtime): 2
Block list (compile+runtime): 3
```

### 4. Extend `registerBotCommands` to all 8 commands

`registerBotCommands()` in `telegram.cpp` currently registers only `/debug` and
`/status`. Extend to register all 8 commands. Increase `DynamicJsonDocument`
capacity from 384 to 768 to accommodate the larger JSON array.

| command | description |
|---------|-------------|
| `debug` | Show SMS diagnostic log |
| `status` | Show device health and stats |
| `listusers` | List authorized Telegram users |
| `adduser` | Add a Telegram user (admin only) |
| `removeuser` | Remove a Telegram user (admin only) |
| `blocklist` | Show SMS sender block lists |
| `block` | Block an SMS sender (admin only) |
| `unblock` | Unblock an SMS sender (admin only) |

Update the success log line to list all 8 commands.

**Note:** Telegram's bot menu IS the canonical `/help` — typing `/` shows all
registered commands with descriptions. A separate `/help` text command would
duplicate this with no added value.

## Notes for handover

### Files changed

1. **`src/main.cpp`** — three changes:
   - Add `--- Configuration ---` section to `statusFn` lambda.
   - Add `cachedCsq = modem.getSignalQuality();` before the boot banner.
   - Replace static `sendMessage` with `statusFn`-backed version.

2. **`src/telegram.cpp`** — two changes:
   - Increase `DynamicJsonDocument` from 384 to 768.
   - Add 6 new command entries and update the success log line.

### `sweepExistingSms()` ordering

`sweepExistingSms()` is called *after* the boot banner call — no change needed.
The banner announces readiness, then the sweep processes offline-arrived SMS.

### Watchdog safety

The watchdog is armed (`esp_task_wdt_add(NULL)`) *after* `sendMessage` in
`setup()`. No `esp_task_wdt_reset()` needed around the boot banner.

### Test approach

No new native tests required. The changed code lives in `main.cpp` and
`telegram.cpp`, both excluded from the native build by `build_src_filter`.

**Manual hardware test plan:**
1. Flash. Check Telegram: first message starts with `🚀 Bridge online` followed
   by all status sections.
2. Verify `Reboot reason:` shows correct reason (power-on, software, watchdog).
3. Verify `Reply-target slots:`, `Debug Log Entries:`, `Users:`, `Block list:`
   counts are accurate after a reboot with NVS state present.
4. Send `/status` — output should match the boot banner body (same data minus
   uptime drift).
5. Type `/` in Telegram — all 8 commands appear in the autocomplete menu.

### Message length

The formatted status message is ~450–550 characters. Well under Telegram's
4,096-character single-message limit.
