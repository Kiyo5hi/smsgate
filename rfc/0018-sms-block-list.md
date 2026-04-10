---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0018: Compile-time SMS sender block list

## Motivation

The bridge forwards every incoming SMS to Telegram regardless of source.
On an active SIM this produces a constant stream of carrier service
messages (`10086`, `10010`), app one-time passwords the user doesn't care
about, and promotional spam — all interleaved with the real person-to-person
messages the bridge exists to surface. There is no way to suppress them
short of reflashing with custom logic.

An allow-list pattern was already applied to the Telegram side (RFC-0014:
`TELEGRAM_CHAT_IDS`). A symmetric block list for SMS senders follows the
same design philosophy: compile-time, secrets.h-backed, zero runtime
overhead, and no NVS wear.

## Current state

`SmsHandler::handleSmsIndex(int idx)` is the sole processing path for
incoming SMS. After PDU parsing (`sms_codec::parseSmsPdu`), the handler
unconditionally calls `forwardSingle()` or `insertFragmentAndMaybePost()`
depending on whether the message is single-part or concatenated. There is
no hook between "sender is known" and "forward to Telegram."

`src/allow_list.h` provides `parseAllowedIds()` (a `strchr`-based CSV
parser for `int64_t` Telegram user IDs) as a header-only inline. Phone
numbers are strings, not integers, so `parseAllowedIds` cannot be reused
directly, but the same `strchr`-based splitting pattern is reusable.

The `sms_codec::SmsPdu` struct carries `String sender` (normalized E.164
or raw number, whichever the modem delivered) populated before any
forwarding decision is made — so the block-list check can be a single
predicate call at the right point in the pipeline with no change to the
PDU parsing layer.

## Plan

### 1. The `SMS_BLOCK_LIST` define

Add an optional `SMS_BLOCK_LIST` define to `src/secrets.h.example`:

```cpp
// Comma-separated phone numbers whose SMS should be silently deleted
// without forwarding to Telegram.  No spaces.  Optional — if this macro
// is not defined, no filtering is performed.
//
// Matching is EXACT: "10086" and "+8610086" are different strings.
// If your carrier sometimes delivers the same number in both forms, list
// both entries.  See RFC-0018 for details.
//
// Example: #define SMS_BLOCK_LIST "10086,10010,+8610086"
// #define SMS_BLOCK_LIST ""
```

The macro is intentionally not defined by default so existing builds are
unaffected. Users paste it into their `secrets.h` or pass it as a build
flag:

```ini
; platformio.ini
build_flags = -DSMS_BLOCK_LIST='"10086,10010"'
```

The define is a C string literal. Maximum 20 entries, maximum 20
characters per number — see the new header below.

### 2. New header: `src/sms_block_list.h`

A pure, header-only translation unit with no Arduino or hardware
dependencies. Structure mirrors `allow_list.h`.

```cpp
#pragma once
// RFC-0018: SMS sender block list helpers.
//
// parseBlockList() and isBlocked() are pure functions (no hardware deps,
// no Arduino.h beyond <string.h>) so they live in this header as inlines
// and compile into both the firmware and the native test env.

#include <string.h>

static constexpr int kSmsBlockListMaxEntries = 20;
static constexpr int kSmsBlockListMaxNumberLen = 20; // chars, not including NUL

// Parse a comma-separated string of phone numbers into out[][21].
// Returns the number of entries parsed.
//
// Rules:
//   - Leading/trailing ASCII whitespace around each token is trimmed.
//   - Empty tokens (two adjacent commas, leading/trailing comma) are skipped.
//   - Tokens longer than kSmsBlockListMaxNumberLen are truncated with a
//     NUL terminator — the entry is still stored (the user will notice
//     the mismatch when block-listing fails to fire).
//   - Silently truncates at maxEntries; the caller should log if count
//     equals maxEntries and the CSV may have had more tokens.
//   - Uses strchr-based splitting — not strtok — to avoid the static-
//     pointer re-entrancy hazard.
inline int parseBlockList(const char *csv,
                          char out[][kSmsBlockListMaxNumberLen + 1],
                          int maxEntries)
{
    if (!csv || maxEntries <= 0)
        return 0;
    int count = 0;
    const char *p = csv;
    while (*p && count < maxEntries)
    {
        while (*p == ' ' || *p == '\t') ++p;
        const char *start = p;
        const char *comma = strchr(p, ',');
        const char *end = comma ? comma : (p + strlen(p));
        const char *trim = end;
        while (trim > start && (*(trim-1) == ' ' || *(trim-1) == '\t'))
            --trim;
        size_t len = (size_t)(trim - start);
        if (len > 0)
        {
            if (len > (size_t)kSmsBlockListMaxNumberLen)
                len = (size_t)kSmsBlockListMaxNumberLen;
            memcpy(out[count], start, len);
            out[count][len] = '\0';
            count++;
        }
        if (!comma) break;
        p = comma + 1;
    }
    return count;
}

// Return true if `number` exactly matches any entry in `list[0..count-1]`.
inline bool isBlocked(const char *number,
                      const char (*list)[kSmsBlockListMaxNumberLen + 1],
                      int count)
{
    if (!number || !list || count <= 0) return false;
    for (int i = 0; i < count; i++)
    {
        if (strcmp(number, list[i]) == 0)
            return true;
    }
    return false;
}
```

The `out` array type `char[][kSmsBlockListMaxNumberLen + 1]` (21 bytes per
slot) is intentional: it matches the declaration at the call site and makes
the per-entry size self-documenting in the function signature.

### 3. Call site in `SmsHandler`

**Where to check:** in `handleSmsIndex()`, immediately after `parseSmsPdu`
succeeds and the sender is known, but before any path that writes to the
debug log or calls `forwardSingle` / `insertFragmentAndMaybePost`.

Concrete diff sketch (using the `blockList_` / `blockListCount_` members —
**no `#ifdef` inside `sms_handler.cpp`**; the guard is purely runtime):

```cpp
// In sms_handler.cpp, after the parseSmsPdu() block succeeds:

    if (blockList_ && isBlocked(pdu.sender.c_str(), blockList_, blockListCount_))
    {
        Serial.print("SMS from blocked sender ");
        Serial.print(pdu.sender);
        Serial.println(", deleting silently.");
        // MUST delete the SIM slot unconditionally — unlike the normal
        // path where deletion is conditional on a successful Telegram POST.
        // Omitting this causes infinite boot-loop replays of blocked
        // fragments via sweepExistingSms.
        modem_.sendAT("+CMGD=" + String(idx));
        modem_.waitResponseOk(1000UL);
        return;
    }
```

`blockList_` is a member pointer set by `setBlockList()` (see below).
`blockListCount_` is the corresponding count. When `SMS_BLOCK_LIST` is not
defined, `setBlockList()` is never called and `blockList_` stays `nullptr`,
so the `if (blockList_ && ...)` guard is a no-op — no `#ifdef` is needed in
`sms_handler.cpp`, and the native test env can inject a block list directly
via `smsHandler.setBlockList(list, n)`.

**Initialization.** Because `SmsHandler` must compile cleanly in the
native test env without `secrets.h`, the block-list storage and parsing
live in `main.cpp` (which is excluded from the native build) and are
injected via a new optional setter on `SmsHandler`:

```cpp
// sms_handler.h
void setBlockList(const char (*list)[kSmsBlockListMaxNumberLen + 1], int count)
{
    blockList_ = list;
    blockListCount_ = count;
}

private:
    const char (*blockList_)[kSmsBlockListMaxNumberLen + 1] = nullptr;
    int blockListCount_ = 0;
```

In `main.cpp` (inside or before `setup()`):

```cpp
#ifdef SMS_BLOCK_LIST
static char sBlockList[kSmsBlockListMaxEntries][kSmsBlockListMaxNumberLen + 1];
static int  sBlockListCount = 0;
#endif

// Inside setup(), after constructing smsHandler:
#ifdef SMS_BLOCK_LIST
sBlockListCount = parseBlockList(SMS_BLOCK_LIST, sBlockList, kSmsBlockListMaxEntries);
Serial.print("SMS block list: ");
Serial.print(sBlockListCount);
Serial.println(" entries");
if (sBlockListCount == kSmsBlockListMaxEntries)
    Serial.println("[WARN] Block list truncated at max entries — check SMS_BLOCK_LIST");
smsHandler.setBlockList(sBlockList, sBlockListCount);
#endif
```

This keeps `SmsHandler` free of compile-time preprocessor gating; the
`#ifdef` is confined to `main.cpp` and the header includes a graceful
no-op default (`blockList_ = nullptr`, `blockListCount_ = 0`).

### 4. Exact matching only (recommended)

This RFC specifies **exact string match only** — `strcmp` on the raw
`pdu.sender` string as delivered by the modem.

Rationale: prefix matching (e.g., block everything starting with `+86108`)
would be simple to add but risks false positives against legitimate
numbers that share a prefix. The marginal convenience gain (fewer
entries in the list) is not worth the risk of silently dropping a real
message. Callers who need broader suppression can add multiple entries.

**The normalization trap.** The `pdu.sender` field is whatever the modem
decoded from the PDU address field. Depending on carrier and modem
firmware, a Chinese service number may arrive as `10086` (national format)
or as `+8610086` (E.164 with country code). Both are valid encodings of
the same logical number. The bridge does NOT normalize sender numbers
before matching — it would need a country-code database to do so reliably.

Consequence: users who want to block a sender that may be delivered in
either form must list both entries:

```
#define SMS_BLOCK_LIST "10086,+8610086,10010,+8610010"
```

This must be documented prominently in `secrets.h.example` and in the
`Serial.print` boot log ("SMS block list: N entries"), so the user knows
what strings the bridge is actually comparing against. The `/debug` log
command also surfaces the raw `pdu.sender` value for every received SMS,
which is the recommended way to discover which format your carrier uses
before adding an entry.

### 5. Concatenated SMS handling

**The fragment-before-sender problem.** Multi-part SMS arrives as
individual PDU fragments, each carrying the sender address. The sender
IS known at fragment-arrival time — `pdu.sender` is populated for every
fragment. However, the concat reassembly buffer groups fragments by
(sender, ref_number). A blocked sender's fragments are individually
identifiable from the very first fragment.

**Two possible check points:**

**Option A: Block at the fragment stage** (before `insertFragmentAndMaybePost`).
If the sender is blocked, delete the fragment's SIM slot and return
immediately. Subsequent fragments from the same blocked sender are also
caught one by one as they arrive.

Pros:
- Fragments never enter the reassembly buffer — no memory overhead.
- SIM slots are freed immediately.
- Symmetric with the single-part path.

Cons:
- All fragments of a blocked concat group must be individually intercepted
  and deleted. If a fragment arrives and is deleted, but a later fragment
  had already been received in a previous session and is sitting in a SIM
  slot, `sweepExistingSms` at boot would re-enqueue it. On the next boot,
  the fragment would be intercepted again and deleted. This is idempotent
  and correct, but can appear confusing in the log (deleted a fragment
  from a sender that "never arrived").

**Option B: Block at the assembled stage** (inside `insertFragmentAndMaybePost`,
after `group->fragments.size() >= group->totalParts`, before calling
`bot_.sendMessageReturningId`). Fragment accumulation proceeds normally;
the block decision fires only when the full message is ready.

Pros:
- Minimal code change — one check in one location.
- No interaction with the fragment-stage path.

Cons:
- Fragments from blocked senders consume RAM and NVS/SIM resources
  during reassembly. For a spammy concat source this could fill the 8-key
  / 8 KB reassembly caps and evict legitimate in-flight groups. This is
  a real concern if a carrier sends 10-part promotional messages.
- The "blocked" decision is deferred until after all parts arrive, which
  takes longer to clean up SIM slots.

**Recommendation: Option A (block at the fragment stage).**

The per-fragment block check is the correct place because it mirrors the
single-part path exactly, frees SIM slots immediately, and prevents
reassembly-buffer pollution. The idempotent boot-sweep behavior is not a
bug — it is the same mechanism that rehydrates legitimate incomplete
groups. The implementation diff is five lines in `handleSmsIndex()` (one
check before the `if (!pdu.isConcatenated)` branch and one before the
`insertFragmentAndMaybePost` call), both guarded by the same
`isBlocked()` predicate.

The concrete structure in `handleSmsIndex()` after the change:

```
1. CMGR + PDU parse (existing)
2. isBlocked check  <-- NEW, covers BOTH single and concat
3. if single-part → forwardSingle (existing)
4. if concat       → insertFragmentAndMaybePost (existing)
```

Step 2 is a single `if (isBlocked(...)) { delete SIM slot; return; }`
block that short-circuits both downstream paths.

### 6. Test approach

`parseBlockList` and `isBlocked` are pure `<string.h>`-only functions with
no Arduino or hardware dependencies. They are directly exercisable in the
native test env.

Add `test/test_native/test_sms_block_list.cpp` with cases:

- `parseBlockList(nullptr, ...)` returns 0.
- `parseBlockList("", ...)` returns 0.
- Single entry, no whitespace: count == 1, entry matches input.
- Two entries: count == 2, both match.
- Leading/trailing whitespace around tokens is stripped.
- Empty token from adjacent commas is skipped.
- Trailing comma produces no extra entry.
- Token longer than `kSmsBlockListMaxNumberLen` is truncated to exactly 20
  chars with a NUL terminator; count still increments.
- `maxEntries == 0` returns 0.
- Exactly `maxEntries` tokens fills the array; the function returns
  `maxEntries` not `maxEntries + 1`.
- `isBlocked(nullptr, ...)` returns false.
- `isBlocked("10086", list, 0)` returns false.
- `isBlocked("10086", list_without_10086, n)` returns false.
- `isBlocked("10086", list_with_10086, n)` returns true.
- `isBlocked("1008", list_with_10086, n)` returns false (prefix is not a
  match).
- `isBlocked("10086x", list_with_10086, n)` returns false (suffix is not
  a match).
- Case sensitivity: `isBlocked("+8610086", list_with_plus_8610086, n)`
  returns true; `isBlocked("+8610086", list_with_10086, n)` returns false.

The `SmsHandler` integration (the new `setBlockList` setter and the
`isBlocked` gate in `handleSmsIndex`) should be covered by two new test
cases in the existing `test_sms_handler.cpp`:

- Blocked sender (single-part): verify `forwardSingle` is never called,
  the SIM slot is deleted via `IModem`, and the consecutive-failure counter
  is NOT bumped.
- Blocked sender (concat fragment): verify `insertFragmentAndMaybePost` is
  never called, the SIM slot is deleted, and the failure counter is not
  bumped.

No hardware tests are required beyond confirming the boot log prints the
correct "SMS block list: N entries" line after flashing.

### 7. Follow-up: runtime `/block` command (out of scope for this RFC)

A `/block <number>` Telegram bot command that persists entries to NVS
would allow runtime management without reflashing. Design sketch:

- New NVS key `"smsblist"` under the `"tgsms"` namespace: a fixed-size
  blob of `char[20][21]` (8.4 KB maximum) plus a count byte.
- Authorization: admin-only (same `allowedIds[0]` gate as RFC-0014 used
  for `/adduser`).
- At startup, merge the compile-time block list with the NVS list before
  calling `smsHandler.setBlockList()`. Alternatively, accept two block
  lists in `SmsHandler` (compile-time static + NVS-backed dynamic) and
  check both.
- Matching in `handleSmsIndex` needs to check either the merged list or
  both lists.

This is deferred because: (a) the compile-time list covers the common
case (carrier service numbers are stable); (b) NVS wear from `/block`
writes is negligible (rare operation) but the serialization format needs
careful design to survive partial writes; (c) the bot command parser in
`TelegramPoller::processUpdate` already handles `/debug` and `/status`
via simple string comparison — argument parsing for `/block <number>`
requires extracting the trailing token, which is a small but distinct
change. None of these is a blocker, but they are enough friction to
justify leaving this to a follow-up RFC rather than bundling it here.

## Review

**verdict: approved-with-changes**

### BLOCKING

- **`#ifdef` contradiction in `sms_handler.cpp` diff sketch.** The
  "Notes for handover" section correctly states the check in
  `handleSmsIndex` should be a plain runtime branch
  (`if (blockList_ && isBlocked(...))`) with no `#ifdef`, so that host
  tests can inject a block list without defining `SMS_BLOCK_LIST`.
  However, the concrete diff sketch in §3 shows the check wrapped in
  `#ifdef SMS_BLOCK_LIST ... #endif` inside `sms_handler.cpp`. These two
  descriptions are mutually exclusive. The implementation must follow the
  handover note (runtime branch, no `#ifdef` in `sms_handler.cpp`) —
  the diff sketch needs to be corrected before implementation begins.
  As written, a developer following the diff sketch literally would break
  the native test path.

- **SIM slot deletion contract for blocked fragments.** The RFC text in
  §5 ("Step 2" structure and the "Pros" list for Option A) correctly
  says fragments' SIM slots are "freed immediately." The diff sketch in §3
  confirms `AT+CMGD` is issued before return. However, the correctness
  argument for Option A relies on this deletion being unconditional — if
  the `CMGD` call is accidentally omitted in the actual implementation,
  blocked fragments would accumulate on the SIM and be replayed at every
  boot forever. The RFC should explicitly call out this as a
  **must-not-omit** requirement (distinct from the ordinary single-part
  success path where deletion is conditional on a successful Telegram
  POST). It currently buries this in the Option A pros list rather than
  in the normative implementation spec.

### NON-BLOCKING

- **Debug log interaction is correct but the placement described is
  inconsistent.** §3 says the block check fires "before any path that
  writes to the debug log." Looking at the actual code in
  `sms_handler.cpp`, the debug log entry is populated immediately after
  `parseSmsPdu` succeeds (lines 401–413) and is pushed to the log only
  inside the single/concat outcome branches. Inserting the block check
  before the `SmsDebugLog::Entry logEntry` block (as §3 implies) means a
  blocked SMS gets zero debug log coverage — no way to discover what the
  modem delivered. The handover note confirms this is intentional ("blocked
  messages should not appear in the /debug log"), but it conflicts with
  the earlier handover note about using the `/debug` command to discover
  the raw `pdu.sender` format. Consider at minimum emitting a
  `Serial.print` before returning (the diff sketch already does this),
  and note that `/debug` cannot be used to discover the sender format for
  a number already in the block list.

- **`kSmsBlockList` / `kSmsBlockListCount` naming in the diff sketch.**
  The diff sketch in §3 refers to `kSmsBlockList` and `kSmsBlockListCount`
  as if they are file-scope statics in `sms_handler.cpp`, but the paragraph
  immediately below clarifies they actually live in `main.cpp` and are
  injected via `setBlockList`. The diff sketch will confuse an implementer:
  those names cannot exist in `sms_handler.cpp` (it doesn't include
  `sms_block_list.h` from the diff's perspective) and don't match the
  member names (`blockList_`, `blockListCount_`). The diff sketch should
  be rewritten to show the runtime branch using the member names.

- **Stack storage for `sBlockList` is correct but the RFC doesn't state
  why.** `char sBlockList[20][21]` (420 bytes) is declared as a
  `static` file-scope variable in `main.cpp` in the §3 snippet, which
  is the right choice. The RFC does not explain this explicitly — an
  implementer following the sketch should understand that making it a
  local variable inside `setup()` would still work (420 bytes is under
  the ESP32's 8 KB task-stack allocation for `setup()`) but the pointer
  stored in `SmsHandler` would then dangle after `setup()` returns.
  Mentioning this subtlety in the handover note would prevent a subtle
  use-after-scope bug.

- **`pdu.sender` format guidance is adequate but could be more direct.**
  §4 correctly explains the normalization trap and recommends listing
  both forms. The discovery path ("use the /debug command") works only
  for numbers NOT already in the block list. Consider adding a
  note that the `Serial.print` boot log line showing each received
  sender (lines 393–394 of `sms_handler.cpp`) is always emitted before
  the block check fires, so the serial monitor can be used for initial
  discovery even when a number is already blocked.

- **`sweepExistingSms` path is correctly handled** and the RFC
  explicitly documents it in the handover notes. No issue. Noted
  here only for completeness.

- **`isBlocked` null pointer for `list` is not guarded.** The function
  guards `number == nullptr` and `count <= 0` but does not guard
  `list == nullptr`. In the production path `list` is only null when
  `count == 0` (the `blockList_ = nullptr` default pairs with
  `blockListCount_ = 0`), so the runtime `if (blockList_ && ...)` guard
  in `handleSmsIndex` prevents the bad call. But a unit test that passes
  `list = nullptr, count = 1` would dereference null. Add a `!list`
  guard inside `isBlocked` to make it defensively correct.

- **Test case for `isBlocked(number, nullptr, 0)` is missing.** The
  proposed test suite covers `isBlocked(nullptr, ...)` and
  `isBlocked("10086", list, 0)` but not the combination where both
  `list` is null and `count` is 0. Symmetry with the `parseBlockList`
  null-input test suggests adding it.

- **17-test count includes the `SmsHandler` integration tests.** The
  RFC correctly notes those belong in the existing
  `test_sms_handler.cpp`. Confirm `FakeModem` exposes a way to assert
  that `AT+CMGD=<idx>` was issued (it does via `lastCmd` or similar
  mechanism) — the review cannot confirm this without reading
  `test/support/fake_modem.h`, but the RFC should state which
  `FakeModem` accessor the blocked-slot-deletion assertion relies on.

## Code Review

**verdict: PASS — all blocking issues resolved, all non-blocking issues addressed**

Reviewed against the actual implementation files. Each numbered point
below maps to the checklist from the review brief.

### 1. No `#ifdef` in `sms_handler.cpp` — PASS

`sms_handler.cpp` line 405:

```cpp
if (blockList_ && isBlocked(pdu.sender.c_str(), blockList_, blockListCount_))
```

There is no `#ifdef SMS_BLOCK_LIST` anywhere in `sms_handler.cpp`. The
prior `## Review` section flagged this as BLOCKING. The implementation
correctly follows the runtime-branch approach, not the preprocessor
approach. The native test env can inject a block list directly via
`setBlockList()` without defining `SMS_BLOCK_LIST`.

### 2. `AT+CMGD` issued unconditionally before return — PASS

`sms_handler.cpp` lines 406–412:

```cpp
{
    Serial.print("SMS from blocked sender ");
    Serial.print(pdu.sender);
    Serial.println(", deleting silently.");
    modem_.sendAT("+CMGD=" + String(idx));
    modem_.waitResponseOk(1000UL);
    return;
}
```

`AT+CMGD` is unconditional. The comment on lines 399–404 explicitly
calls out the boot-replay hazard. The prior BLOCKING issue is fixed.

### 3. Block check before `if (!pdu.isConcatenated)` branch — PASS

The sequence in `handleSmsIndex()` is:

1. CMGR + PDU parse (lines 356–390)
2. Debug print of sender/timestamp/content (lines 392–397)
3. Block check (lines 399–413) — single `if` block covering both paths
4. Debug log capture (lines 415–429)
5. Single-part branch (lines 431–447)
6. Concat path (lines 449–490)

The block check at step 3 precedes both downstream paths. One
check point covers both single-part and concat fragments, consistent
with Option A from RFC §5.

Note: the block check fires AFTER the `Serial.print` of
sender/timestamp/content (lines 392–397), not before. This is a minor
divergence from the RFC's "before any path that writes to the debug
log" wording, but the outcome is benign and arguably useful: the serial
monitor will show the raw `pdu.sender` for blocked messages, which
directly addresses the sender-format discovery use case from RFC §4.
The divergence does not affect correctness. The `/debug` log entry is
still suppressed for blocked senders (the `logEntry` capture at step 4
is never reached). **NON-BLOCKING.**

### 4. Consecutive-failure counter not bumped for blocked messages — PASS

The block-check branch returns immediately before any call to
`noteTelegramFailure()` or to the `forwardSingle` / `insertFragmentAndMaybePost`
paths that gate on Telegram success. The integration tests
(`test_blocked_single_part_not_forwarded_sim_slot_deleted` and
`test_blocked_concat_fragment_not_buffered_sim_slot_deleted`) both
assert `handler.consecutiveFailures() == 0` and `rebootCalls == 0`.

### 5. `isBlocked(number, nullptr, count>0)` null-list guard — PASS

`sms_block_list.h` line 63:

```cpp
if (!number || !list || count <= 0) return false;
```

The prior NON-BLOCKING issue (missing `!list` guard) is fixed. The
guard covers `list == nullptr` regardless of `count`.

`test_sms_block_list.cpp` includes two tests for `nullptr` list:
- `test_isBlocked_null_list_returns_false`: `isBlocked("10086", nullptr, 1)` → false
- `test_isBlocked_null_list_zero_count_returns_false`: `isBlocked("10086", nullptr, 0)` → false

Both cases from the prior review are covered.

### 6. Integration tests assert `AT+CMGD` via `FakeModem::sentCommands()` — PASS

Both integration tests use `modem.sentCommands()` (the `std::vector<String>`
returned by `FakeModem::sentCommands()`) to assert the exact AT command
sequence. The prior NON-BLOCKING concern about which `FakeModem`
accessor was used is resolved: it is `sentCommands()`, not a `lastCmd`
field. The assertions are:

Single-part test (`test_blocked_single_part_not_forwarded_sim_slot_deleted`):
```cpp
TEST_ASSERT_EQUAL(2, (int)sent.size());
TEST_ASSERT_EQUAL_STRING("+CMGR=7", sent[0].c_str());
TEST_ASSERT_EQUAL_STRING("+CMGD=7", sent[1].c_str());
```

Concat fragment test (`test_blocked_concat_fragment_not_buffered_sim_slot_deleted`):
```cpp
TEST_ASSERT_EQUAL(2, (int)sent.size());
TEST_ASSERT_EQUAL_STRING("+CMGR=5", sent[0].c_str());
TEST_ASSERT_EQUAL_STRING("+CMGD=5", sent[1].c_str());
```

The concat test also asserts `handler.concatKeyCount() == 0`, confirming
the fragment never entered the reassembly buffer. Both tests also assert
`bot.callCount() == 0`.

### 7. `sms_block_list.h` has no Arduino.h dependency — PASS

The header includes only `<string.h>`. No `Arduino.h`, no `String`,
no `millis()`, no hardware types. Functions are `inline` with no
external linkage requirements. The header compiles cleanly in the
native test env: `test_sms_block_list.cpp` includes it directly and
the `[env:native]` `build_src_filter` does not list `sms_block_list.h`
(header-only, no `.cpp` TU to include).

### 8. All tests registered in `test_main.cpp` — PASS

`test_main.cpp` declares `void run_sms_block_list_tests()` and calls it.
Both integration tests in `test_sms_handler.cpp`
(`test_blocked_single_part_not_forwarded_sim_slot_deleted` and
`test_blocked_concat_fragment_not_buffered_sim_slot_deleted`) are
registered in `run_sms_handler_tests()` which is already called from
`test_main.cpp`.

### 9. Double-include / ODR risk from `#include "sms_block_list.h"` in `sms_handler.h` — PASS (with note)

`main.cpp` includes `sms_block_list.h` directly (line 17) and also
includes `sms_handler.h` (line 19), which itself includes
`sms_block_list.h` (line 11). Within a single translation unit the
`#pragma once` guard prevents the header from being processed twice,
so there is no double-definition of `kSmsBlockListMaxEntries` or
`kSmsBlockListMaxNumberLen` within any single TU.

`static constexpr int` at namespace scope gets internal linkage in
C++17 (a `static constexpr` integral constant in a header is
permissible and does not violate the ODR when the header is included
in multiple TUs). The `inline` functions are explicitly `inline`,
also ODR-safe. No issue.

The direct include in `main.cpp` is redundant (it is transitively
satisfied by the `sms_handler.h` include) but harmless. **NON-BLOCKING.**

### 10. New total test count — 194 tests

`RUN_TEST` call counts per file after the RFC-0018 additions:

| File | Count |
|---|---|
| test_sms_codec.cpp | 22 |
| test_sms_handler.cpp | 13 (was 11, +2 block-list integration tests) |
| test_call_handler.cpp | 18 |
| test_sms_pdu.cpp | 8 |
| test_sms_handler_pdu.cpp | 10 |
| test_reply_target_map.cpp | 9 |
| test_sms_sender.cpp | 17 |
| test_telegram_poller.cpp | 22 |
| test_sms_pdu_encode.cpp | 19 |
| test_delivery_report.cpp | 21 |
| test_allow_list.cpp | 15 |
| test_sms_block_list.cpp | 20 (new file) |
| **Total** | **194** |

All 194 tests are expected to pass. No test exercises a path that was
absent before RFC-0018 (the new code paths are pure additions with no
modification of existing behaviour when `blockList_ == nullptr`).

### Additional findings

**`sBlockList` declared as file-scope `static` in `main.cpp` — PASS.**
The prior NON-BLOCKING concern about dangling pointer from a
`setup()`-local variable is correctly avoided: `sBlockList` is declared
at file scope (`static char sBlockList[...][...]`), so the pointer
stored in `SmsHandler` via `setBlockList()` is valid for the process
lifetime.

**`secrets.h.example` comment is accurate and positioned correctly.**
The block-list comment is the last entry in the file, following the
heartbeat entry. It correctly documents the exact-match semantics and
the national-vs-E.164 ambiguity. The example value includes neither
trailing nor leading spaces. Matches the RFC §4 guidance.

**`main.cpp` includes `sms_block_list.h` before `sms_handler.h`.**
This ordering is fine. Because `sms_handler.h` also includes
`sms_block_list.h` via `#pragma once`, the include order between the
two is irrelevant. No issue.
