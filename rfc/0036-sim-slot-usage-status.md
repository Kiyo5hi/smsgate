---
status: implemented
created: 2026-04-10
updated: 2026-04-10
---

# RFC-0036: SIM slot usage in `/status`

## Motivation

Concat SMS fragments that haven't been fully assembled (e.g. one part arrived, others
haven't) stay on the SIM card. If enough fragments accumulate the SIM fills up and new
SMS can't arrive. Currently the user has no way to see how many SIM slots are in use
without serial access. `AT+CPMS?` surfaces this instantly.

## Plan

**`src/main.cpp`**

Add two file-scope statics (refreshed every 30s alongside CSQ + AT+COPS?):
```cpp
static int cachedSimUsed  = -1; // -1 = not yet queried
static int cachedSimTotal = 0;
```

In the 30s refresh block, add:
```cpp
{ String r; modem.sendAT("+CPMS?"); modem.waitResponse(2000UL, r);
  int smPos = r.indexOf('"');
  if (smPos >= 0) {
    int c1 = r.indexOf(',', smPos); int c2 = r.indexOf(',', c1+1);
    if (c1 >= 0 && c2 > c1) {
      cachedSimUsed  = r.substring(c1+1, c2).toInt();
      int c3 = r.indexOf(',', c2+1); if (c3<0) c3=r.length();
      cachedSimTotal = r.substring(c2+1, c3).toInt();
    }
  }
}
```

Same block runs at boot to prime the value. In statusFn SMS section:
```
SIM: 3/20 slots
```

## Notes for handover

Only `src/main.cpp` changed. No test changes needed.
