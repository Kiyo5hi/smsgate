---
status: deferred
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0013: Runtime WiFi credential provisioning

## Motivation

`WIFI_SSID` and `WIFI_PASSWORD` are `#define` macros in `src/secrets.h`,
baked into the firmware at compile time. Moving the bridge to a different
WiFi network — a new home, a different AP after a router replacement, a
guest network during travel — requires editing `secrets.h`, rebuilding,
and reflashing over USB. For a device that is meant to run unattended,
that friction is a deployment blocker.

This RFC specifies a provisioning mechanism that lets the owner change
WiFi credentials at runtime, without a laptop or USB cable.

## Current state

`connectToWiFi()` in `main.cpp` (lines 104-115):

```cpp
static const char *ssid     = WIFI_SSID;
static const char *password = WIFI_PASSWORD;

void connectToWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); ... }
}
```

`WIFI_SSID` and `WIFI_PASSWORD` are defined only in `src/secrets.h`
(gitignored). There is no fallback, no runtime override, and no NVS
storage for credentials. The first-time setup section of `CLAUDE.md`
explicitly requires users to edit `secrets.h` and reflash for any
credential change.

`RealPersist` already wraps ESP32 NVS (the `Preferences` library) under
the namespace `"tgsms"` with two keys: `"uid"` (int32, update watermark)
and `"rtm"` (blob, reply-target ring buffer). Extending it to store WiFi
credentials requires a new NVS namespace or two new keys in the existing
namespace.

`IPersist` is a narrow interface sized for the TG->SMS pipeline (RFC-0003).
WiFi credentials are a separate concern; they should live in a separate NVS
namespace rather than extending `IPersist` with unrelated methods.

## Plan

### Option A: ESP32 SmartConfig

`WiFi.beginSmartConfig()` puts the ESP32 into a listen mode where a phone
app (Espressif "ESP-Touch" / "IoT Espressif" on iOS or Android) sends the
SSID and password as a UDP multicast packet that the ESP32 captures and
stores in flash automatically via `WiFi.stopSmartConfig()`.

Pros:
- Zero extra hardware; works on any ESP32.
- Very small code delta in `connectToWiFi()`.

Cons:
- Requires installing a third-party phone app.
- SmartConfig packets are cleartext UDP multicast — any device on the same
  LAN segment can observe the credentials during provisioning.
- Does not work over 5 GHz-only APs (SmartConfig uses 802.11b/g/n 2.4 GHz
  channel sniffing).
- User experience is poor if the target AP and the phone are on different
  subnets.
- No obvious trigger for entering provisioning mode without a serial
  monitor or a reset button.

### Option B: WiFiManager-style AP + captive portal

On failed WiFi connect after N retries, the ESP32 turns itself into a
soft AP broadcasting a known SSID (e.g. `SMS-Bridge-Setup`). The owner
connects a phone to that AP, and a captive portal (tiny HTTP server on
the ESP32) serves a one-page form where they enter the new SSID and
password. On submit, credentials are saved to NVS and the device
restarts.

Pros:
- No phone app required; any browser works.
- Credential exchange happens over the isolated AP — no LAN eavesdroppers.
- Well-understood UX pattern (smart plugs, Shelly devices, etc.).

Cons:
- Requires implementing a minimal HTTP server (or pulling in a library like
  WiFiManager / ESP-IDF httpd), which is a nontrivial code addition on a
  heap-constrained ESP32 (~320 KB free in typical firmware builds here).
- The open AP is a brief attack surface: anyone nearby can connect and
  submit arbitrary credentials during the provisioning window.
- Does not work if the owner is not physically nearby with a WiFi-capable
  device.

### Option C: SMS-based provisioning (recommended)

The bridge already has a working SIM card and can receive SMS even when
WiFi is completely absent — `modem.waitResponse(100000UL, "SMS DONE")` and
the `+CMTI` URC path are established long before `connectToWiFi()` is
called. The owner can send a specially formatted SMS to the device's own
SIM number:

```
WIFI-SSID: MyNewNetwork
WIFI-PASS: hunter2
```

The bridge parses the message, saves the credentials to NVS, and reboots.
On the next boot it reads from NVS before falling back to compiled-in
defaults.

This option is uniquely well-suited to this project: it exploits the
hardware capability that distinguishes this device from a plain ESP32.
The owner already trusts the SMS channel implicitly — they control the
SIM, and inbound SMS is point-to-point (operator-level delivery), not a
shared LAN broadcast.

Pros:
- No phone app, no browser, no laptop required.
- Works regardless of WiFi state — exactly the scenario where
  re-provisioning is needed.
- Credential exchange rides the carrier's SMS infrastructure; not
  observable by LAN neighbours.
- Minimal code delta: reuse the existing URC drain in `loop()` and the
  existing `parseCmgrBody` / `parseSmsPdu` codec path.

Cons:
- SMS is cleartext at the carrier/operator level. Anyone with access to
  the carrier's infrastructure can read the message. This is the same
  trust level as every other SMS the device handles.
- The provisioning command must be authenticated — otherwise any caller
  who knows the SIM number can change the WiFi credentials. Authentication
  is addressed in the Security section below.
- If the bridge is stuck in a boot loop before `sweepExistingSms()` runs,
  the provisioning SMS may not be processed. A dedicated pre-WiFi SMS sweep
  is needed (see Boot flow below).

**Recommendation: implement Option C.** Option A requires a third-party
app and sends credentials as cleartext multicast. Option B is heavier,
adds an attack-surface AP, and requires physical proximity. Option C
exploits the device's unique hardware advantage, requires no auxiliary
tooling, and works at exactly the point where provisioning is needed
(WiFi is down or unknown).

### Credential storage

Add a second NVS namespace `"wifiprov"` with two string keys:

| Key       | Type     | Content               |
|-----------|----------|-----------------------|
| `"ssid"`  | `String` | WiFi network SSID     |
| `"pass"`  | `String` | WiFi network password |

A header-only `WifiCredStore` class (similar in style to `RealPersist`)
wraps these reads/writes. It does not implement `IPersist` — WiFi
credentials are unrelated to the TG->SMS pipeline.

```cpp
// src/wifi_cred_store.h  (new file)
class WifiCredStore {
public:
    bool begin() { return prefs_.begin("wifiprov", false); }

    bool loadCreds(String &ssid, String &pass) const;
    void saveCreds(const String &ssid, const String &pass);
    void clearCreds();

private:
    Preferences prefs_;
};
```

`loadCreds` returns `false` if no credentials have ever been stored (both
keys absent). `saveCreds` uses `putString`. `clearCreds` calls `remove`
on both keys (used if the owner wants to revert to compiled-in defaults).

### Boot flow

Replace the current `connectToWiFi()` body with a prioritised lookup:

1. **NVS first.** Call `wifiCredStore.loadCreds(ssid, pass)`. If it
   returns credentials, attempt `WiFi.begin(ssid, pass)`.
2. **Compiled-in fallback.** If NVS is empty, use `WIFI_SSID` /
   `WIFI_PASSWORD` from `secrets.h` and attempt `WiFi.begin(...)`.
3. **Retry limit.** After `kWifiConnectRetries` (e.g. 20) attempts
   (each 500 ms = 10 s total), give up connecting and enter
   **provisioning sweep mode** (step 4). Do NOT reboot in this path —
   the SIM may have the provisioning SMS waiting and a reboot loop
   would process it on every boot, never reaching WiFi.
4. **Provisioning sweep.** Call `runProvisioningSweep()` — a simplified
   blocking version of the startup SMS sweep that only looks for a
   provisioning message (see format below). If a valid provisioning
   message is found, save creds to NVS and call `ESP.restart()`. If
   not, log the situation and try WiFi again in a loop (or enter a
   low-power wait; see Notes for handover).

The pre-WiFi provisioning sweep runs before `setupTelegramClient()` and
before `sweepExistingSms()`, using only the modem (which is already
initialised at that point in `setup()`).

### Provisioning SMS format

```
WIFI-SSID: <ssid>
WIFI-PASS: <password>
WIFI-AUTH: <pin>
```

- Each key is on its own line; order is flexible.
- `WIFI-AUTH` carries a 6-digit PIN set at compile time via a new
  `WIFI_PROVISION_PIN` define in `secrets.h`. If absent from the message
  or incorrect, the command is rejected.
- Leading/trailing whitespace on values is stripped.
- A message containing all three keys (with correct PIN) is a valid
  provisioning command.
- A message containing only `WIFI-CLEAR: <pin>` clears stored NVS
  credentials and reboots (reverts to compiled-in defaults).

Parsing lives in a new `src/wifi_provisioner.{h,cpp}` TU:

```cpp
// Returns true and populates ssid/pass if msg is a valid provisioning command.
bool parseProvisioningCommand(const String &body, const String &expectedPin,
                              String &ssid, String &pass);
```

This is a pure function with no hardware deps, so it can be covered by
the native test env.

### Handling provisioning SMS at runtime

After `setup()` completes (WiFi is up), the normal `loop()` URC drain
continues to fire. A provisioning SMS arriving at runtime triggers the
standard `+CMTI` path through `SmsHandler::handleSmsIndex`. `SmsHandler`
needs a hook to check whether an incoming message is a provisioning
command before forwarding it to Telegram.

The cleanest approach: `SmsHandler` calls an optional
`ProvisionFn = std::function<bool(const String &body)>` after decoding
each message. If `ProvisionFn` returns `true` (message was consumed as a
provisioning command), `SmsHandler` skips the Telegram forward and
deletes the slot. This keeps provisioning logic out of `SmsHandler` and
lets `main.cpp` wire it up.

```cpp
// sms_handler.h
using ProvisionFn = std::function<bool(const String &body)>;
void setProvisionFn(ProvisionFn fn) { provisionFn_ = std::move(fn); }
```

### `secrets.h.example` additions

```cpp
// 6-digit PIN required in WIFI-AUTH: field of a provisioning SMS.
// Change this to something non-default before flashing.
#define WIFI_PROVISION_PIN "000000"
```

### CLAUDE.md updates

- Move the "First-time setup" step 3 note about editing `secrets.h` for
  WiFi to mention that NVS provisioning is also available.
- Add a "WiFi provisioning" subsection under Architecture describing the
  SMS command format and the boot flow priority order.

## Security considerations

**SMS as a provisioning channel:**
The provisioning SMS is carried over the mobile carrier's infrastructure.
It is plaintext at the operator level — the carrier can read it. In
practice, this is the same trust level as every other SMS passing through
the device, and the same risk accepted when using the device for any
sensitive message forwarding. The `WIFI_PROVISION_PIN` provides a second
factor: an attacker who knows the SIM number but not the PIN cannot
change credentials.

**PIN strength:**
A 6-digit numeric PIN has 10^6 = 1,000,000 combinations. This is not
strong against a targeted attack by a motivated adversary with carrier
access, but it is sufficient against opportunistic SIM-number guessing by
a random actor. Operators who need stronger authentication can use a
longer alphanumeric PIN via the same `WIFI_PROVISION_PIN` define.

**Rate limiting:**
There is no built-in rate limit on provisioning attempts via SMS.
An attacker sending many provisioning SMSes (all with wrong PINs) would
cause the device to process and reject each one, but would not cause a
lockout or DoS beyond the SMS processing cost. Adding a per-boot counter
cap (e.g. reject after 5 failed provisioning attempts until reboot) would
tighten this.

**AP mode (Option B) risks not inherited:**
Option C does not open a soft AP, so there is no provisioning-window
attack surface beyond the existing SMS channel.

**NVS plaintext:**
ESP32 NVS is stored in flash without encryption by default. Physical
access to the board allows reading credentials via JTAG or flash dump.
This is the same risk as the current compile-time `secrets.h` approach
(credentials in firmware binary). Flash encryption (`CONFIG_FLASH_ENCRYPTION_ENABLED`)
mitigates both; it is out of scope for this RFC but worth noting.

## Notes for handover

- **Pre-WiFi SMS sweep ordering matters.** The modem must be fully
  registered (past `modem.waitResponse(100000UL, "SMS DONE")` and the
  registration loop) before the provisioning sweep can read SMS slots.
  That point in `setup()` is already before `connectToWiFi()`. The sweep
  must NOT call `smsHandler.sweepExistingSms()` — that function forwards
  everything to Telegram, which requires WiFi. A separate bare-loop over
  `AT+CMGL=0` (or reusing `handleSmsIndex` with a provisioning-only
  flag) is needed for the pre-WiFi sweep.

- **Avoid modifying `IPersist`.** The `IPersist` interface is sized
  deliberately for the TG->SMS pipeline. WiFi credentials belong in a
  separate NVS namespace and a separate class. Do not add
  `saveWifiCreds` / `loadWifiCreds` to `IPersist` — it would require
  updating `FakePersist` in the test support layer for functionality
  that has no unit-testable interaction with the TG->SMS pipeline.

- **`WifiCredStore` is hardware-only.** Like `RealPersist`, it uses
  `Preferences` and should be excluded from the native build via
  `build_src_filter`. The `parseProvisioningCommand` function in
  `wifi_provisioner.cpp` is the unit-testable piece and should be
  included in the native env.

- **The `kWifiConnectRetries` timeout.** 20 x 500 ms = 10 s is
  intentionally short. If it proves too aggressive in environments with
  slow AP association (e.g. a router that takes >5 s to respond), bump
  it to 40 retries. Do not make it infinite — the whole point is to fall
  through to the provisioning path when the credentials are genuinely
  wrong, not merely slow to associate.

- **Interaction with the RFC-0004 cellular fallback.** RFC-0004 proposes
  switching Telegram transport from WiFi to the modem's GPRS/LTE data
  path. If RFC-0004 is implemented first, WiFi becomes optional and this
  RFC's boot-flow priority changes: the device can come fully online over
  cellular, receive the provisioning SMS through the normal `loop()` path
  (rather than a pre-WiFi sweep), and then reconnect WiFi with the new
  credentials. This would simplify the provisioning flow significantly.
  Consider implementing RFC-0004 before or alongside this RFC.

- **`WIFI_PROVISION_PIN` must be changed from the default `"000000"`.** 
  The example value is intentionally trivial to flag it for the owner.
  Document this prominently in `secrets.h.example` and in the first-time
  setup section of `CLAUDE.md`. A future hardening step could require
  the PIN to be at least 8 characters.

- **Concat SMS provisioning.** The provisioning command (three lines of
  text) is well under 160 GSM-7 chars and will always arrive as a
  single-part SMS. There is no need to handle concat reassembly in the
  provisioning path. If for some reason the command arrives as a concat
  SMS (e.g. a carrier that splits short messages), the existing
  `SmsHandler` concat buffer already assembles it before calling
  `ProvisionFn`.

- **Test coverage targets for the native env:**
  - `parseProvisioningCommand` with correct PIN, wrong PIN, missing PIN,
    missing SSID, missing password, excess whitespace, different line
    orderings, and the `WIFI-CLEAR` variant.
  - `SmsHandler` with a `ProvisionFn` set: verify a matching provisioning
    message is NOT forwarded to Telegram and the slot is deleted; verify a
    non-provisioning message still forwards normally.

## Review

**Reviewer:** claude-sonnet-4-6  
**Date:** 2026-04-09  
**verdict: deferred**

---

### BLOCKING issues

- **The PIN is security theater for the stated threat model.**  
  The RFC claims the PIN guards against "an attacker who knows the SIM
  number but not the PIN." But the only realistic attacker who can send a
  provisioning SMS to this device and have it processed is someone with
  either (a) physical access to the board or (b) carrier-level access to
  the SMS infrastructure. In case (a) they can dump the firmware binary
  with `esptool.py read_flash` in under a minute and extract
  `WIFI_PROVISION_PIN` as a literal string. In case (b) they can read the
  cleartext SMS in transit and see the PIN they need to inject. The PIN
  does not protect against either adversary. The only threat it actually
  raises the bar against is a random actor who knows the SIM's phone
  number and sends provisioning SMSes at random — a threat that barely
  exists in practice. The Security section acknowledges the firmware-binary
  exposure indirectly ("same risk as compile-time `secrets.h`") but then
  still presents the PIN as providing a "second factor," which is
  misleading. Either (1) remove the PIN and be honest that the security
  boundary is physical custody of the device, or (2) document the PIN
  narrowly as "stops opportunistic SIM-number guessing only, not physical
  or carrier-level attacks." The current framing overstates the protection
  and will mislead future maintainers. This is BLOCKING because the
  security model is a core design decision; getting it wrong here shapes
  how the PIN length / complexity guidance is written and how users
  understand the risk.

- **Boot-flow state machine is incomplete — device can hang permanently.**  
  The Plan specifies: if provisioning sweep finds no valid command, "log
  the situation and try WiFi again in a loop (or enter a low-power wait;
  see Notes for handover)." That "or" is the problem. The Notes for
  handover then explicitly defers this decision: "If not, log the
  situation and try WiFi again in a loop." There is no specified terminal
  state for the case where WiFi never comes back AND no provisioning SMS
  ever arrives. The current `connectToWiFi()` is an infinite blocking
  `while` loop (lines 107–113 of `main.cpp`) — the RFC must explicitly
  specify what happens to avoid replacing one infinite hang with a
  different one. The correct answer is probably: after the provisioning
  sweep also finds nothing, loop back to step 1 with increasing backoff and
  keep retrying indefinitely, since the device is useless without
  connectivity. But this must be stated explicitly in the RFC, not left
  as a "see Notes for handover" stub. A state machine that can reach an
  undefined state is a firmware hang waiting to happen.

### NON-BLOCKING issues

- **`ProvisionFn` hook on `SmsHandler` is unnecessary overhead for the
  primary use case.**  
  The hook is motivated by runtime provisioning (WiFi already up, new SMS
  arrives via `+CMTI`). But the RFC itself identifies the primary scenario
  as WiFi-is-down-so-I-need-to-reprovision, which is handled entirely by
  the pre-WiFi `runProvisioningSweep()` path. The runtime path via
  `ProvisionFn` handles a secondary scenario: owner sends a new SSID while
  WiFi is up (maybe they're preparing for a network change). That's useful,
  but it adds complexity to `SmsHandler` — a new callback member, wiring in
  `main.cpp`, a conditional skip of the Telegram forward — for a
  convenience case. More importantly, if WiFi is up, `SmsHandler` WILL try
  to forward the provisioning SMS to Telegram before (or instead of)
  recognising it, leaking `WIFI-PASS:` and the PIN into the owner's
  Telegram chat. The hook prevents that — but it also means the hook is
  load-bearing for security, not just convenience, and needs to be
  documented as such. Consider whether it is simpler to handle runtime
  provisioning at the same pre-WiFi sweep call site via a periodic
  background sweep (every N minutes, rescan for provisioning commands) and
  skip `ProvisionFn` entirely. This is NON-BLOCKING because the hook can be
  added incrementally, but the security implication of leaking credentials
  to Telegram if the hook is absent or mis-wired needs to be called out
  prominently.

- **`parseProvisioningCommand` format is underspecified.**  
  The RFC shows a three-line format but does not specify: maximum SSID
  length (network names up to 32 bytes per 802.11), maximum password
  length (up to 63 bytes for WPA2-Personal), behaviour if the same key
  appears twice, case-sensitivity of keys (`WIFI-SSID:` vs `wifi-ssid:`),
  or what character encoding is assumed (GSM-7 vs UCS-2 — relevant because
  the PDU decoder already handles both). The "flexible order" claim also
  needs to be confirmed against how `parseSmsPdu` delivers the body — it
  assembles a single `String` after decoding, so line order is indeed
  flexible, but is the line terminator `\r\n`, `\n`, or carrier-dependent?
  These details belong in the RFC before implementation begins so the test
  cases are authoritative rather than implementation-derived.

- **`WifiCredStore` as a separate header-only class vs. extending
  `IPersist` — the justification is weak but the conclusion is correct.**  
  The RFC says "WiFi credentials are unrelated to the TG->SMS pipeline"
  and that extending `IPersist` would pollute `FakePersist`. This is right
  in spirit but slightly misdirected. The real reason to separate them is
  that `IPersist` is an interface designed for dependency-injection and
  test-doubling of the TG->SMS pipeline, and WiFi credential storage has
  no test-double requirement — it is always hardware-backed and only called
  in `setup()`. A separate `WifiCredStore` that directly wraps `Preferences`
  without an interface layer is simpler, not more complex. The RFC's
  recommendation is correct; the rationale just needs tightening. This is
  NON-BLOCKING.

- **No rate limiting on provisioning attempts — but this is low risk.**  
  The RFC notes the absence of rate limiting and proposes a per-boot
  counter cap as a future hardening step. Given that (a) each bad-PIN SMS
  costs the attacker real money and (b) wrong PINs cause no state change,
  this is acceptable for a first implementation. The suggestion to add it
  later is fine.

- **`WIFI-CLEAR` command: PIN-protected but no confirmation.**  
  Clearing stored NVS credentials and rebooting (reverting to compiled-in
  defaults) is a destructive action that could lock the owner out if the
  compiled-in credentials are stale. There is no "are you sure?" mechanism
  and no way to recover without a USB cable if the defaults are wrong. The
  RFC should note this explicitly, or require that `WIFI-CLEAR` only
  succeeds if the compiled-in credentials are present and non-empty.

---

### Summary

The RFC is well-structured and its choice of Option C (SMS-based
provisioning) over SmartConfig or captive portal is sound: it exploits the
hardware's unique advantage and avoids adding a web server or a phone app.
However, two issues prevent approval as written. First, the PIN security
model needs an honest restatement — the PIN is not a meaningful second
factor against physical or carrier-level attackers, only against
opportunistic guessing, and the RFC should say so plainly. Second, the
boot-flow state machine has an undefined terminal state when both WiFi
fails and no provisioning SMS is found; this must be resolved to a
concrete behaviour before implementation to avoid shipping a firmware that
can hang permanently. Beyond those two blockers, the recommendation is to
**defer this RFC pending RFC-0004 (cellular fallback).** If RFC-0004 lands
first, WiFi becomes optional transport rather than the sole connectivity
path, and the entire boot-flow complexity dissolves: the device comes up
over cellular, receives the provisioning SMS through the normal `loop()`
URC path, saves new WiFi credentials, and reconnects — no pre-WiFi sweep,
no `ProvisionFn` hook, no state-machine edge cases. The RFC itself
acknowledges this interaction but stops short of recommending deferral.
Given that RFC-0004 eliminates the hardest 60% of this RFC's implementation
complexity, deferral is the correct call. If the owner needs WiFi
re-provisioning urgently before RFC-0004 is ready, the minimal viable
version is: just add NVS credential storage and the pre-WiFi sweep, skip
the `ProvisionFn` runtime hook entirely, and leave the boot-flow hang
problem solved by "reboot after N failed provisioning sweeps" with a
generous timeout.
