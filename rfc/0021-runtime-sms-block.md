---
status: implemented
created: 2026-04-10
updated: 2026-04-09
---

# RFC-0021: Runtime SMS block list management via bot commands

## Motivation

RFC-0018 implemented a compile-time `SMS_BLOCK_LIST` define that silently
deletes incoming SMS from known-spam senders (carrier service numbers, OTP
sources, promotional numbers). That list works well for a stable set of
senders where the user is willing to reflash to add entries.

The gap: in practice new spam sources appear constantly. Adding `10086` and
`10010` at compile time doesn't help when a new number starts sending
promotional messages next week. Reflashing requires a build environment and
physical access to the board. RFC-0018 §7 explicitly deferred a runtime
management path:

> A `/block <number>` Telegram bot command that persists entries to NVS would
> allow runtime management without reflashing.

RFC-0019 laid the exact infrastructure this feature needs:

- `IPersist::loadBlob` / `saveBlob` — generic NVS blob I/O now available.
- `ListMutatorFn` pattern in `TelegramPoller` — admin-checked mutation lambda
  injected from `main.cpp`; the poller dispatches the command and the lambda
  does auth + state change + NVS write.
- `extractArg` file-static helper in `telegram_poller.cpp` — already strips a
  command prefix and trims whitespace; identical pattern works for `/block`.

The compile-time `isBlocked()` function in `sms_block_list.h` is already
called twice in `handleSmsIndex` (once pre-concat-branch for both single-part
and concat). Calling it a second time against a runtime list adds one
`isBlocked()` call — no new setter on `SmsHandler`, no merged array to
maintain.

## Current state

### Block list in firmware

`src/sms_block_list.h` defines:
- `kSmsBlockListMaxEntries = 20` — max entries per list.
- `kSmsBlockListMaxNumberLen = 20` — max characters per number (NUL not
  included), so each slot is 21 bytes.
- `parseBlockList(csv, out[][21], maxEntries)` — pure, `<string.h>`-only CSV
  parser.
- `isBlocked(number, list[][21], count)` — pure exact-match predicate.

`src/sms_handler.h` has a `setBlockList(list, count)` setter that stores a
pointer and count as private members `blockList_` / `blockListCount_`.
`handleSmsIndex` checks `if (blockList_ && isBlocked(...))` immediately after
PDU parse, before either the single-part or concat downstream paths.

`main.cpp` declares file-scope statics:

```cpp
#ifdef SMS_BLOCK_LIST
static char sBlockList[kSmsBlockListMaxEntries][kSmsBlockListMaxNumberLen + 1];
static int  sBlockListCount = 0;
#endif
```

populated at startup with `parseBlockList(SMS_BLOCK_LIST, ...)` and injected
via `smsHandler.setBlockList(sBlockList, sBlockListCount)`.

### TelegramPoller command dispatch

`processUpdate` in `telegram_poller.cpp` dispatches non-reply messages by
string comparison on the lowercased text. After the `AuthFn` gate, the
dispatch table is currently:

| Command | Handler |
|---------|---------|
| `/debug` | dump `SmsDebugLog` |
| `/status` | call `StatusFn` |
| `/listusers` | call `mutator_(fromId, "list", 0, reason)` |
| `/adduser <id>` | call `mutator_(fromId, "add", targetId, reason)` |
| `/removeuser <id>` | call `mutator_(fromId, "remove", targetId, reason)` |
| (anything else) | error reply with usage hint |

The `ListMutatorFn` type alias (from RFC-0019, as actually implemented) is:

```cpp
using ListMutatorFn = std::function<bool(int64_t callerId, const String &cmd,
                                         int64_t targetId, String &reason)>;
```

The 8th constructor parameter to `TelegramPoller` (defaulting to `nullptr`).

### NVS key space

`RealPersist` (namespace `"tgsms"`) currently uses:

| Key | Content |
|-----|---------|
| `"uid"` | `int32_t` last Telegram update_id watermark |
| `"rtm"` | `ReplyTargetMap` ring buffer blob |
| `"ulist"` | runtime user list blob (RFC-0019) |
| `"smslog"` | SMS debug log blob (RFC-0020) |

Key `"smsblist"` is available for the runtime block list.

## Plan

### 1. Three new bot commands

Dispatch inside `TelegramPoller::processUpdate`, after the existing `AuthFn`
gate, in the non-reply-message branch (same location as `/adduser` et al.):

| Command | Argument | Who may call |
|---------|----------|--------------|
| `/block <number>` | A phone number string | Admin only |
| `/unblock <number>` | A phone number string | Admin only |
| `/blocklist` | none | Any authorized user |

"Admin" means `u.fromId` is in the compile-time `allowedIds[]` array — same
definition as RFC-0019's admin gate for `/adduser` / `/removeuser`.

The dispatch code follows the established pattern:

```cpp
// /blocklist — any authorized user.
if (lower == "/blocklist")
{
    if (!smsBlockMutator_)
    {
        bot_.sendMessageTo(u.chatId, String("SMS block list management not configured."));
        return;
    }
    String reason;
    smsBlockMutator_(u.fromId, String("list"), String(), reason);
    bot_.sendMessageTo(u.chatId, reason);
    return;
}

// /block <number> — admin only (enforced inside smsBlockMutator_).
if (lower == "/block" || lower.startsWith("/block "))
{
    String arg = extractArg(lower, "/block ");
    if (arg.length() == 0)
    {
        bot_.sendMessageTo(u.chatId, String("Usage: /block <number>"));
        return;
    }
    if (!smsBlockMutator_)
    {
        bot_.sendMessageTo(u.chatId, String("SMS block list management not configured."));
        return;
    }
    String reason;
    if (!smsBlockMutator_(u.fromId, String("block"), arg, reason))
    {
        bot_.sendMessageTo(u.chatId, reason);
        return;
    }
    bot_.sendMessageTo(u.chatId, String("Blocked: ") + arg);
    return;
}

// /unblock <number> — admin only (enforced inside smsBlockMutator_).
if (lower == "/unblock" || lower.startsWith("/unblock "))
{
    String arg = extractArg(lower, "/unblock ");
    if (arg.length() == 0)
    {
        bot_.sendMessageTo(u.chatId, String("Usage: /unblock <number>"));
        return;
    }
    if (!smsBlockMutator_)
    {
        bot_.sendMessageTo(u.chatId, String("SMS block list management not configured."));
        return;
    }
    String reason;
    if (!smsBlockMutator_(u.fromId, String("unblock"), arg, reason))
    {
        bot_.sendMessageTo(u.chatId, reason);
        return;
    }
    bot_.sendMessageTo(u.chatId, String("Unblocked: ") + arg);
    return;
}
```

Note: the `extractArg` helper works on the *lowercased* text, so phone numbers
come through lowercased too. For phone numbers (digits, `+`, `-`) this is
harmless — there are no lowercase-vs-uppercase distinctions in E.164. The
`arg` passed to `smsBlockMutator_` must be compared case-insensitively if the
user could type letter-based vanity numbers, but for numeric phone numbers
no normalization is needed beyond what the user already typed.

### 2. New `SmsBlockMutatorFn` type and constructor parameter

Add a parallel type alias to `telegram_poller.h`, following the `ListMutatorFn`
pattern:

```cpp
// Called by /block, /unblock, /blocklist.
// Signature: (callerId, cmd, number, reason&) -> bool
//   - cmd is "block", "unblock", or "list"
//   - number is the phone number string (empty for "list")
//   - On success: returns true; for "list", reason contains the reply text
//   - On failure: returns false; reason contains the user-facing error message
// The lambda is responsible for the admin check (callerId in compile-time list?).
// nullptr means SMS block list bot commands are disabled.
using SmsBlockMutatorFn = std::function<bool(int64_t callerId, const String &cmd,
                                              const String &number, String &reason)>;
```

Add as the 9th constructor parameter (defaulting to `nullptr`):

```cpp
TelegramPoller(IBotClient &bot,
               SmsSender &smsSender,
               ReplyTargetMap &replyTargets,
               IPersist &persist,
               ClockFn clock,
               AuthFn auth,
               StatusFn status = nullptr,
               ListMutatorFn mutator = nullptr,
               SmsBlockMutatorFn smsBlockMutator = nullptr);
```

Add member `SmsBlockMutatorFn smsBlockMutator_` to `TelegramPoller`'s private
section, initialized from the constructor.

All existing `TelegramPoller` construction call sites in `main.cpp` pass the
8-parameter form (with `mutator` named); the 9th parameter defaults to
`nullptr`, so existing builds compile without change. Tests that construct
`TelegramPoller` with positional arguments are unaffected unless they already
pass exactly 8 arguments — those sites need a trailing `nullptr`.

### 3. NVS storage: key `"smsblist"`

**Format** (424 bytes, fixed size):

```
Offset  Size   Field
──────  ────   ────────────────────────────────────────────────────────
0       4      int32_t count        number of valid entries (0..20)
4       420    char numbers[20][21] phone numbers, NUL-terminated each
```

Total: `sizeof(int32_t) + 20 * 21 = 4 + 420 = 424` bytes.

Loaded at startup via `realPersist.loadBlob("smsblist", &blob, sizeof(blob))`.
Saved after every `/block` or `/unblock` via
`realPersist.saveBlob("smsblist", &blob, sizeof(blob))`.

The `int32_t count` prefix makes the blob self-describing. On first use the
key is absent in NVS; treat as an empty list (count = 0). On corruption
(loaded `count` is outside `[0, 20]`), treat as empty and overwrite on next
save.

NVS wear: `/block` and `/unblock` are rare (O(1) per week at most). Writing
424 bytes once per command against an NVS flash sector with ~10,000-cycle
endurance is negligible.

### 4. In-memory runtime block list in `main.cpp`

Add two file-scope statics, parallel to the compile-time `sBlockList[]`:

```cpp
static char sRuntimeBlockList[kSmsBlockListMaxEntries][kSmsBlockListMaxNumberLen + 1];
static int  sRuntimeBlockListCount = 0;
```

**Why file-scope (not `setup()`-local)?** `SmsHandler` stores a pointer to the
block list via `setBlockList()`. A pointer into a `setup()`-local array would
dangle after `setup()` returns. File-scope `static` gives the array process
lifetime — the same reason `sBlockList` is file-scope in the current code.

Populate at startup (inside `setup()`, after `RealPersist` is initialized,
before `SmsHandler` is configured):

```cpp
{
    struct { int32_t count; char numbers[20][21]; } blob{};
    size_t got = realPersist.loadBlob("smsblist", &blob, sizeof(blob));
    if (got >= sizeof(int32_t) && blob.count >= 0 && blob.count <= 20)
    {
        sRuntimeBlockListCount = blob.count;
        memcpy(sRuntimeBlockList, blob.numbers,
               blob.count * (kSmsBlockListMaxNumberLen + 1));
    }
    Serial.printf("Runtime SMS block list: %d entr%s\n",
                  sRuntimeBlockListCount,
                  sRuntimeBlockListCount == 1 ? "y" : "ies");
}
```

### 5. `isBlocked` check in `SmsHandler` (Option C — dual call, no merge)

The prompt documents three options for how `SmsHandler` consults both the
compile-time and runtime lists. **This RFC recommends Option C: call
`isBlocked()` twice in `handleSmsIndex`, once per list.** No new setter, no
merged array, no change to `SmsHandler`'s interface.

The existing check in `handleSmsIndex` (immediately after PDU parse):

```cpp
if (blockList_ && isBlocked(pdu.sender.c_str(), blockList_, blockListCount_))
{
    // ... delete and return
}
```

Extend to:

```cpp
if ((blockList_     && isBlocked(pdu.sender.c_str(), blockList_,        blockListCount_))  ||
    (runtimeList_   && isBlocked(pdu.sender.c_str(), runtimeList_,      runtimeListCount_)))
{
    Serial.print("SMS from blocked sender ");
    Serial.print(pdu.sender);
    Serial.println(", deleting silently.");
    modem_.sendAT("+CMGD=" + String(idx));
    modem_.waitResponseOk(1000UL);
    return;
}
```

Add `setRuntimeBlockList(list, count)` setter and private members
`runtimeList_` / `runtimeListCount_` to `SmsHandler`:

```cpp
// sms_handler.h
void setRuntimeBlockList(const char (*list)[kSmsBlockListMaxNumberLen + 1], int count)
{
    runtimeList_ = list;
    runtimeListCount_ = count;
}

private:
    const char (*runtimeList_)[kSmsBlockListMaxNumberLen + 1] = nullptr;
    int runtimeListCount_ = 0;
```

In `main.cpp`, after calling `smsHandler.setBlockList(...)`:

```cpp
smsHandler.setRuntimeBlockList(sRuntimeBlockList, sRuntimeBlockListCount);
```

**Why Option C over A (two setters) and B (merged array)?**

- Option A is also clean but requires `SmsHandler` to know semantically that
  one list is compile-time and the other is runtime — unnecessary coupling.
- Option B requires rebuilding a 40-entry merged array in `main.cpp` on every
  `/block` and `/unblock` and re-calling `setBlockList()` each time. More
  code churn, and the merged array adds 420 bytes of stack-or-heap pressure
  at the rebuild site.
- Option C: `SmsHandler` gains one new setter + two new private members. The
  dual-call in `handleSmsIndex` is two `isBlocked()` calls on the same line
  connected with `||`. The lists stay separate in memory and in the serial
  log. The `/blocklist` command reply (built in the lambda) can still report
  them under separate headings. Total code diff is minimal.

The `AT+CMGD` deletion after a match is unconditional in both paths — same
requirement as RFC-0018: omitting the deletion causes the blocked number's
messages to accumulate on the SIM and replay at every reboot via
`sweepExistingSms`. This must not regress.

### 6. `SmsBlockMutatorFn` lambda in `main.cpp`

The lambda closes over `allowedIds`, `allowedIdCount`, `sRuntimeBlockList`,
`sRuntimeBlockListCount`, `sBlockList`, `sBlockListCount`, `realPersist`, and
`smsHandler`. It is passed to `TelegramPoller` as the 9th constructor
argument.

```cpp
[](int64_t callerId, const String &cmd, const String &number, String &reason) -> bool
{
    // Admin check for mutating commands.
    bool isAdmin = false;
    for (int i = 0; i < allowedIdCount; i++)
        if (callerId == allowedIds[i]) { isAdmin = true; break; }

    // --- /blocklist: any authorized user ---
    if (cmd == "list")
    {
        String reply = "Compile-time block list (" + String(sBlockListCount) + "):\n";
        for (int i = 0; i < sBlockListCount; i++)
            reply += String("  ") + sBlockList[i] + "\n";
        reply += "\nRuntime block list (" + String(sRuntimeBlockListCount) + "):\n";
        for (int i = 0; i < sRuntimeBlockListCount; i++)
            reply += String("  ") + sRuntimeBlockList[i] + "\n";
        if (sRuntimeBlockListCount == 0 && sBlockListCount == 0)
            reply = "(No numbers blocked)";
        reason = reply;
        return true;
    }

    // Mutating commands require admin.
    if (!isAdmin)
    {
        reason = "Admin access required.";
        return false;
    }

    // --- /block ---
    if (cmd == "block")
    {
        // Already in compile-time list?
        if (isBlocked(number.c_str(), sBlockList, sBlockListCount))
        {
            reason = number + " is already in the compile-time block list.";
            return false;
        }
        // Already in runtime list?
        if (isBlocked(number.c_str(), sRuntimeBlockList, sRuntimeBlockListCount))
        {
            reason = number + " is already in the runtime block list.";
            return false;
        }
        // Full?
        if (sRuntimeBlockListCount >= kSmsBlockListMaxEntries)
        {
            reason = "Runtime block list full (max 20 entries). Use /unblock to remove one first.";
            return false;
        }
        // Number too long?
        if (number.length() > kSmsBlockListMaxNumberLen)
        {
            reason = "Number too long (max 20 characters).";
            return false;
        }
        // Add to in-memory list.
        memcpy(sRuntimeBlockList[sRuntimeBlockListCount],
               number.c_str(), number.length() + 1); // includes NUL
        sRuntimeBlockListCount++;
        smsHandler.setRuntimeBlockList(sRuntimeBlockList, sRuntimeBlockListCount);
        // Persist.
        struct { int32_t count; char numbers[20][21]; } blob{};
        blob.count = sRuntimeBlockListCount;
        memcpy(blob.numbers, sRuntimeBlockList,
               sRuntimeBlockListCount * (kSmsBlockListMaxNumberLen + 1));
        realPersist.saveBlob("smsblist", &blob, sizeof(blob));
        Serial.printf("SMS block list: added %s (%d runtime entries)\n",
                      number.c_str(), sRuntimeBlockListCount);
        return true;
    }

    // --- /unblock ---
    if (cmd == "unblock")
    {
        // Find in runtime list.
        int found = -1;
        for (int i = 0; i < sRuntimeBlockListCount; i++)
            if (strcmp(sRuntimeBlockList[i], number.c_str()) == 0) { found = i; break; }
        if (found < 0)
        {
            if (isBlocked(number.c_str(), sBlockList, sBlockListCount))
                reason = number + " is in the compile-time list and cannot be removed at runtime.";
            else
                reason = number + " is not in the runtime block list.";
            return false;
        }
        // Compact: shift entries above found down by one.
        for (int i = found; i < sRuntimeBlockListCount - 1; i++)
            memcpy(sRuntimeBlockList[i], sRuntimeBlockList[i + 1], kSmsBlockListMaxNumberLen + 1);
        memset(sRuntimeBlockList[sRuntimeBlockListCount - 1], 0, kSmsBlockListMaxNumberLen + 1);
        sRuntimeBlockListCount--;
        smsHandler.setRuntimeBlockList(sRuntimeBlockList, sRuntimeBlockListCount);
        // Persist.
        struct { int32_t count; char numbers[20][21]; } blob{};
        blob.count = sRuntimeBlockListCount;
        memcpy(blob.numbers, sRuntimeBlockList,
               sRuntimeBlockListCount * (kSmsBlockListMaxNumberLen + 1));
        realPersist.saveBlob("smsblist", &blob, sizeof(blob));
        Serial.printf("SMS block list: removed %s (%d runtime entries)\n",
                      number.c_str(), sRuntimeBlockListCount);
        return true;
    }

    reason = "Unknown command.";
    return false;
}
```

**Important detail:** `smsHandler.setRuntimeBlockList(...)` is called
immediately after the in-memory mutation and before `saveBlob`. If `saveBlob`
were called first and the device lost power before `setRuntimeBlockList`, the
NVS and in-memory state would diverge until the next reboot (harmless —
the reboot loads from NVS). Calling `setRuntimeBlockList` first ensures the
live filter is updated even if the NVS write is delayed or fails. Either order
is acceptable; the above ordering prioritizes immediate enforcement over
persistence ordering.

### 7. Number normalization trap (carry-forward from RFC-0018)

`pdu.sender` is whatever the modem decoded from the PDU address field. The
same number may arrive as `10086` (national format) or `+8610086` (E.164) on
different messages or from different carriers. The runtime block list performs
exact `strcmp` matching — no normalization.

**Discovery path:** the serial monitor always prints the raw `pdu.sender` for
every incoming SMS (at the `Serial.print` in `handleSmsIndex` before the
block check), even for numbers already in the block list. This is the
recommended way to discover which format your carrier uses before calling
`/block`. The `/debug` bot command shows the same information via the
`SmsDebugLog`, but only for messages NOT already blocked (blocked messages do
not reach the debug log entry capture). **The serial monitor is the definitive
discovery path for numbers that are already blocked.**

Document this in the confirmation reply for `/block`:

> "Blocked: +8610086. Note: matching is exact. If the same sender arrives as
> 10086 (no country code), block that form too. Check the serial log to see
> the exact format your carrier sends."

A short version in the bot reply is sufficient; the full explanation lives
here and in `secrets.h.example`.

### 8. Interaction with compile-time list

- `/block` refuses to add a number already in the compile-time `sBlockList`.
  This prevents confusion about which list "owns" an entry and avoids silent
  duplication (the duplicate would waste a runtime slot without changing
  behaviour).
- `/unblock` refuses to remove a number from the compile-time list. The reply
  explains that compile-time entries require a reflash. The number may still
  be in the runtime list independently; if the user somehow has both, the
  runtime entry can be removed — the compile-time entry will still block the
  number.
- `/blocklist` shows both lists under separate headings:
  ```
  Compile-time block list (2):
    10086
    10010

  Runtime block list (1):
    +8613812345678
  ```

### 9. Capacity

- Runtime list: 20 entries (`kSmsBlockListMaxEntries`).
- Compile-time list: 20 entries (same constant).
- Combined maximum blocked: 40 distinct entries checked per incoming SMS.

The combined 40-entry scan is two sequential linear scans of 20 entries each.
At ESP32 CPU speeds this is negligible even for sustained SMS bursts.

If 20 runtime slots prove insufficient, increase `kSmsBlockListMaxEntries`
(or introduce a separate `kSmsRuntimeBlockListMaxEntries`) and update the NVS
blob size accordingly. The format version is implicit in the blob's fixed size;
a size mismatch on load (`got != sizeof(blob)`) should be treated as "absent"
and the old data discarded.

### 10. Error reply for `/block` suffix ambiguity

The dispatch uses `lower == "/block" || lower.startsWith("/block ")` (with a
trailing space after the command), identical to the `/adduser` / `/removeuser`
pattern. This avoids accidentally matching a hypothetical `/blockall` or
`/blocklist` command — `/blocklist` is matched earlier in the dispatch chain
before the `/block` check.

**Order matters:** `/blocklist` must be checked before `/block` in the
dispatch chain. If `/block` is checked first, `lower == "/blocklist"` would
fail the `lower.startsWith("/block ")` test (no trailing space) but could
in principle match a future `/blocklist ...` form. The safe ordering is:

```
1. /blocklist      (exact match, checked first)
2. /block <arg>    (prefix match with trailing space, checked second)
3. /unblock <arg>  (prefix match with trailing space)
```

### 11. Fallback help text update

The fallback error reply in `processUpdate` (currently:
`"Reply to a forwarded SMS to send a response. Use /debug for the SMS
diagnostic log, /status for device health."`) should be extended to mention
the new commands when `smsBlockMutator_` is set:

```
Reply to a forwarded SMS to send a response.
Commands: /debug, /status, /listusers, /blocklist, /block <num>, /unblock <num>
```

List only the commands that are actually configured (non-null mutators). This
avoids advertising `/blocklist` when `smsBlockMutator_` is nullptr (e.g. in
a build that doesn't wire the lambda).

## Notes for handover

### What to implement

1. **`telegram_poller.h`**: add `SmsBlockMutatorFn` type alias and 9th
   constructor parameter `smsBlockMutator = nullptr`. Add private member
   `smsBlockMutator_`.

2. **`telegram_poller.cpp`**: add dispatch for `/blocklist`, `/block <arg>`,
   `/unblock <arg>` in the non-reply-message branch of `processUpdate`. Must
   check `/blocklist` before `/block` (see §10). Update fallback help text
   (§11).

3. **`sms_handler.h`**: add `setRuntimeBlockList(list, count)` setter and
   private members `runtimeList_` / `runtimeListCount_` (both default to
   `nullptr` / `0`). Include `sms_block_list.h` (already included via
   `sms_block_list.h` transitively, but make it explicit if needed).

4. **`sms_handler.cpp`**: extend the existing `isBlocked` check to OR in the
   runtime list check. One line change (see §5 diff sketch).

5. **`main.cpp`**: add `sRuntimeBlockList[20][21]` and `sRuntimeBlockListCount`
   file-scope statics. Add NVS load block in `setup()`. Add
   `smsHandler.setRuntimeBlockList(...)` call. Wire the `SmsBlockMutatorFn`
   lambda. Pass it as the 9th argument to `TelegramPoller`.

### File-scope lifetime requirement

Both `sRuntimeBlockList` and `sBlockList` (the compile-time array) must be
file-scope `static` in `main.cpp`. `SmsHandler` stores raw pointers to these
arrays; a `setup()`-local array would dangle after `setup()` returns. This is
the same requirement as the compile-time list in RFC-0018. Do not move either
array inside `setup()`.

### Test approach

**New file: `test/test_native/test_sms_block_mutator.cpp`**

Test the `SmsBlockMutatorFn` lambda in isolation (host-runnable, no hardware).
Since the lambda closes over `main.cpp` statics, extract the logic into a
free function or a testable struct for the native env, or test via the
`TelegramPoller` integration test path (see below). Cases:

- `/block` adds an entry; subsequent `isBlocked` returns true.
- `/block` duplicate (already in runtime list) returns false with reason.
- `/block` number already in compile-time list returns false with reason.
- `/block` when runtime list is full (20 entries) returns false with reason.
- `/block` number longer than 20 chars returns false with reason.
- `/unblock` removes an existing runtime entry; subsequent `isBlocked` returns false.
- `/unblock` a number not in the runtime list returns false with reason.
- `/unblock` a compile-time-only number returns false with reason indicating reflash needed.
- `/blocklist` returns both lists, each under separate headings, with correct counts.
- `/blocklist` when both lists are empty returns "(No numbers blocked)".
- Non-admin caller cannot `/block` or `/unblock`; can call `/blocklist`.

**Additions to `test/test_native/test_telegram_poller.cpp`**

Dispatch tests for the three new commands (parallel to the existing
`/adduser` / `/removeuser` tests):

- `/blocklist` dispatches to `smsBlockMutator_` with `cmd == "list"`.
- `/block 10086` dispatches to `smsBlockMutator_` with `cmd == "block"`,
  `number == "10086"`.
- `/block` (no argument) returns usage error without calling mutator.
- `/unblock 10086` dispatches to `smsBlockMutator_` with `cmd == "unblock"`,
  `number == "10086"`.
- `/unblock` (no argument) returns usage error without calling mutator.
- `/blocklist` is matched before `/block` (send `/blocklist` and verify
  `cmd == "list"` not `cmd == "block"` with `arg == "list"`).
- When `smsBlockMutator_` is `nullptr`, all three commands return
  "not configured" messages without crashing.

**Additions to `test/test_native/test_sms_handler.cpp`**

Two new integration tests (parallel to the RFC-0018 blocked-sender tests):

- `test_runtime_blocked_single_part_not_forwarded_sim_slot_deleted`: inject
  a runtime block list containing `"99999"`, send a single-part SMS from
  `"99999"`, assert `bot.callCount() == 0`, assert `+CMGD=<idx>` was sent,
  assert `consecutiveFailures() == 0`.
- `test_runtime_blocked_concat_fragment_not_buffered_sim_slot_deleted`:
  inject a runtime block list, send a concat fragment from the blocked number,
  assert it never enters the reassembly buffer (`concatKeyCount() == 0`),
  assert `+CMGD=<idx>` was sent, assert `bot.callCount() == 0`.

Use `FakeModem::sentCommands()` for AT command assertions (established
pattern from RFC-0018).

### Known limitation: not reflected in running tests immediately

After `/block` updates the in-memory `sRuntimeBlockList` and calls
`smsHandler.setRuntimeBlockList(...)`, any SMS arriving in the same `loop()`
iteration (before the next `handleSmsIndex` call) will already see the updated
list. There is no race: `loop()` is single-threaded on the ESP32's Arduino
runtime. `/block` takes effect immediately for all subsequent SMS.

### Incremental delivery order

If you implement in order:
1. `SmsHandler` setter + dual `isBlocked` check (pure addition, no behaviour change until `setRuntimeBlockList` is called)
2. NVS load in `main.cpp` + `setRuntimeBlockList` call (loads empty list on first boot, so no behaviour change until a `/block` is sent)
3. `TelegramPoller` dispatch + `SmsBlockMutatorFn` lambda (activates the bot commands)

Each step is independently deployable and testable. Step 1 alone passes all
existing tests unchanged. Step 2 alone logs "Runtime SMS block list: 0 entries"
at boot. Step 3 enables the user-facing feature.
