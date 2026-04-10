---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0190: `/setsmsagefilter <hours>` — skip forwarding of old SMS

## Motivation

First boot with a SIM that has weeks or months of stored SMS causes
`sweepExistingSms` to flood Telegram. Operators want to skip SMS older
than N hours (e.g. "skip anything older than 24 hours") without having
to manually delete them from the SIM first.

## Plan

### `SmsHandler` changes

- New field: `int maxSmsAgeHours_ = 0` (0 = disabled / forward all).
- New setter: `void setMaxSmsAgeHours(int h)` and getter `maxSmsAgeHours()`.
- In `forwardSingle` (single-part) and after concat assembly:
  - If `maxSmsAgeHours_ > 0` and `pdu.timestamp` is non-empty:
    - Parse timestamp via `sms_codec::pduTimestampToUnix(timestamp)`.
    - Compute `ageHours = (time(nullptr) - pduUnix) / 3600`.
    - If `ageHours > maxSmsAgeHours_` → log "skipped (age Xh)" + delete
      from SIM slot, don't forward.
  - If timestamp is empty (PDU had no timestamp), always forward.

### New sms_codec helper

```cpp
// Parse a PDU timestamp string "YY/MM/DD,HH:MM:SS+tz" into Unix epoch.
// Returns 0 if the timestamp cannot be parsed or NTP has not synced.
time_t pduTimestampToUnix(const String &ts);
```

This reuses the existing "YY/MM/DD,HH:MM:SS+tz" format decoded in
`parseCmgrBody`. The timezone offset in the timestamp is used to
convert to UTC before comparison with `time(nullptr)`.

### TelegramPoller command: `/setsmsagefilter <hours>`

- Range: 0–8760 (0 = disable, 8760 = 1 year).
- Replies: "✅ SMS age filter set to 24h." or "✅ Age filter disabled."
- No new fn setter needed — use the existing pattern with a
  `smsAgeFilterFn_` setter.

### Persistence

NVS key `sms_age_h` (int32_t). Load at boot, save on mutation.

## Notes for handover

- If the clock is not synced (NTP unavailable), `time(nullptr) < 1e9`
  — the filter is skipped and all SMS are forwarded regardless.
- The age filter applies to both `handleSmsIndex` (new arrivals) and
  `sweepExistingSms` (boot drain).
- Deleted-as-stale messages appear in the debug log with outcome
  "age skip".
