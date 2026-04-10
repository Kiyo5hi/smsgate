---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0132 — /exportaliases command

## Motivation

The operator wants to back up or migrate phone aliases from one device
to another. `/exportaliases` outputs all aliases as `name=number` lines,
suitable for copy-paste or re-import.

## Plan

1. Wire `/exportaliases` in `TelegramPoller`. When `aliasStore_` is set,
   outputs all aliases as `name=+number\n` lines. When not set, replies
   "(alias store not configured)". When empty, replies "(no aliases)".

2. No new setter needed — reuses the existing `setAliasStore` path.

3. Tests:
   - `/exportaliases` with aliases → replies with "name=number" lines.
   - `/exportaliases` with empty store → "(no aliases)".
   - `/exportaliases` without store → "(alias store not configured)".
