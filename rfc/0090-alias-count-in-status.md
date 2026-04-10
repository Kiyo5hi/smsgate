---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0090: Alias count in /status

## Motivation

After RFC-0088 the operator can manage up to 10 phone aliases, but `/status`
doesn't show how many are defined. A one-liner in the Config section makes
the current alias inventory visible without running `/aliases`.

## Design

Add to the Config section of the status lambda in `main.cpp`:

    Aliases: N/10

Only show if `smsAliasStore.count() > 0` OR always show — showing always
makes the feature discoverable. Always show.

## File changes

**`src/main.cpp`** — add alias count line in statusFn Config section
