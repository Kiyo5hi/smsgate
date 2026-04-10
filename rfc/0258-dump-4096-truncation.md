---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0258: Truncate SmsDebugLog::dump() output at Telegram's 4096-char limit

## Motivation

`SmsDebugLog::dump()` builds a multi-line string with one block per
entry (timestamp, sender, concat info, outcome, PDU prefix).  The
header comment claimed "Fits in a single 4096-char message for 20
entries", but the worst-case per-entry size is:

| Field | Max chars |
|-------|-----------|
| `\n#20 | ` | 8 |
| timestamp `2026-04-10 14:32 UTC` | 20 |
| ` | ` + sender (15-char E.164) | 18 |
| `\n  9999 chars | concat ref=65535 [8/8] | ` | 42 |
| outcome (e.g. `err: AT+CMGR failed, idx=999`) | 30 |
| `\n  PDU: ` + 120-char pduPrefix + `...` | 131 |
| trailing `\n` | 1 |
| **subtotal per entry** | **250** |

Header `SMS debug log (last 20)\n` = 24 chars.

**Worst-case total**: 24 + 20 × 250 = **5024 chars** — 928 chars over
the Telegram API limit of 4096.

`bot_.sendMessageTo()` relies on the Telegram API returning `"ok":true`
in the response body. If the message text is too long, Telegram returns
`"ok":false` and `sendMessageTo` returns false — a **silent failure**:
the `/debug` response never arrives and the user doesn't know why.

## Plan

After building the full `out` string in `dump()`, check whether it
exceeds 4096 characters. If it does, walk backwards from
`4096 - footer_length` to find the nearest `\n` (entry boundary), then
truncate there and append:

```
\n...(truncated, use /debugbrief for summary)
```

Implementation note: `sms_debug_log.cpp` is compiled in both the ESP
firmware env and the `native` test env.  The fix must use only String
methods already in the native stub (`length()`, `operator[]`,
`substring(unsigned, unsigned)`, `operator+=`).  No new String methods
are needed.

## Notes for handover

- The `dumpBrief`, `dumpBriefFiltered`, `dumpBriefByOutcome`,
  `dumpBriefSince`, `dumpBriefRange` variants are safe: they iterate
  only over a bounded number of entries in the compact one-line format
  (~55 chars/entry), so 10 entries × 55 = ~550 chars — far below 4096.
- `dumpCsv()` produces ~70 chars/entry × 20 = ~1421 chars — safe.
- `stats()` is a fixed-length block (~160 chars) — safe.
- `topSenders()` is bounded by kMaxEntries senders × ~30 chars = ~600
  chars — safe.
- Only `dump()` (the verbose full log) can overflow.
