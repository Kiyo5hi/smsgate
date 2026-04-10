---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0103 — /ussd command (USSD relay)

## Motivation

Carrier services (balance check, data quota, top-up confirmation) are
typically triggered via USSD codes such as `*100#` or `*101*1#`. Currently
there is no way to invoke these from Telegram without physical access to the
SIM. A `/ussd <code>` command relays the code to the modem and returns the
carrier's text response, turning the bridge into a USSD terminal.

## Plan

1. Add `virtual String ussdQuery(const String &code, uint32_t timeoutMs)` to
   `IModem`. Returns the carrier's USSD response text on success, or an empty
   string on failure. The method sends `AT+CUSD=1,<code>,15` and waits for a
   `+CUSD:` URC in the response, then extracts the quoted text field.

2. Implement in `RealModem` (`real_modem.h`): call `sendAT` with
   `"+CUSD=1," + code + ",15"`, then `waitResponse(timeout, "+CUSD:")`.
   If the response contains `+CUSD:`, parse out the second field (the quoted
   text string) and return it. Return empty string on timeout or error.

3. Add a stub to `FakeModem` with a `queueUssdResponse(text)` helper and a
   default of returning empty string (timeout / unsupported).

4. Add a `/ussd <code>` handler in `TelegramPoller::processUpdate`:
   - Extract the USSD code from the command argument.
   - Basic sanity check: code must match `[*#0-9*#]+` (digits, * and # only).
   - Call `modem_.ussdQuery(code, 8000)`.
   - Reply with the response text or an error if empty / timed out.
   - Add a `setModemFn` or direct modem reference… actually, wire it via a
     new `setUssdFn(std::function<String(const String&)>)` setter on
     TelegramPoller, so the interface stays mockable without touching IModem.

5. Update `/help` in TelegramPoller.
6. Update `telegram.cpp` registered commands list.
7. Add native tests for the handler.

## Notes for handover

Using a `setUssdFn` setter (rather than adding `ussdQuery` to IModem) keeps
the USSD path entirely out of the IModem interface change, which avoids
touching fake_modem.h just for a bot command. The setter default is nullptr;
if unset the command replies "(USSD not configured)". Production wires it in
`main.cpp` as a lambda calling `realModem.ussdQuery(...)`.

USSD codes on A76xx: `AT+CUSD=1,*100#,15` typically returns a synchronous
`+CUSD: 0,"<text>",15` followed by `OK`. The response arrives within 2–8s
depending on carrier.
