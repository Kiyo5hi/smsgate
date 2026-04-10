---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0187: `/testfmt <phone> <body>` — custom format preview

## Motivation

`/fwdtest` (RFC-0181) previews the forwarded SMS format with a fixed
test sender (+10000000000) and body. Users setting up aliases want to
verify that a specific phone number renders with the correct alias, or
that a particular body (e.g. an emoji, long text) wraps correctly.

## Plan

### Command: `/testfmt <phone> <body>`

Parses the first whitespace-delimited token as `phone`, the remainder
as `body`. Both are passed to `SmsHandler::previewFormat(phone, ts, body)`
with a PDU timestamp synthesised from the current wall clock (identical
to `/fwdtest`). Requires `fwdTestFn_` — if unset, replies
"(fwdtest not configured)".

Rather than adding a separate setter, the existing `fwdTestFn_` is
extended: the new `setFwdTestFn2` takes a `std::function<String(const
String&, const String&)>` (phone, body) variant. The no-arg `/fwdtest`
path continues to call the no-arg fn. The `/testfmt` path calls the
two-arg fn.

### New setter

```cpp
void setFwdTestPhoneBodyFn(std::function<String(const String &, const String &)> fn);
```

### main.cpp wiring

```cpp
telegramPoller->setFwdTestPhoneBodyFn([&smsHandler](const String &phone, const String &body) -> String {
    time_t now = time(nullptr);
    struct tm *t = gmtime(&now);
    char tsBuf[22];
    snprintf(tsBuf, sizeof(tsBuf), "%02d/%02d/%02d,%02d:%02d:%02d+32", ...);
    return smsHandler.previewFormat(phone, String(tsBuf), body);
});
```

### Example interaction

```
/testfmt +13800138000 Hello, can you call me back?
```
Reply:
```
🔍 Format preview:
alice (+13800138000) | 2026-04-10T14:32:00+08:00
-----
Hello, can you call me back?
```

## Notes for handover

- If `phone` is empty or body is empty, reply with usage.
- The fn is in `main.cpp` — no new state needed in `SmsHandler`.
- `/testfmt` appears in `/help` under the aliases section since it's
  most useful for verifying alias display.
