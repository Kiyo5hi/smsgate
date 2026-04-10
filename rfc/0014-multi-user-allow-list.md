---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0014: Multi-user allow list

## Motivation

`TELEGRAM_CHAT_ID` in `src/secrets.h` is a single `int64_t` baked into the
firmware at compile time. The authorization gate in `TelegramPoller` passes or
fails every incoming update against that one value:

```cpp
// main.cpp, inside the AuthFn lambda passed to TelegramPoller
return fromId != 0 && fromId == kAllowedChatId;
```

For a household or small-team scenario this is a hard blocker. A spouse,
family member, or colleague cannot reply to a forwarded SMS, run `/status`,
or use `/debug` — they receive no acknowledgment and the bot silently drops
their messages. The current design also means that if the single authorized
user changes their Telegram account, the firmware must be reflashed.

A minimal multi-user allow list removes this constraint without adding
runtime management overhead.

## Current state

### Authorization hook

`TelegramPoller` already accepts an `AuthFn` callback
(`std::function<bool(int64_t fromId)>`) as a constructor parameter. The
production wiring in `main.cpp` constructs a single-capture lambda:

```cpp
static const int64_t kAllowedChatId = parseChatIdAsInt64(TELEGRAM_CHAT_ID);

// Inside TelegramPoller construction in setup():
[](int64_t fromId) -> bool {
    return fromId != 0 && fromId == kAllowedChatId;
},
```

The `AuthFn` interface is already extensible. No change to `TelegramPoller`
itself is needed; only the lambda and the `secrets.h` contract change.

### Single destination for outbound forwards

`RealBotClient::sendMessage` and `sendMessageReturningId` send to the chat ID
configured in `telegram.cpp` at `setupTelegramClient()` / construction time.
Inbound SMS forwards currently go to exactly one chat (the single configured
ID). The multi-user RFC must decide what happens to this fan-out once the
allow list has more than one entry.

### Existing NVS layout

`RealPersist` uses namespace `"tgsms"` with keys `"uid"` (int32 watermark)
and `"rtm"` (reply-target blob). There is no existing slot for a dynamic
allow list.

## Plan

### Option A: Compile-time list in `secrets.h` (recommended)

Replace the single `TELEGRAM_CHAT_ID` define with a comma-separated
`TELEGRAM_CHAT_IDS` define:

```cpp
// src/secrets.h.example
// Comma-separated list of Telegram user IDs that may interact with the bot.
// Maximum 10 entries. The first ID is the admin (receives incoming SMS forwards).
// Example: "111111111,222222222,333333333"
#define TELEGRAM_CHAT_IDS "0000000000"
```

At startup, `main.cpp` parses the string into a `std::array<int64_t, 10>`
(or equivalent fixed-size structure — see "Max list size" below):

```cpp
// Parse at startup; result has process lifetime.
static std::array<int64_t, 10> allowedIds{};
static int allowedIdCount = 0;

static void parseAllowedIds(const char *csv) {
    char buf[256];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok = strtok(buf, ",");
    while (tok && allowedIdCount < 10) {
        int64_t id = (int64_t)strtoll(tok, nullptr, 10);
        if (id != 0) allowedIds[allowedIdCount++] = id;
        tok = strtok(nullptr, ",");
    }
}
```

The `AuthFn` lambda captures `allowedIds` and `allowedIdCount` by reference
(both are file-scope statics with process lifetime — same as `kAllowedChatId`
today):

```cpp
[](int64_t fromId) -> bool {
    if (fromId == 0) return false;
    for (int i = 0; i < allowedIdCount; i++) {
        if (fromId == allowedIds[i]) return true;
    }
    return false;
},
```

The backward-compatible migration path for existing `secrets.h` files: keep
`TELEGRAM_CHAT_ID` as an alias or document that it must be renamed to
`TELEGRAM_CHAT_IDS`. The existing single-ID value is valid as-is (no comma
needed for a single entry).

**Pros:**
- Trivial code delta. No new TU, no NVS changes, no runtime commands.
- List is auditable: visible in source control alongside other credentials.
- No race conditions, no state to corrupt on power loss.
- Works even if NVS is wiped or corrupt.

**Cons:**
- Requires reflash to add or remove a user.
- Maximum size is fixed at compile time (acceptable for the household/team
  use case; see "Max list size" below).

### Option B: Runtime allow list via Telegram bot commands (follow-up)

The admin (first entry in the compiled list, or the original `TELEGRAM_CHAT_ID`
holder) can send bot commands to manage a secondary list stored in NVS:

- `/adduser <id>` — add a Telegram user ID to the runtime allow list
- `/removeuser <id>` — remove a user ID from the runtime allow list
- `/listusers` — reply with the current runtime list

The runtime list would be persisted as a new NVS key (e.g. `"ulist"`) under
the existing `"tgsms"` namespace, as a small fixed-size blob of `int64_t[]`.
Authorization then checks: compile-time list OR runtime list.

The "superuser" concept is explicit: only a user whose ID appears in the
compile-time `TELEGRAM_CHAT_IDS` list may invoke `/adduser` / `/removeuser`.
A user in the runtime NVS list only may NOT manage the allow list.

**Pros:**
- No reflash needed to add household members.
- Admin can revoke access remotely.

**Cons:**
- Substantially more code: three new command handlers in `TelegramPoller`,
  a new NVS blob, a merge of compile-time and runtime predicates.
- NVS runtime list can diverge from source-controlled `secrets.h`.
- Adds a command-parsing surface to `processUpdate`, which already handles
  `/debug` and `/status` via a simple string comparison; user management
  commands are more complex (require argument parsing and validation).
- The command handlers must be behind the same admin-only authorization gate
  to prevent a runtime-listed user from escalating their own privileges.

**Recommendation: implement Option A now; leave Option B as a follow-up.**
The household/team use case is met by Option A as long as the number of
users is small and stable (both are typically true). Option B adds
meaningful complexity for a marginal convenience gain and can be layered
on top of Option A's compile-time list at any time.

### Max list size: 10 entries

Ten users is more than sufficient for the household/team target. A
`std::array<int64_t, 10>` is 80 bytes on the stack — no heap allocation,
no dynamic sizing. The parser silently truncates at 10 and logs a warning
if the CSV has more tokens.

If the use case ever grows beyond 10 (e.g. an office deployment), Option B
(NVS-backed dynamic list) is the right answer, not a larger compile-time
array.

### Notification routing: who receives incoming SMS forwards?

With a multi-user allow list, all authorized users can reply and use bot
commands. But `RealBotClient::sendMessage` and `sendMessageReturningId` send
to a single chat ID — the one set in `telegram.cpp` at construction time.
Three options:

**Option N1: Admin-only forwarding (recommended for now)**

Keep the current behavior: SMS forwards go to a single designated chat (the
first entry in `TELEGRAM_CHAT_IDS`, hereafter "the admin"). Other authorized
users can reply and use commands but do not receive unsolicited forwards.

This is the zero-change-to-RealBotClient path. The `kAllowedChatId` single
value for outbound messages becomes the first element of the parsed array.
Only `main.cpp`'s `AuthFn` lambda changes.

**Option N2: Broadcast to all authorized users**

`RealBotClient` gains a `sendMessageBroadcast(text)` variant that iterates
over the allow list and sends the same message to each chat ID. The
`reply_to_message_id` returned from the first successful send is stored in
the ring buffer (or a mapping from each per-user message ID back to the SMS
sender is stored — significantly more complex).

Complexity cost: the `ReplyTargetMap` currently maps a single Telegram
message ID to an SMS sender phone. Broadcast means one SMS forward produces
N Telegram messages (one per user); any of those N messages can be replied
to. The ring buffer would need to store N entries per SMS forward, or the
map would need to accept any of the N message IDs as valid reply targets.

**Option N3: Group chat**

The admin creates a Telegram group, adds all family members and the bot, and
sets `TELEGRAM_CHAT_IDS` to the group's (negative) chat ID. Forwarding stays
single-target; all members see all messages; replies are already multi-user
because Telegram's own group reply threading handles it.

This requires zero code changes — it works with Option A as specified, since
group chat IDs are valid `int64_t` values. The `AuthFn` lambda would check
`fromId` against the group ID or — for group messages — `chat.id` rather
than `from.id`. The `TelegramUpdate` struct already carries `chatId` as a
fallback: `TelegramPoller::processUpdate` checks `auth_(u.fromId)` (line 44
of `telegram_poller.cpp`).

**Recommendation: implement Option N1 for the inbound forwarding path in
this RFC.** The `TELEGRAM_CHAT_IDS` first-entry as the forward target is a
clear, simple convention. Document Option N3 (group chat) as a zero-code
alternative for households where everyone should see every SMS. Defer Option
N2 (true broadcast) because the ring-buffer fan-out makes it a substantially
larger change.

## Implementation summary (Option A + Option N1)

Files touched:

1. **`src/secrets.h.example`** — rename `TELEGRAM_CHAT_ID` to
   `TELEGRAM_CHAT_IDS`, update the comment, add migration note.

2. **`src/main.cpp`**:
   - Add `parseAllowedIds(TELEGRAM_CHAT_IDS)` call at file scope (before
     `setup()`).
   - Replace `kAllowedChatId` with `allowedIds[0]` for the outbound forward
     target (passed wherever `kAllowedChatId` was used to identify the
     forwarding destination in `RealBotClient` or `setupTelegramClient`).
   - Replace the single-capture `AuthFn` lambda with the multi-entry loop
     shown above.
   - Log the parsed ID list at startup for diagnostics.

3. **`src/telegram.cpp` / `src/telegram.h`** — wherever the chat ID is
   currently hardcoded or derived from `TELEGRAM_CHAT_ID` at construction
   time, replace with a parameter or a call to `allowedIds[0]`. (Confirm
   the exact wiring during implementation; this file was not fully audited
   for this RFC.)

4. **`src/secrets.h`** (each developer's local file, gitignored) — must
   rename the define manually. A `#ifdef TELEGRAM_CHAT_ID` / `#ifndef
   TELEGRAM_CHAT_IDS` compatibility shim in `main.cpp` can ease migration:

   ```cpp
   #ifndef TELEGRAM_CHAT_IDS
   #  ifdef TELEGRAM_CHAT_ID
   #    define TELEGRAM_CHAT_IDS TELEGRAM_CHAT_ID
   #  else
   #    error "Define TELEGRAM_CHAT_IDS in secrets.h (see secrets.h.example)"
   #  endif
   #endif
   ```

No changes to `TelegramPoller`, `IPersist`, `RealPersist`, `ReplyTargetMap`,
`SmsHandler`, or `CallHandler`.

## Notes for handover

### Interaction with `/status` and `/debug`

Both commands are handled in `TelegramPoller::processUpdate` after the
`AuthFn` gate. No change is needed: any authorized user (anyone whose
`fromId` passes the multi-entry `AuthFn`) can invoke them. The status
message is sent via `bot_.sendMessage(statusFn_())`, which sends to the
single configured chat ID (the admin's chat). This is a minor UX issue: if
user B sends `/status`, the reply goes to user A's chat. This is a
limitation of the single-destination `IBotClient::sendMessage` design.

For a complete per-user reply path, `IBotClient::sendMessage` would need to
accept an optional target chat ID, defaulting to the configured admin ID.
That is a follow-up, not a blocker for this RFC.

### Interaction with the reply-target map

`ReplyTargetMap` maps `telegram_message_id -> sms_sender_phone`. The message
ID comes from `IBotClient::sendMessageReturningId`, which today sends to the
admin chat and returns that chat's message ID. With Option N1 (admin-only
forwarding), this does not change: there is still exactly one message ID per
SMS forward.

Any authorized user can subsequently reply to that forwarded message if they
are in the same chat as the admin (i.e. Option N3 — group). In a DM-only
setup with Option N1, only the admin sees the forwarded messages and
therefore only the admin can reply. Other authorized users can still send
independent messages to the bot (which will be routed through the
`replyToMessageId == 0` path and get the "reply to a forwarded SMS" error),
but cannot initiate SMS replies.

This is expected and acceptable for the household/team use case where a
single person (the admin) manages the SMS bridge and others use `/status` or
`/debug` only.

### Interaction with the `AuthFn` in CallHandler

`CallHandler` (call notify, RFC-0005) does not have an `AuthFn`. It sends
call notifications to the single `IBotClient` destination unconditionally.
This RFC does not change that behavior — call notifications continue to go
to the admin (first `TELEGRAM_CHAT_IDS` entry).

### The `chatId` vs `fromId` distinction in group chats

When a message is sent inside a Telegram group, `from.id` is the individual
user's ID and `chat.id` is the group's ID. The current `TelegramUpdate`
struct (parsed in `RealBotClient::pollUpdates`) carries both `fromId` and
`chatId`. `processUpdate` passes `u.fromId` to `auth_` (line 44 of
`telegram_poller.cpp`).

For Option N3 (group chat), the admin might configure a group ID as the
sole entry in `TELEGRAM_CHAT_IDS`. The `AuthFn` would then need to check
`u.chatId` instead of `u.fromId` (or both). The current code checks only
`fromId`. If Option N3 is pursued, `processUpdate` must pass `u.chatId` as
a second argument to `auth_`, or `AuthFn` must be extended to accept both
values. This is a small change; document it when implementing.

### Test coverage

The multi-entry `AuthFn` is a pure lambda over a fixed array — easily
covered by the existing native test infrastructure. Suggested cases:

- Single entry, matching `fromId`.
- Single entry, non-matching `fromId`.
- Multiple entries, `fromId` matches first.
- Multiple entries, `fromId` matches last.
- Multiple entries, `fromId` matches none.
- `fromId == 0` (always rejected regardless of list contents).
- Empty list (no entries after parsing; everything rejected).
- CSV with leading/trailing whitespace around IDs.
- CSV with 11 entries (truncated to 10, warning logged).

These can be added to `test/test_native/` as a new test file
`test_allow_list.cpp` or folded into an existing test file if one is
suitable. No hardware is needed.

## Review

**verdict: approved-with-changes**

### Issues

- **BLOCKING — Double-define trap in the backward-compat shim.** The proposed
  shim is:
  ```cpp
  #ifndef TELEGRAM_CHAT_IDS
  #  ifdef TELEGRAM_CHAT_ID
  #    define TELEGRAM_CHAT_IDS TELEGRAM_CHAT_ID
  #  endif
  #endif
  ```
  If a developer copies `secrets.h.example`, renames `TELEGRAM_CHAT_ID` to
  `TELEGRAM_CHAT_IDS` as instructed, but also leaves the old line in place
  (easy to do), both macros will be defined. The shim's `#ifndef
  TELEGRAM_CHAT_IDS` guard fires first (the new name is already defined), so
  `TELEGRAM_CHAT_ID` is silently ignored — correct outcome. However, the
  _reverse_ mistake (defines only `TELEGRAM_CHAT_ID`, not `TELEGRAM_CHAT_IDS`)
  passes through the shim without any warning being emitted. The RFC's
  example shim is missing the `#else #error` branch that would catch a build
  where both are defined simultaneously (possible if an IDE auto-completes
  both lines). Add an `#elif defined(TELEGRAM_CHAT_ID) && defined(TELEGRAM_CHAT_IDS)`
  branch that `#error`s on the ambiguous case, or at a minimum document it as
  a gotcha in `secrets.h.example`.

- **BLOCKING — `telegram.cpp` hard-codes `chatID` and the RFC under-specifies
  the fix.** The RFC acknowledges this in the Implementation Summary (point 3)
  but phrases it as "confirm the exact wiring during implementation; this file
  was not fully audited". This is the most load-bearing change in the RFC and
  it is waved through. `doSendMessage()` in `telegram.cpp` does
  `doc["chat_id"] = chatID` where `chatID` is `TELEGRAM_CHAT_ID` cast to a
  string at file-scope. With Option N1 (admin-only forwarding), the fix is
  straightforward: pass `allowedIds[0]` (as a string or int64) to
  `RealBotClient` at construction or via a setter. But the RFC leaves the
  mechanism open ("replace with a parameter or a call to `allowedIds[0]`"),
  which means the implementer must make an API decision under time pressure.
  The recommended concrete fix: add a `void setAdminChatId(int64_t id)`
  method to `RealBotClient` (or pass it in the constructor), store it as a
  member, and serialize it into the JSON payload. This replaces the
  file-static `const char *chatID` and removes the last direct reference to
  `TELEGRAM_CHAT_ID` from `telegram.cpp`. The RFC should prescribe this
  before approval, not leave it to the implementer.

- **NON-BLOCKING — `/status` and `/debug` reply to the admin, not the
  requester.** The RFC correctly identifies this in "Interaction with
  `/status` and `/debug`" and calls it "a minor UX issue". It is, but the
  consequence is that a non-admin user who types `/status` gets no visible
  reply in their own chat — their message is silently acknowledged by the bot
  (the watermark advances, no error is returned) but the response lands in the
  admin's DM. From the non-admin user's perspective the bot is broken. This
  should be documented more prominently in the RFC and ideally gated: either
  restrict `/status` / `/debug` to admin-only (check `fromId ==
  allowedIds[0]` before dispatching), or park the fix with a clear TODO
  comment in `processUpdate`. Silently discarding the response is worse than
  an explicit "sorry, direct this to the admin" reply.

- **NON-BLOCKING — `std::array` vs `std::vector` (mentioned in the RFC,
  correctly resolved).** The RFC already prescribes `std::array<int64_t, 10>`
  (80 bytes, stack-allocated, no heap). The plan section is consistent with
  this. No change needed; just confirming the resolution is correct.

- **NON-BLOCKING — `strtok` is not re-entrant.** `parseAllowedIds` uses
  `strtok`, which modifies a local `buf[]` and uses a static internal pointer.
  On a single-core Arduino/ESP32 `setup()` context this is safe because no
  interrupt-driven caller could interleave, but it is worth a comment. If any
  future caller uses `strtok` elsewhere in the same TU, the calls will
  corrupt each other. Consider `strtok_r` (POSIX, available on ESP-IDF /
  newlib) or a manual `strchr`-based split for clarity.

- **NON-BLOCKING — Option B deferral is well-reasoned and acceptable.**
  The complexity analysis (three new command handlers, argument parsing,
  NVS blob, privilege separation) is accurate. There is no simpler path to
  runtime management that would avoid these costs — the cheapest possible
  version (one new NVS key, no `/listusers`) still requires argument parsing
  and an admin-only gate. The deferral is correct.

- **NON-BLOCKING — Security note absent.** The compiled-in allow list is
  readable from the firmware binary by anyone who extracts the flash. For this
  use case (household, Telegram IDs are not credentials) this is acceptable,
  but the RFC should include a one-sentence acknowledgment so a future
  reader does not assume the list is secret. Telegram user IDs are not
  secret by Telegram's own model (they appear in API payloads), so the
  practical risk is negligible.

- **NON-BLOCKING — `ReplyTargetMap` multi-user correctness is sound.**
  The concern is whether user B can reply to a message originally forwarded
  to user A. With Option N1 (admin-only forwarding), all forwarded messages
  go to the admin's DM only, and only the admin can reply to them — so the
  ring buffer continues to map exactly one `message_id` per SMS forward and
  the routing is correct. With Option N3 (group chat), the single forwarded
  message is visible to all group members, and any member's reply carries the
  same stable `reply_to_message_id` — the ring buffer lookup is by message ID
  regardless of who is replying, so routing is still correct. Neither path
  requires changes to `ReplyTargetMap`. The RFC's analysis (p. "Interaction
  with the reply-target map") is correct.

- **NON-BLOCKING — Scope is appropriate.** The core code change is: (a)
  replace one lambda (~3 lines) with a multi-entry loop (~6 lines), (b) add a
  `parseAllowedIds()` function (~15 lines), (c) wire `allowedIds[0]` into the
  outbound forward destination. Total net delta is on the order of 25-35 lines
  across two files (`main.cpp` and `telegram.cpp`), plus the `secrets.h.example`
  update. The RFC is correctly scoped — it does not bloat into Option N2 or
  Option B.

### Summary

The RFC is well-reasoned overall: the options are correctly enumerated, the
recommended choices (Option A + Option N1) are appropriately conservative, and
the deferral of runtime management is justified. Two blocking issues prevent
immediate implementation sign-off: the backward-compat shim needs an
explicit `#error` guard for the ambiguous double-define case, and the
`telegram.cpp` `chatID` wiring needs to be specified concretely (a
`setAdminChatId` setter or constructor parameter) rather than left open.
The non-blocking items are editorial or minor code quality points. Address
the two blocking items in the RFC text before implementation begins.

## Code Review

**verdict: approved**

### Issues

**BLOCKING — none.**

**NON-BLOCKING — `secrets.h.example` still has `TELEGRAM_CHAT_ID` active as the
default example, with `TELEGRAM_CHAT_IDS` commented out.** This is intentional
(backward-compat migration path) and the inline comment explains what to do. One
subtle consequence: the placeholder value `"0000000000"` is parsed by
`parseAllowedIds` as the integer 0, which is silently filtered (`if (id != 0)`),
so `allowedIdCount` will be 0 on a fresh flash with an unconfigured `secrets.h`.
The WARNING log at setup() line 436 fires in that case and makes the problem
visible. Acceptable, but worth noting in `secrets.h.example` that replacing the
placeholder is mandatory (not just cosmetic). The current comment says "fill in
the values" implicitly — a one-liner like `// Replace 0000000000 with your real
Telegram chat ID before flashing` would make the consequence explicit.

**NON-BLOCKING — `setAdminChatId` called with 0 when `allowedIdCount == 0`
(main.cpp line 443).** The guard `allowedIdCount > 0 ? allowedIds[0] : 0` is
present and correct — it avoids reading uninitialized memory. When
`adminChatId_` is 0, `doSendMessage` will serialize `chat_id: 0` into the JSON
payload; Telegram will reject this with a 400 or an `ok:false` response, which
`doSendMessage` treats as failure (returns 0). The boot banner `sendMessage`
will fail silently (no crash), and the WARNING log already printed makes the
cause clear. Behavior is safe; adding a log line inside `doSendMessage` when
`adminChatId_ == 0` (before the HTTP round-trip) would avoid a wasted TLS
request, but this is not a blocker.

**NON-BLOCKING — `adminChatId_` ArduinoJson serialization is correct.**
`doc["chat_id"] = adminChatId_` where `adminChatId_` is `int64_t` — ArduinoJson
v6 serializes this as a JSON number (not a quoted string). The inline comment on
that line confirms this. No issue.

**NON-BLOCKING — No explicit test for a trailing-comma CSV (`"111,"`).**
`parseAllowedIds("111,", ...)` is handled correctly: after processing "111", `p`
advances to `comma+1` which points at `'\0'`, and the outer `while (*p && ...)`
terminates. The behavior is correct but there is no dedicated test case for it.
The existing `test_allow_list_skips_zero_tokens` covers the empty-token-between-
commas case (`"0,999,,888"`) but not the trailing-comma case. Add one test if
future refactors touch the parsing loop.

**NON-BLOCKING — No explicit test for a fully non-numeric token (`"abc"` or
`"111,abc,222"`).** `strtoll("abc", nullptr, 10)` returns 0, which is filtered
by the `if (id != 0)` guard, so the token is silently skipped. This is the
correct behavior (same as an empty token), but a test asserting it would serve
as documentation. Not blocking.

**NON-BLOCKING — `test_auth_fromId_zero_always_rejected` uses `ids[] = {0, 111, 222}`.**
Since `parseAllowedIds` never stores 0 in the array, this scenario cannot arise
from a real CSV, but it is still a valid test of the AuthFn's explicit early-
return guard (`if (fromId == 0) return false`). The guard is present in the
main.cpp lambda (line 461) and mirrored in the test's `authFn` helper. Correct
and useful as defense-in-depth documentation.

### Resolved blocking items from the pre-implementation Review section

Both blocking items identified before implementation are resolved:

1. **Double-define guard**: `#if defined(TELEGRAM_CHAT_ID) && defined(TELEGRAM_CHAT_IDS) #error` is at main.cpp lines 43–45, immediately after the include block and before any code. It fires at compile time for the ambiguous case. The shim below it correctly defines `TELEGRAM_CHAT_IDS` from `TELEGRAM_CHAT_ID` when only the old name is present, and emits a hard `#error` when neither is defined.

2. **`telegram.cpp` `chatID` wiring**: `chatID` (the old file-static string) has been removed. `doSendMessage` now uses `adminChatId_` (an `int64_t` member of `RealBotClient`), set via `setAdminChatId()` in `setup()` after parsing the allow list. The `setAdminChatId` setter is declared in `telegram.h` and called in main.cpp line 443 with the correct guard (`allowedIdCount > 0 ? allowedIds[0] : 0`).

### Summary

The implementation is clean and correct. Both blocking items from the
pre-implementation review have been addressed as specified. The `strchr`-based
parser in `allow_list.h` avoids the `strtok` re-entrancy concern noted as
NON-BLOCKING, handles all identified edge cases correctly, and is covered by 15
host-side Unity tests. The `adminChatId_` member properly replaces the old
file-static `chatID` string, serializes as a JSON number, and is guarded against
the zero case in `setup()`. The double-define `#error` is in the right place and
cannot be bypassed. No source changes are required.
