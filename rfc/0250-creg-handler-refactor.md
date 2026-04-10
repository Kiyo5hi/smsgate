---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0250: Refactor +CREG: URC handler into a shared helper function

## Motivation

The `+CREG:` stat-parsing and RFC-0082 alert logic introduced by RFC-0247
was replicated verbatim in three dispatch paths:

1. The main URC drain loop (`while (SerialAT.available())`)
2. The RFC-0236 piggybacked-URC scan block (30 s status refresh AT responses)
3. The RFC-0239 piggybacked-URC scan block (modem health check AT response)

Each copy was ~35 lines, totalling ~105 lines of near-identical code.
Divergence between copies under future maintenance is a correctness risk —
any bug fix or enhancement would need to be applied in three places.

## Plan

Extract the common logic into a file-static helper:

```cpp
static void handleCregUrc(const String &line, const char *logTag);
```

The helper:
- Parses both unsolicited `+CREG: <stat>` (mode 1) and solicited
  `+CREG: <n>,<stat>` formats via the existing comma-index logic.
- Updates `cachedRegStatus` via the full stat→RegStatus switch.
- Fires the RFC-0082 registration-lost / registration-restored alert
  via `s_regLostAlertSent` (idempotent guard, safe to call from
  multiple dispatch sites).
- Logs `[<logTag>] +CREG URC: stat=N (desc)` for diagnostics.

The three inline blocks are replaced with single-line calls:
```cpp
handleCregUrc(line, "RFC-0247");       // drain loop
handleCregUrc(line, "RFC-0247/236");   // RFC-0236 piggyback
handleCregUrc(l239, "RFC-0247/239");   // RFC-0239 piggyback
```

Net reduction: ~70 lines of duplicate code removed.

## Notes for handover

The helper is placed immediately before `setup()`, after the
`resetReasonStr()` helper, so it can be called from both `setup()` and
`loop()` if needed in the future. No interface changes to any class.
