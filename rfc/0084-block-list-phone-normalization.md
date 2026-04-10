---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0084: Phone normalization for /block and /unblock

## Motivation

RFC-0078 normalized phone numbers in `/send`. The same problem exists
for `/block` and `/unblock`: if the user types `/block +44 7911-123456`,
the number is stored with spaces, but incoming SMS arrive as
"+447911123456" — so the block rule never fires. Normalizing the number
on entry ensures consistent matching.

Wildcard entries (e.g. `/block +44*`) must preserve the trailing `*`.

## Plan

**`src/main.cpp`**:
- In the `SmsBlockMutatorFn` lambda, in both the `"block"` and
  `"unblock"` branches, normalize the `number` argument before use:
  ```cpp
  String normedNumber = number;
  if (normedNumber.endsWith("*")) {
      String prefix = sms_codec::normalizePhoneNumber(normedNumber.substring(0, normedNumber.length() - 1));
      normedNumber = prefix + "*";
  } else {
      normedNumber = sms_codec::normalizePhoneNumber(normedNumber);
  }
  ```
  Then use `normedNumber` instead of `number` for all storage/comparison
  operations in those branches.

## Notes for handover

Changed: `src/main.cpp`, `rfc/0084-block-list-phone-normalization.md`.

No new tests — the underlying `normalizePhoneNumber` is already tested
(RFC-0078); wiring it here is straightforward.
