---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0078: Phone number normalization in /send

## Motivation

Users type phone numbers in many formats:
- `07911 123456` (national with space)
- `+44 7911-123 456` (international with dashes and spaces)
- `(+44)7911123456` (parentheses around country code)

The modem's `AT+CMGS` expects a clean E.164 string like `+447911123456`.
Passing the raw user input causes "CMS ERROR 330" (invalid destination
address) silently — the user sees only the failure message.

## Plan

Add `sms_codec::normalizePhoneNumber(const String&) -> String`:
- Remove all whitespace, dashes, and parentheses.
- Leave `+` and digits intact.
- If the result starts with `00`, replace with `+` (ITU prefix variant).
- Applied in `TelegramPoller::processUpdate` just after extracting
  `phone` from the `/send` command argument.

## Changes

**`src/sms_codec.h`**:
- Declare `String normalizePhoneNumber(const String &raw);`

**`src/sms_codec.cpp`**:
- Implement: iterate chars, keep `+` and digits, skip all others.
  Apply `00` → `+` substitution at start.

**`src/telegram_poller.cpp`**:
- After `String phone = arg.substring(0, spacePos);`, add:
  `phone = sms_codec::normalizePhoneNumber(phone);`

**`test/test_native/test_sms_codec.cpp`** (or equivalent):
- Add tests for space/dash/paren stripping and `00` → `+` conversion.

## Notes for handover

Changed: `src/sms_codec.{h,cpp}`, `src/telegram_poller.cpp`,
`test/test_native/test_main.cpp`, `rfc/0078-phone-normalization.md`.
