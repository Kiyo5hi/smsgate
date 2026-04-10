---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0092: /csq command

## Motivation

`/status` is comprehensive but long. When the operator just wants to check
signal strength quickly, a lightweight `/csq` command is more convenient:
one or two lines showing CSQ, registration, and operator.

## Design

`/csq` replies with a compact string like:

    📶 CSQ 18 (good) | home (T-Mobile) | WiFi -65 dBm

Uses the already-cached `cachedCsq`, `cachedRegStatus`, `cachedOperatorName`
from the `statusFn` closure in main.cpp — same data, no new AT commands.

Since TelegramPoller doesn't have direct access to these modem values (they
live in main.cpp file-scope statics), use the same `StatusFn` pattern: add
a new `CsqFn` setter, or reuse the existing `StatusFn` approach by giving
the CsqFn a dedicated lambda.

## Implementation

- Add `void setCsqFn(std::function<String()> fn)` to TelegramPoller.
- Add `/csq` handler in processUpdate.
- Wire in main.cpp with a compact lambda using the cached modem values.
- Register command in telegram.cpp.

## File changes

**`src/telegram_poller.h`** — add setCsqFn setter + private member  
**`src/telegram_poller.cpp`** — add /csq handler, add to /help  
**`src/main.cpp`** — wire setCsqFn lambda  
**`src/telegram.cpp`** — register command  
**`test/test_native/test_telegram_poller.cpp`** — test /csq command
