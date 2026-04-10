---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0262: Increase pollUpdates JSON document from 4096 → 16384 bytes

## Motivation

`RealBotClient::pollUpdates()` parses the getUpdates JSON response into a
`DynamicJsonDocument doc(4096)` with a field filter.  The comment says "we
only keep ~5 ints + 1 string per entry" — but "1 string" is the `text`
field, which Telegram allows up to 4096 UTF-16 code units (~4096 bytes
ASCII or ~16 KB UTF-8 max).

After RFC-0261, `/schedulesend` accepts bodies up to 1530 chars, so the
`text` field for a scheduled SMS command is ~1550 bytes.  With `limit=10`
and 3+ such updates pending, the filtered document exceeds 4096 bytes and
`deserializeJson` returns `DeserializationError::NoMemory`.

The error path returns `false` without advancing `lastUpdateId_`, so the
same 10 updates are re-fetched on every tick.  If all 10 have max-length
texts the bridge is permanently stuck — none of those updates will ever be
processed.

## Plan

Increase `DynamicJsonDocument doc(4096)` to `DynamicJsonDocument doc(16384)`
(16 KB).

### Why 16 KB?

- Each update's filtered data ≈ 400 bytes overhead + text bytes
- Worst realistic case: 10 updates × 1600 bytes each ≈ 16 000 bytes
- 16 384 bytes (next power of two above 16 000) fits the full limit=10
  batch even when all messages are near the 1530-char cap from RFC-0261

### Heap impact

`DynamicJsonDocument` allocates from the heap for the duration of the
parse call only (~1–2 ms in practice), then the local variable is
destroyed and the memory is released.  Peak allocation during
`pollUpdates()` was already dominated by the `body` String (up to 16 384
bytes with `target = min(contentLength, 16384)`).  Adding 12 KB more
(from 4 → 16 KB document) increases peak by ≤12 KB; with ≥80 KB
normally available heap this is safe.

## Notes for handover

- The only change is the document capacity constant; no logic changes.
- The low-heap warning (RFC-0066, threshold 15 KB) remains a safety net:
  if free heap is already low, the allocation may fail and ArduinoJson
  returns `NoMemory` — same as before this fix, but now only happens
  under genuine memory pressure rather than from ordinary-length texts.
