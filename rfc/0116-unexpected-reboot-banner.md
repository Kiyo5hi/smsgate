---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0116 — Unexpected reboot banner differentiation

## Motivation

The current startup banner uses "🚀 Bridge online" for all reboot types.
An operator receiving this message can't immediately tell whether the
device restarted because they sent `/reboot`, because of a power cycle,
or because the hardware watchdog fired (indicating a real problem). The
reset reason IS shown inside the full `/status` block that follows, but
it's buried and easy to miss.

## Plan

Change the boot banner header emoji and text based on `s_resetReason`:

| Reason         | Header                                        |
|----------------|-----------------------------------------------|
| Power-on       | "🚀 Bridge online\n"                          |
| Software reset | "🔄 Bridge restarted\n"                       |
| Watchdog (WDT) | "⚠️ Bridge restarted (watchdog timeout!)\n"   |
| Panic          | "🚨 Bridge restarted (panic/exception!)\n"    |
| Brownout       | "⚡ Bridge restarted (brownout!)\n"           |
| Other          | "🔄 Bridge restarted\n"                       |

All variants still append the full `statusFn()` output so operators
can see the complete health snapshot.

## File changes

**`src/main.cpp`** — modify the boot banner string construction in
`setup()` before calling `realBot.sendMessage(bootMsg)`.

## Notes for handover

`s_resetReason` is captured via `esp_reset_reason()` in `setup()` at
RFC-0020. No new variables needed. No tests (main.cpp-only glue).
