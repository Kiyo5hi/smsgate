---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0052: Prefix-wildcard SMS blocking

## Motivation

Exact-match blocking is insufficient against spam campaigns that use many
numbers sharing a common prefix (e.g. all numbers starting with `+8613100`).
The user had to add each number individually. A `*` wildcard suffix allows
blocking an entire range with one entry.

## Plan

**`src/sms_block_list.h`** — extend `isBlocked()`:
```cpp
if (entry[entryLen - 1] == '*')
    return strncmp(number, entry, entryLen - 1) == 0; // prefix match
else
    return strcmp(number, entry) == 0;                // exact match (unchanged)
```

**`src/telegram_poller.cpp`** — update `/block` usage hint and confirmation:
- Usage: `/block <number>  (append * for prefix: /block +8613*)`
- Confirmation distinguishes exact vs prefix: "Prefix match — all numbers
  starting with +8613 will be blocked."

## Example

```
/block +8613*    → blocks +8613, +86130001234, +86138888888, ...
/block +8613     → blocks only the exact number +8613
```

## Notes for handover

`sms_block_list.h`, `telegram_poller.cpp` changed.
Tests in `test_sms_block_list.cpp`: `test_isBlocked_wildcard_prefix_matches_number`,
`test_isBlocked_wildcard_does_not_match_shorter_number`,
`test_isBlocked_exact_entry_not_mistaken_for_wildcard`.
