---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0045: Modem firmware version in `/status`

## Motivation

When troubleshooting modem behaviour it's useful to know the firmware version
without requiring a serial connection. `AT+CGMR` returns the firmware string;
querying it once at boot and caching it is essentially free.

## Plan

**`src/main.cpp`**

Add file-scope static:
```cpp
static String cachedModemFirmware;
```

In the boot prime block (after operator name prime), query `AT+CGMR`:
```cpp
modem.sendAT("+CGMR");
modem.waitResponse(1000UL, cachedModemFirmware);
cachedModemFirmware.trim();
if (cachedModemFirmware.startsWith("+CGMR: "))
    cachedModemFirmware = cachedModemFirmware.substring(7);
cachedModemFirmware.trim();
```

In statusFn, append after the modem CSQ / operator line:
```cpp
if (cachedModemFirmware.length() > 0) {
    msg += "\n  FW: " + cachedModemFirmware;
}
```

Result: `Modem: CSQ 18 (good)  home (China Mobile)\n  FW: A7670GR01A01V01`

## Notes for handover

Only `src/main.cpp` changed. No test changes needed.
