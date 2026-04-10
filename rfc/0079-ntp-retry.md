---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0079: Periodic NTP retry when clock is invalid

## Motivation

If NTP fails at boot (pool.ntp.org unreachable, DNS failure, or WiFi
connects but TCP is filtered) the clock stays at epoch. All timestamps
in forwarded SMS, /time responses, and smsDebugLog entries show
"(no NTP)". The bridge currently makes no further attempt to sync the
clock until the operator manually sends `/ntp`.

Adding a periodic retry in the 30-second loop (checked every 5 minutes
when the clock is still invalid) recovers clock sync automatically once
network conditions improve, without operator intervention.

## Plan

**`src/main.cpp`**:
- Add `static uint32_t s_lastNtpRetryMs = 0;` alongside other
  periodic-check statics.
- `static constexpr uint32_t kNtpRetryIntervalMs = 5UL * 60UL * 1000UL;` (5 min).
- In the 30-second block, after the heap checks:
  ```cpp
  if (time(nullptr) <= 8 * 3600 * 2) {
      if (millis() - s_lastNtpRetryMs >= kNtpRetryIntervalMs) {
          s_lastNtpRetryMs = millis();
          syncTime();
          if (time(nullptr) > 8 * 3600 * 2) {
              realBot.sendMessage("🕐 Clock synced via NTP.");
          }
      }
  }
  ```
  Only fires when WiFi transport is active (NTP needs internet access).

## Notes for handover

Changed: `src/main.cpp`, `rfc/0079-ntp-retry.md`.
