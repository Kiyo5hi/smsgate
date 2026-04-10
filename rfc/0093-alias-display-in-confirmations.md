---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0093: Alias name in /send and /test confirmations

## Motivation

When the operator types `/send @alice Hello`, the confirmation currently says
"✅ Queued to +447911123456: Hello..." — the alias name is lost. Showing
"✅ Queued to @alice (+447911123456): Hello..." is friendlier and lets the
operator confirm the alias expanded correctly.

## Design

In `/send`: when `rawPhone` starts with `@`, use
`@name (+resolved_phone)` as the display string in the "Queued to" confirmation
and the Serial log. The E.164 phone stays in `capturedPhone` for routing.

Same treatment in `/test`: "📤 Test SMS queued to @alice (+447911123456)".

## File changes

**`src/telegram_poller.cpp`** — display string change in /send and /test
