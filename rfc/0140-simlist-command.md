---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0140: /simlist command — list all SMS stored in SIM

## Motivation

After a partial outage there may be SMS stuck in SIM slots (concat
fragments waiting for their counterparts, or messages that failed to
forward). Currently there is no way to see what is stored in the SIM
from Telegram.

## Plan

Add `setSimListFn(std::function<String()>)` setter to `TelegramPoller`.
The fn issues AT+CMGL="ALL" (PDU mode) and formats a compact list:
  idx: <sender> (<chars>c)
Returns "(no SMS in SIM)" when the list is empty.

In `main.cpp` wire it via `realModem.sendAT("+CMGL=\"ALL\"")` and
parse the CMGL response lines.

## Notes for handover

In PDU mode `AT+CMGL` returns lines like:
  +CMGL: <idx>,<status>,,[<len>]
  <PDU hex>
Parse only the +CMGL: lines to get index and status; skip the PDU hex
for the listing (full content is accessible via `/simread <idx>`).
