---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0251: Fix /simstatus +CREG stat=4 and stat=6 display

## Motivation

The `setSimStatusFn` lambda (`/simstatus` command) issues a live
`AT+CREG?` query and renders the stat value via a `switch` statement.
The switch had cases for stat values 0, 1, 2, 3, and 5 but was missing:

- **stat=4** (`REG_UNKNOWN`): "state not determined" — rare but valid.
  Falling through to the default "unknown" label was technically correct
  but not descriptive.
- **stat=6** (`REG_SMS_ONLY`): "SMS-only service" — a legitimate
  registration state (introduced in 3GPP TS 27.007) that should display
  as "sms-only service", not "unknown". Displaying it as "unknown" would
  mislead operators into thinking registration had failed.

## Plan

Add the two missing cases to the switch in `setSimStatusFn`:

```cpp
case 4: regDesc = "unknown (not determined)"; break;
case 6: regDesc = "sms-only service"; break;
```

No other changes needed — the live AT query path is read-only and
`cachedRegStatus` is not involved here.

## Notes for handover

This is a display-only correctness fix. The main `cachedRegStatus`
update path (RFC-0247 `handleCregUrc()`) already handled stat=4 and
stat=6 correctly. This aligns the `/simstatus` live output with the
same state labels used everywhere else.
