---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0083: SIM PIN unlock support

## Motivation

SIMs can require a PIN code to become active. Without PIN unlock the
modem boots, returns `+CPIN: SIM PIN`, and never registers — causing
silent failure (no SMS forwarding, no Telegram alerts). The operator
has no diagnostic path short of attaching a terminal and typing AT+CPIN=.

## Plan

**`src/secrets.h.example`**:
- Add optional `// #define SIM_PIN "1234"` entry (commented out by default).

**`src/main.cpp`**:
- After "Wait SMS Done." and before the NETWORK_APN block, add an
  `#ifdef SIM_PIN` block:
  - Call `modem.getSimStatus()`.
  - If status != 3 (not ready), call `modem.simUnlock(SIM_PIN)`.
  - Log "SIM PIN accepted." / "SIM PIN rejected!".
  - No Telegram notification here — the bot transport isn't set up yet.

## Design choices

- Feature-gated via `#ifdef SIM_PIN` so builds without SIM_PIN defined
  incur zero overhead (no AT traffic, no binary size increase).
- Error is logged to Serial only (Telegram transport not yet available
  at this point in setup()).
- SIM_PUK (PUK unlock for blocked SIMs) is out of scope — PUK entry is
  destructive and should not be automated.

## Notes for handover

Changed: `src/main.cpp`, `src/secrets.h.example`,
`rfc/0083-sim-pin-support.md`.
