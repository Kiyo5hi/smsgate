---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0019: Runtime user management via bot commands

## Motivation

RFC-0014 introduced a compile-time multi-user allow list (`TELEGRAM_CHAT_IDS`)
and explicitly deferred a runtime management path:

> **Option B: Runtime allow list via Telegram bot commands (follow-up)**
> The admin can send bot commands to manage a secondary list stored in NVS:
> `/adduser <id>`, `/removeuser <id>`, `/listusers`.
> **Recommendation: implement Option A now; leave Option B as a follow-up.**

The compile-time list covers households where the user roster is small and
stable. When it is not — a new family member gets a Telegram account, a
flatmate leaves, a temporary helper needs SMS access for a week — the only
current remedy is editing `secrets.h` and reflashing. The hardware may be
physically inaccessible (mounted in a cabinet) or the person managing the
device may not have a build environment handy. A runtime management path
removes this friction.

The architecture required to implement Option B has been fully laid in by
subsequent RFCs:

- RFC-0016 added `IBotClient::sendMessageTo(chatId, text)`, so command
  responses can be addressed to the requester rather than the admin.
- `TelegramPoller::processUpdate` already dispatches `/debug` and `/status`
  as string comparisons after the `AuthFn` gate — the new commands slot into
  the same dispatch table.
- `IPersist` / `RealPersist` (namespace `"tgsms"`) have an established
  pattern for blobs via `loadReplyTargets` / `saveReplyTargets`; adding a
  parallel key for the user list follows the same shape.

## Current state

### Authorization

`main.cpp` holds two file-scope statics:

```cpp
static int64_t allowedIds[10] = {};
static int     allowedIdCount = 0;
```

Populated at startup by `parseAllowedIds(TELEGRAM_CHAT_IDS, allowedIds, 10)`
(from `src/allow_list.h`). The `AuthFn` lambda passed to `TelegramPoller`
checks `fromId` against all entries with a linear scan.

`realBot.setAdminChatId(allowedIds[0])` is called in `setup()`. The admin
chat ID is fixed at boot and is always the first compile-time entry.

### Command dispatch

`TelegramPoller::processUpdate` (in `telegram_poller.cpp`):

1. Passes `u.fromId` through `auth_`. Unauthorized updates are dropped; the
   watermark still advances.
2. Compares `lower == "/debug"` and `lower == "/status"` for built-in commands,
   replies via `bot_.sendMessageTo(u.chatId, ...)`.
3. Tries the reply-target-map path if the update has `reply_to_message_id` set.
4. Falls through to a help message otherwise.

There is no `AdminFn` predicate and no argument-parsing infrastructure yet.

### NVS layout

`RealPersist` (namespace `"tgsms"`) currently uses:

- Key `"uid"` — `int32_t` last Telegram update_id watermark.
- Key `"rtm"` — opaque blob for the `ReplyTargetMap` ring buffer.

No slot exists for a runtime user list.

### IPersist interface

`IPersist` currently exposes four methods: `loadLastUpdateId`,
`saveLastUpdateId`, `loadReplyTargets`, `saveReplyTargets`. A new key for the
runtime user list needs two parallel methods or a generic blob-by-key API.
The simplest approach that avoids changing `IPersist` for every new NVS key
is to add a `loadUserList` / `saveUserList` method pair — the same pattern
already used for the reply-target map. An alternative is a generic
`loadBlob(key, buf, size)` / `saveBlob(key, buf, size)` pair; this RFC
recommends the generic form to prevent the interface from growing
indefinitely as more NVS keys are added (see "Plan" §3 below).

## Plan

### 1. Commands

Three new bot commands, all dispatched inside `TelegramPoller::processUpdate`
after the existing `AuthFn` gate:

| Command | Argument | Who may call |
|---------|----------|--------------|
| `/adduser <id>` | A Telegram user ID (int64) | Admin only |
| `/removeuser <id>` | A Telegram user ID (int64) | Admin only |
| `/listusers` | none | Any authorized user |

"Admin" means `u.fromId` is present in the compile-time `allowedIds[]` array.
This is an additional gate applied after the primary `AuthFn` gate: the caller
must first pass `AuthFn` (compile-time OR runtime list), then additionally pass
the admin check (compile-time list only). A user in the runtime NVS list but
not in the compile-time list may call `/listusers` but not `/adduser` or
`/removeuser`.

### 2. Storage: new NVS key `"ulist"`

Format: a fixed-size blob of exactly 84 bytes:

```
Offset  Size   Field
──────  ────   ────────────────────────────────────────────────
0       4      int32_t count  — number of valid entries (0–10)
4       80     int64_t ids[10] — runtime user IDs, ids[count..9] undefined
```

Total: 84 bytes. The `count` prefix makes the blob self-describing — no
separate version field is needed for the initial implementation. On first
use the key is absent; treat as an empty list (count = 0, no entries).

NVS wear is negligible: `/adduser` and `/removeuser` each cause one full
84-byte rewrite of the key. These commands are rare (O(1) per week or month);
NVS flash endurance of ~10,000 write cycles is not a concern.

### 3. IPersist: generic blob methods

Add two new virtual methods to `IPersist`:

```cpp
// Generic NVS blob load/save by key name. Returns bytes read (0 if absent).
// key must be a valid NVS key (max 15 chars, no spaces).
virtual size_t loadBlob(const char *key, void *buf, size_t bufSize) = 0;
virtual void   saveBlob(const char *key, const void *buf, size_t bufSize) = 0;
```

`RealPersist` implements both via the ESP32 `Preferences` library:
`prefs_.getBytes(key, buf, bufSize)` and `prefs_.putBytes(key, buf,
bufSize)`.

`FakePersist` (test double) implements both via an in-memory `std::map<std::string,
std::vector<uint8_t>>`. This is compatible with the existing `FakePersist`
design — it adds one `std::map` member and two method implementations.

The existing `loadReplyTargets` / `saveReplyTargets` methods are **kept as-is**;
no existing call sites are changed in this RFC. The user-list persistence uses
the new generic form:

```cpp
// In the load path (startup):
persist_.loadBlob("ulist", &runtimeIdsBlob, sizeof(runtimeIdsBlob));

// In the save path (/adduser, /removeuser):
persist_.saveBlob("ulist", &runtimeIdsBlob, sizeof(runtimeIdsBlob));
```

### 4. In-memory runtime list

Add two file-scope statics to `main.cpp`, parallel to the existing
`allowedIds[]` / `allowedIdCount`:

```cpp
static int64_t runtimeIds[10] = {};
static int     runtimeIdCount = 0;
```

At startup (in `setup()`, after `RealPersist` is initialized and before
`TelegramPoller` is constructed):

```cpp
{
    struct { int32_t count; int64_t ids[10]; } blob{};
    size_t got = realPersist.loadBlob("ulist", &blob, sizeof(blob));
    if (got >= sizeof(int32_t) && blob.count >= 0 && blob.count <= 10) {
        runtimeIdCount = blob.count;
        memcpy(runtimeIds, blob.ids, blob.count * sizeof(int64_t));
    }
    Serial.printf("Runtime user list: %d entr%s\n",
                  runtimeIdCount, runtimeIdCount == 1 ? "y" : "ies");
}
```

The `AuthFn` lambda is extended to check both lists:

```cpp
[](int64_t fromId) -> bool {
    if (fromId == 0) return false;
    // Compile-time list
    for (int i = 0; i < allowedIdCount; i++)
        if (fromId == allowedIds[i]) return true;
    // Runtime NVS list
    for (int i = 0; i < runtimeIdCount; i++)
        if (fromId == runtimeIds[i]) return true;
    return false;
},
```

Both loops always run — no early-termination after a compile-time miss. The
admin (`allowedIds[0]`) is authorized through the compile-time loop.

### 5. Privilege separation: folded into ListMutatorFn

**No `AdminFn` constructor parameter is added.** The admin check belongs at
the call site in `main.cpp`, not as a member of `TelegramPoller` — same
pattern as `AuthFn` and `StatusFn`. `TelegramPoller` gains a single new
constructor parameter:

```cpp
// Called by /adduser and /removeuser. Receives the command ("add" or "remove")
// and the target user ID. Returns true on success; on false, `reason` is
// populated with a human-readable explanation.
using ListMutatorFn = std::function<bool(const String &cmd, int64_t id, String &reason)>;

TelegramPoller(IBotClient &bot,
               SmsSender &smsSender,
               ReplyTargetMap &replyTargets,
               IPersist &persist,
               ClockFn clock,
               AuthFn auth,
               StatusFn status = nullptr,
               ListMutatorFn mutator = nullptr);  // <-- new, optional; nullptr = commands disabled
```

The `ListMutatorFn` lambda in `main.cpp` closes over `allowedIds[]`,
`allowedIdCount`, `runtimeIds[]`, `runtimeIdCount`, and `realPersist`. It
performs both the admin check and the mutation:

```cpp
[](const String &cmd, int64_t id, String &reason) -> bool {
    // Admin check: only compile-time list may mutate.
    bool isAdmin = false;
    for (int i = 0; i < allowedIdCount; i++)
        if (id == allowedIds[i]) { isAdmin = true; break; }
    // NOTE: the check uses the *requester's* id (passed as `id` for /adduser),
    // but actually we need the from-id of the caller, not the target id.
    // See implementation note below.
    ...
}
```

**Implementation note on the admin check**: The `ListMutatorFn` signature
passes the *target* user ID, not the caller's ID. The admin check must use the
caller's ID. Pass `u.fromId` as an additional first argument, or restructure:

```cpp
using ListMutatorFn = std::function<bool(int64_t callerId, const String &cmd,
                                          int64_t targetId, String &reason)>;
```

Production `processUpdate` call site:

```cpp
if (lower.startsWith("/adduser ") || lower == "/adduser") { /* parse arg */ }
if (mutator_) {
    String reason;
    if (!mutator_(u.fromId, "add", newId, reason)) {
        bot_.sendMessageTo(u.chatId, reason);
        return;
    }
    bot_.sendMessageTo(u.chatId, "User added.");
}
```

The lambda in `main.cpp` checks whether `callerId` is in `allowedIds[]` before
touching `runtimeIds[]`. A user in the runtime-only list gets a
"Permission denied" reason string from the lambda. No `admin_` member is needed
in `TelegramPoller`.

### 6. Argument parsing

`processUpdate` currently uses `lower == "/debug"` (exact equality). Commands
with arguments require prefix matching plus token extraction:

```cpp
// Helper (file-static or a short lambda inside processUpdate):
// Returns the trimmed argument after the command prefix, or an empty String
// if lower does not start with prefix.
static String extractArg(const String &lower, const char *prefix) {
    if (!lower.startsWith(prefix)) return String();
    String arg = lower.substring(strlen(prefix));
    arg.trim();
    return arg;
}
```

For `/adduser 123456789`:

```cpp
String arg = extractArg(lower, "/adduser ");
if (arg.length() == 0) {
    bot_.sendMessageTo(u.chatId, "Usage: /adduser <telegram_user_id>");
    return;
}
char *end = nullptr;
int64_t newId = (int64_t)strtoll(arg.c_str(), &end, 10);
if (!end || *end != '\0' || newId <= 0) {
    bot_.sendMessageTo(u.chatId, "Invalid user ID. Must be a positive integer.");
    return;
}
```

`strtoll` with `end` validation rejects non-numeric tokens, partial parses
(`"123abc"`), and IDs that are zero or negative. IDs the modem or Telegram
would reject in practice (> 2^53) pass the `int64_t` range check without
overflow and are stored as-is — Telegram does not impose a narrower range in
its API.

**Edge case — `/adduser` with no space:** `lower` will be exactly `"/adduser"`
with no trailing space, so `lower.startsWith("/adduser ")` returns false and
the usage error fires. This is the correct behavior.

**Edge case — `/adduser` vs `/addusers`:** `startsWith("/adduser ")` (with
trailing space) prevents `/addusers` from matching. The `/removeuser` prefix
check uses `"/removeuser "` (with space) for the same reason.

### 7. `/adduser` and `/removeuser` mutation logic

Both commands are handled entirely inside the `ListMutatorFn` lambda in
`main.cpp` — **no `saveRuntimeList` free function exists in
`telegram_poller.cpp`** (that would require cross-TU access to `runtimeIds[]`
which lives in `main.cpp`). The lambda closes over everything it needs:
`allowedIds[]`, `allowedIdCount`, `runtimeIds[]`, `runtimeIdCount`, and
`realPersist`. The NVS write is performed inside the lambda, not by
`processUpdate`.

**`/adduser <id>`:**

1. Check: `newId` not already in `allowedIds[]` (already an admin — no-op
   with a friendly message: "User is already an admin-level user.").
2. Check: `newId` not already in `runtimeIds[]` (already added — no-op with
   "User is already in the runtime list.").
3. Check: `runtimeIdCount < 10` (list full — error: "Runtime user list is full
   (maximum 10). Remove a user first.").
4. Append `newId` to `runtimeIds[runtimeIdCount++]`.
5. Call `saveRuntimeList(persist_)`.
6. Reply: `"User <id> added. They can now send replies and use /status, /debug,
   /listusers. They cannot manage the user list."`.

**`/removeuser <id>`:**

1. Scan `runtimeIds[]` for `newId`. If not found, reply "User <id> not in
   the runtime list." and return.
2. Attempting to remove an ID from `allowedIds[]` (compile-time list) is
   rejected: "Cannot remove a compile-time admin user. Edit secrets.h and
   reflash to change the compile-time list." (This prevents confusion where
   the admin tries to demote themselves via `/removeuser`.)
3. Shift remaining entries left to fill the gap, decrement `runtimeIdCount`.
4. Call `saveRuntimeList(persist_)`.
5. Reply: `"User <id> removed from the runtime list."`.

### 8. `/listusers`

Available to any authorized user (compile-time OR runtime list). Does not
modify state.

```
/listusers

Compile-time users (2):
  111111111 [admin]
  222222222

Runtime users (1):
  333333333

Total authorized: 3 (max 20)
```

The `[admin]` marker is appended only to `allowedIds[0]` — the primary admin
who receives SMS forwards. All compile-time entries may manage the runtime
list; the admin distinction matters only for `setAdminChatId`.

Implementation: build the reply string in `processUpdate` from the
in-memory arrays; no NVS read is needed (the arrays are always in sync with
NVS after startup).

### 9. Reboot semantics

No reboot is needed. `/adduser` writes to both `runtimeIds[]`/`runtimeIdCount`
(in-memory) and `"ulist"` (NVS) in a single synchronous call. The next
incoming update from the newly added user passes `AuthFn` immediately —
there is no per-update cache that needs flushing.

The NVS write happens before the success reply is sent, so if the write fails
(full NVS partition, hardware fault), the failure propagates as a missing
confirm reply. The in-memory array has already been updated; it will diverge
from NVS until the next successful write. This is an acceptable transient
state: on reboot the NVS value is the authority, and the user will need to
re-issue `/adduser`. Log the NVS failure with `Serial.println`.

### 10. Combined user limit and AuthFn contract

- Compile-time list: 0–10 entries (enforced by `parseAllowedIds` truncation).
- Runtime NVS list: 0–10 entries (enforced by the `/adduser` full-list check).
- Total maximum authorized users: **20**.

`AuthFn` checks both lists independently; there is no merged array. The two
lists are never deduplicated — if the same ID appears in both, it simply
matches twice (no harm; the first match returns `true`). This cannot happen
via normal operation because `/adduser` already rejects IDs present in
`allowedIds[]`, but a manual NVS write could produce it; the behavior is
well-defined.

### 11. Interaction with admin chat ID

`realBot.setAdminChatId(allowedIds[0])` is called in `setup()` and is never
modified at runtime. Adding users to the runtime list does not change the
admin chat ID. SMS forwards continue to go to `allowedIds[0]` regardless of
how many runtime users exist. This is intentional: the admin-chat-ID concept
maps to "who receives unsolicited notifications", which is a property of the
compile-time configuration, not the dynamic user list.

## Notes for handover

### Files to change

1. **`src/ipersist.h`** — add `loadBlob` / `saveBlob` pure virtuals.

2. **`src/real_persist.h`** — implement `loadBlob` / `saveBlob` via
   `Preferences::getBytes` / `putBytes`.

3. **`test/support/fake_persist.h`** — implement `loadBlob` / `saveBlob`
   via `std::map<std::string, std::vector<uint8_t>>`. Existing tests that
   use `FakePersist` continue to compile without change — the new methods
   have no-op or empty-return defaults that do not affect tests that do not
   exercise them.

4. **`src/telegram_poller.h`** — add `ListMutatorFn` type alias and the new
   `mutator` constructor parameter (defaulted to `nullptr`). Add `ListMutatorFn
   mutator_` member. No `AdminFn` or `admin_` is added.

5. **`src/telegram_poller.cpp`**:
   - Add `extractArg` helper as a `static` free function at file scope (not
     exposed in the header).
   - Implement `/adduser`, `/removeuser`, `/listusers` dispatch branches
     before the `reply_to_message_id` path.
   - For `/adduser` and `/removeuser`: call `mutator_(u.fromId, cmd, id,
     reason)` and send the `reason` string back on failure. No admin check
     inside `processUpdate` — that is the lambda's responsibility.

6. **`src/main.cpp`**:
   - Add `runtimeIds[10]` / `runtimeIdCount` file-scope statics.
   - Add blob load call in `setup()` (after `RealPersist` init, before
     `TelegramPoller` construction).
   - Extend `AuthFn` lambda with the runtime-list scan.
   - Write the `ListMutatorFn` lambda capturing `allowedIds`, `allowedIdCount`,
     `runtimeIds`, `runtimeIdCount`, `realPersist` by reference — performs
     both the admin check and the NVS mutation.
   - Pass the `ListMutatorFn` lambda as the 8th constructor argument.

### Mutation callback vs. direct references

`TelegramPoller` is a testable class that should not depend on file-scope
statics in `main.cpp`. Two clean options:

**Option M1: `ListMutatorFn` callback** (recommended)

Add a new using alias to `TelegramPoller`:

```cpp
// Called by /adduser, /removeuser. Receives the new desired state of the
// runtime list. Returns true on success (NVS write succeeded), false on
// failure (full list, duplicate, etc.). On false, `reason` is populated.
using ListMutatorFn = std::function<bool(const String &cmd, int64_t id, String &reason)>;
```

Production wiring in `main.cpp` closes over `runtimeIds[]`,
`runtimeIdCount`, and `realPersist` directly. The lambda handles all
mutation and persistence; `processUpdate` calls it and sends the reply.
`TelegramPoller` stays free of any knowledge of the storage layout.

**Option M2: direct struct injection**

Pass a `RuntimeUserList *` pointer to `TelegramPoller`, where
`RuntimeUserList` is a small POD:

```cpp
struct RuntimeUserList {
    int64_t ids[10];
    int count;
};
```

`TelegramPoller` mutates the struct directly and calls
`persist_.saveBlob("ulist", ...)` using the existing `persist_` member it
already holds. This is slightly simpler at the call site but couples
`TelegramPoller` to the NVS key name.

**Recommendation: Option M1.** It keeps `TelegramPoller` free of storage
layout details, makes the list-mutation path fully testable via a `FakeMutator`
lambda in unit tests, and is consistent with the existing `ClockFn` /
`StatusFn` / `AuthFn` callback pattern already in the class.

### Test approach

All new logic is covered by the existing native test infrastructure
(`pio test -e native`). No hardware is needed.

**New test cases for `TelegramPoller`** (in
`test/test_native/test_telegram_poller.cpp` or a new
`test_user_management.cpp`):

- `/adduser 123456789` from a non-admin authorized user (in runtime list
  only) → permission-denied reply, mutator lambda NOT called.
- `/adduser 123456789` from an admin (in compile-time list) → mutator
  lambda called with `("add", 123456789, reason)`, success reply sent to
  requester's `chatId`.
- `/adduser` with no argument → usage error reply, mutator NOT called.
- `/adduser abc` (non-numeric) → invalid-id error reply, mutator NOT called.
- `/adduser 0` (zero id) → invalid-id error reply (must be positive).
- `/removeuser 123456789` from non-admin → permission-denied reply.
- `/removeuser 123456789` from admin → mutator called with `("remove", ...)`.
- `/listusers` from admin → reply contains compile-time and runtime entries.
- `/listusers` from runtime-only user → reply contains compile-time and
  runtime entries (same output; no privilege distinction for reads).
- `/listusers` from unauthorized user → update dropped at `AuthFn` gate,
  no reply.

**New test cases for `ListMutatorFn` logic** (unit-test the lambda in
isolation, not via `TelegramPoller`):

- Add to empty list → count 1, NVS written.
- Add to full list (count == 10) → returns false, reason "full".
- Add duplicate (already in runtime list) → returns false, reason "already in
  runtime list".
- Add ID that is in `allowedIds[]` → returns false, reason "already admin".
- Remove from list of 1 → count 0, NVS written.
- Remove ID not in list → returns false, reason "not found".
- Remove ID in compile-time `allowedIds[]` (not in runtime list) → returns
  false, reason "cannot remove compile-time admin".
- NVS blob round-trip: write count=2 with two IDs, reload from `FakePersist`,
  verify count and IDs match.

**`FakePersist` additions** (to support the new blob tests):

`FakePersist::loadBlob` returns 0 if the key was never written, which matches
the "absent key → empty list" contract. No special initialization is needed;
tests that rely on an empty runtime list simply don't call `saveBlob("ulist",
...)` before the code under test runs.

### Interaction with `sweepExistingSms` and boot order

The runtime user list must be loaded from NVS **before** `TelegramPoller` is
constructed (so the `AuthFn` lambda's closure over `runtimeIds[]` reflects
the persisted state from the start). `sweepExistingSms` runs at the end of
`setup()` and does not interact with the runtime user list (SMS receive is
not gated on Telegram user identity). Boot order is:

1. `RealPersist` initialized (NVS namespace opened).
2. Runtime user list loaded from `"ulist"` key.
3. `TelegramPoller` constructed with the extended `AuthFn` and `AdminFn`.
4. `telegramPoller.begin()` — loads update_id watermark.
5. `smsHandler.sweepExistingSms()` — drains existing SIM slots.
6. `loop()` begins.

### Interaction with RFC-0018 (`/block` command follow-up)

RFC-0018 §7 sketched a runtime `/block <number>` command using a new NVS key
`"smsblist"`. If that RFC is implemented after this one, it should use the
`loadBlob` / `saveBlob` generic methods added here (rather than adding yet
another pair of specific methods to `IPersist`). The NVS key names are
distinct (`"ulist"` vs. `"smsblist"`), so there is no collision.

### Security note

The runtime user list is stored in NVS flash. The NVS partition on the ESP32
is not encrypted by default — an attacker with physical USB access can read
the flash and enumerate all authorized Telegram user IDs. Telegram user IDs
are not credentials (they appear in API payloads and are not secret by
Telegram's own model), so the practical risk is negligible for the household
use case. If encryption is needed in a future deployment context, ESP32 NVS
encryption can be enabled independently of this feature.

The `/adduser` command surface does not introduce new remote attack surface
beyond what already exists: an attacker would need to be authorized (pass
`AuthFn`) to call `/adduser`, and only compile-time admins pass `AdminFn`.
An unauthorized user receiving a reply-target-expired error cannot infer the
allow list size or contents.

## Review

**verdict: approved-with-changes**

### BLOCKING

- **`AdminFn` as an 8th constructor parameter is the wrong abstraction.**
  `TelegramPoller` already takes 7 parameters; the RFC itself acknowledges
  the constructor is unwieldy (§5 notes that `AdminFn` defaults to `nullptr`,
  and §"Mutation callback vs. direct references" recommends `ListMutatorFn`
  as the right pattern). The admin predicate is trivially expressible as a
  lambda at the `main.cpp` call site — exactly the same pattern already used
  for `AuthFn`. Since `allowedIds[]` / `allowedIdCount` are file-scope statics
  with process lifetime, the `AdminFn` lambda can close over them directly
  without passing them into `TelegramPoller` at all. The RFC should drop
  `AdminFn` as a constructor parameter and instead fold the admin check into
  the `ListMutatorFn` lambda (which is already recommended as Option M1).
  `processUpdate` calls `mutator_(...)` and the lambda decides whether to
  allow or deny — no separate `admin_` member needed in the class at all.
  This keeps the constructor at 8 parameters (`+ListMutatorFn`) but removes
  `AdminFn` entirely, and it keeps `TelegramPoller` free of the
  compile-time-list concept. As written the RFC proposes BOTH `AdminFn` and
  `ListMutatorFn` as separate things; the plan section (§5) and the handover
  notes (§"Mutation callback") are in tension with each other and the
  implementer will have to resolve this ambiguity. Resolve it now: pick M1,
  drop `AdminFn` from the constructor signature, and have the `ListMutatorFn`
  lambda in `main.cpp` do the admin check internally.

- **`IPersist::loadBlob` / `saveBlob` are new pure virtuals — all existing
  implementations must be updated before the build compiles.** The RFC lists
  the three files (`ipersist.h`, `real_persist.h`, `fake_persist.h`) but does
  not flag that *every existing test that constructs a `FakePersist` directly
  will fail to link* until `FakePersist` gets the two new method bodies. The
  RFC should explicitly state that the `IPersist` change and the `FakePersist`
  update must land in the same commit as any code that calls `loadBlob` /
  `saveBlob`, and that the existing `loadReplyTargets` / `saveReplyTargets`
  method pair must be kept (not removed or deprecated mid-RFC) so existing
  call sites in `ReplyTargetMap` continue to compile. The "may be kept or
  deprecated" language in §3 is too vague for a handover doc — tighten it to
  "kept; no existing call sites are changed in this RFC."

- **`saveRuntimeList` is proposed as a free function in `telegram_poller.cpp`
  that closes over `runtimeIds[]` / `runtimeIdCount` from `main.cpp**, but
  those file-scope statics are not visible to `telegram_poller.cpp`.** The
  RFC notes the dependency in §"Files to change" item 5: "the mutation logic
  must live in `processUpdate` rather than in a free function." This directly
  contradicts the `saveRuntimeList` code snippet in §7, which shows it as a
  standalone static helper. The RFC needs to commit to one approach: either
  (a) the mutation lives entirely inside the `ListMutatorFn` lambda in
  `main.cpp` (recommended — consistent with M1), in which case
  `saveRuntimeList` disappears from `telegram_poller.cpp` entirely, or (b)
  `TelegramPoller` receives a mutable reference/pointer to a
  `RuntimeUserList` struct. Pick one and remove the contradictory snippet.

### NON-BLOCKING

- **Missing test case: exceeding the 10-entry runtime limit via
  `ListMutatorFn`.** The test list in §"Test approach" covers "Add to full
  list (count == 10) → returns false, reason 'full'" for the mutator lambda
  in isolation, which is correct. However, there is no corresponding
  `TelegramPoller`-level test that sends `/adduser` when the mutator returns
  false with reason "full", verifying the reply text sent back to the user.
  Add it to the `TelegramPoller` test list for completeness.

- **`/listusers` output says "Total authorized: 3 (max 20)" but the
  compile-time list cap is 10 and the runtime cap is 10, so the combined
  maximum is indeed 20. This is correct, but the line is potentially
  confusing** because a reader might think 20 is a single-list limit.
  Consider "Total authorized: 3 (10 compile-time + 10 runtime max)" or just
  drop the max from the status line — the individual list counts already
  imply the cap.

- **`extractArg` placement should be stated unambiguously.** The RFC says
  "file-static or a short lambda inside `processUpdate`" (§6). For a
  handover doc this is too open-ended. State clearly: `extractArg` is a
  `static` free function at file scope in `telegram_poller.cpp`, not exposed
  in the header. The lambda-inside-processUpdate alternative is messier
  (captures nothing, so a lambda adds noise) and should not be offered as an
  option.

- **The reboot-divergence scenario in §9 is documented accurately** (in-memory
  updated before NVS write, so a failed write leaves them out of sync until
  reboot), but the recommendation to "log the NVS failure with
  `Serial.println`" should also suggest sending an error reply to the
  requester — otherwise the admin sees no response and may retry, accumulating
  duplicate in-memory entries. Add "Send an error reply to the user's chatId
  in addition to the Serial log."

- **`AdminFn` is described in §5 as having a `nullptr` default meaning
  "denied."** The `nullptr`-means-denied sentinel is a footgun: if the
  `AdminFn` is accidentally omitted in a future refactor, `/adduser` silently
  gives a "Permission denied" response rather than a build error. This is
  acceptable given `AdminFn` is being collapsed into `ListMutatorFn` if
  blocking item 1 is addressed — the issue disappears.

- **`int32_t count` in the NVS blob (§2) could theoretically be negative
  if the blob is corrupted or partially written.** The load-path code in §4
  already guards `blob.count >= 0 && blob.count <= 10`, so this is handled.
  No change needed — just confirming the guard is present and sufficient.

- **Interaction with RFC-0014's `allowedIdCount == 0` warning path**: if
  `allowedIdCount` is 0 at startup (empty compile-time list), `AdminFn` /
  `ListMutatorFn` can never authorize anyone for `/adduser`, making the
  runtime list permanently unmanageable via bot. This edge case is not a
  regression (the current `AuthFn` already rejects everything when
  `allowedIdCount == 0`), but it is worth a one-line note in §11 or the
  handover so future operators are not surprised.

## Code Review

**verdict: approved-with-one-fix-required**

The implementation resolves all three blocking issues raised in the RFC pre-review and is
correct for every scenario in the checklist. One new BLOCKING bug was found in
`telegram_poller.cpp`; all other findings are non-blocking.

### BLOCKING

**B-1 — `startsWith("/adduser")` (no space) is too broad; matches `/addusers`**

`telegram_poller.cpp` line 127:

```cpp
if (lower.startsWith("/adduser"))
```

`startsWith("/adduser")` without a trailing space matches any message whose
lowercase text begins with the eight characters `/adduser` — including
`/addusers`, `/adduser_admin`, etc. The RFC's own §6 explicitly requires the
trailing space to prevent this: "startsWith('/adduser ') (note space) prevents
/addusers from matching."

What actually happens at runtime for `/addusers hello`:
1. The outer `if` fires (bug — it should not).
2. `extractArg(lower, "/adduser ")` receives `/addusers hello`; since
   `/addusers hello` does not start with `/adduser ` (the 's' breaks the
   prefix match), `extractArg` returns an empty `String`.
3. The `arg.length() == 0` guard fires and sends "Usage: /adduser
   \<telegram\_user\_id\>" to the caller.

The concrete consequence is limited: the user sees a misleading "Usage" error
instead of the generic "Reply to a forwarded SMS" help text. No state is
mutated, no mutator is called, and no security boundary is crossed because
`AuthFn` has already passed before this code runs. However it is still wrong
behavior, contradicts the RFC spec, and the same bug applies symmetrically to
`/removeuser` (line 160: `startsWith("/removeuser")`).

Fix — two one-character changes in `telegram_poller.cpp`:

```cpp
// line 127
if (lower.startsWith("/adduser ") || lower == "/adduser")

// line 160
if (lower.startsWith("/removeuser ") || lower == "/removeuser")
```

The `|| lower == "/adduser"` clause is needed so a bare `/adduser` with no
trailing space still enters the branch and reaches the usage-error path, rather
than falling through to the help text. `extractArg` already handles this
correctly (returns empty when the prefix with space is not found), so only the
outer guard needs the fix.

Alternatively, and more readably, drop the outer prefix check entirely and rely
solely on `extractArg`:

```cpp
{
    String arg = extractArg(lower, "/adduser ");
    // arg is empty for both "/adduser" (no space) and "/addusers blah"
    // but we must enter this block ONLY for messages that are
    // "/adduser" or "/adduser <something>", not "/addusers".
    // Check that lower is exactly "/adduser" or starts with "/adduser ":
    if (lower != "/adduser" && !lower.startsWith("/adduser ")) goto next_check;
    ...
}
```

The simplest correct form is:

```cpp
if (lower == "/adduser" || lower.startsWith("/adduser "))
```

and the same for `/removeuser`. Use whichever form you prefer; both are
correct.

### NON-BLOCKING

**N-1 — `list` branch of `ListMutatorFn` bypasses the admin check — intentional and correct**

Confirmed: the lambda in `main.cpp` returns `true` with the list content
before reaching the `if (!isAdmin)` gate. This matches the RFC spec ("any
authorized user may call `/listusers`") and the test
`test_UserMgmt_listusers_accessible_by_runtime_user` covers it explicitly. No
change needed.

**N-2 — `mutator_` nullable check is correct for all three commands**

`/listusers` checks `!mutator_` before calling it (returns "not configured").
`/adduser` and `/removeuser` check `!mutator_` after argument parsing but
before calling it (same "not configured" reply, no crash). No null-deref path
exists. No change needed.

**N-3 — NVS blob validation on load is correct**

`main.cpp` line 733:

```cpp
if (got >= sizeof(int32_t) && blob.count >= 0 && blob.count <= 10)
```

A corrupted or partially-written blob with `count` outside `[0, 10]` is
silently discarded and the runtime list starts empty. The `memcpy` at line 736
receives `(size_t)blob.count * sizeof(int64_t)` bytes, bounded above by
`10 * 8 = 80` which fits inside `blob.ids[10]`. No out-of-bounds write is
possible. No change needed.

**N-4 — Admin check correctly uses `callerId`, not `targetId`**

The lambda signature is `(int64_t callerId, const String &cmd, int64_t
targetId, String &reason)`. The `isAdmin` loop at lines 622–626 checks
`callerId == allowedIds[i]`, not `targetId`. This correctly separates
"who is asking" from "who is the subject of the command". No change needed.

**N-5 — `/adduser` with own compile-time ID returns false with "already admin" reason**

`test_UserMgmt_add_duplicate_compiletime_denied` covers this: admin 1000
calls `/adduser 1001` where 1001 is also in `allowedIds[]`. The lambda
returns false with "already a compile-time admin user." Correct. No change needed.

**N-6 — `/removeuser` shift logic is correct and off-by-one-free**

`main.cpp` lines 699–702:

```cpp
for (int i = idx; i < runtimeIdCount - 1; i++)
    runtimeIds[i] = runtimeIds[i + 1];
runtimeIdCount--;
```

For a 3-element list `[100, 200, 300]` removing `200` (idx=1):
- i=1: `runtimeIds[1] = runtimeIds[2]` → `[100, 300, 300]`
- loop ends (1 < 2)
- `runtimeIdCount--` → 2
- Visible array: `[100, 300]`

`test_UserMgmt_remove_middle_entry_shifts_correctly` verifies this exactly.
No change needed.

**N-7 — `String(long long)` and `String(unsigned long long)` in Arduino.h stub**

Added at lines 72–75 of `test/support/Arduino.h`. The existing `String(int)`
and `String(long)` overloads are distinct types on all LP64 and LLP64
platforms (`int` = 32-bit, `long` = 32-bit on Windows/LLP64 or 64-bit on
Linux/LP64, `long long` = 64-bit everywhere). There is no ambiguity or
overload collision; `String((long long)someInt64)` will always resolve to the
new overload rather than truncating through `String(int)`. On Windows
(LLP64) `long` and `int` are both 32 bits, so `String(long long)` is the only
path for full 64-bit ID values. The new overloads are safe and necessary. No
change needed.

**N-8 — `/listusers` true/false branches in `processUpdate` are identical (dead else branch)**

`telegram_poller.cpp` lines 115–123:

```cpp
if (mutator_(u.fromId, String("list"), 0, reason))
    bot_.sendMessageTo(u.chatId, reason);
else
    bot_.sendMessageTo(u.chatId, reason);
```

Both branches call `sendMessageTo(u.chatId, reason)`, so the `if`/`else` is
pure dead code. The `reason` output is what matters (the list text on success,
an error string on failure), and the lambda always returns `true` for "list",
so the `else` branch is unreachable in practice. This is harmless but mildly
confusing. Simplify to:

```cpp
mutator_(u.fromId, String("list"), 0, reason);
bot_.sendMessageTo(u.chatId, reason);
```

Non-blocking — wrong behavior does not result from the current code.

**N-9 — Unknown `cmd` in `ListMutatorFn` falls through to a no-op NVS write**

If `processUpdate` ever calls `mutator_` with a `cmd` other than "list",
"add", or "remove" — which it does not today — the lambda would skip both
mutation branches and perform a no-op `saveBlob` (writing the current list
back unchanged) before returning `true`. This is wasteful but harmless and
cannot happen via the current dispatch table. An `else { reason = "unknown
command"; return false; }` guard would make the lambda more robust, but given
the closed call sites this is low priority. Non-blocking.

**N-10 — `RealPersist::loadBlob` does not pre-check `getBytesLength`**

`real_persist.h` `loadBlob` calls `prefs_.getBytes(key, buf, bufSize)` directly
without the `getBytesLength` pre-check that `loadReplyTargets` does. On ESP32,
`Preferences::getBytes` returns 0 both when the key is absent and when the
stored value is larger than the supplied buffer. For the `"ulist"` use case
the buffer is always exactly 84 bytes and the stored blob (when written by
this code) is also always 84 bytes, so the silent-truncation case cannot arise
in practice. The asymmetry with `loadReplyTargets` is a mild code-consistency
concern but not a bug. Non-blocking.

**N-11 — `TelegramPoller` is constructed before `realPersist.begin()` — ordering is safe**

`main.cpp` line 597 constructs `telegramPoller` (which captures `realPersist`
by reference via the `ListMutatorFn` lambda) before `realPersist.begin()` at
line 719. This is safe because:

1. The lambda does not call `realPersist.saveBlob` until a `/adduser` or
   `/removeuser` command arrives, which cannot happen until `loop()` starts.
2. `loop()` begins only after `setup()` returns, which is after line 719.
3. The `loadBlob("ulist", ...)` call for startup hydration (line 732) is
   already inside the `else` branch of `if (!realPersist.begin())` at line
   723, so NVS is open before the load.

The comment at line 725 documents this correctly. No change needed, but the
ordering is a latent trap for future refactors — worth preserving the comment.

**N-12 — NVS key `"ulist"` does not collide with existing keys**

Existing keys: `"uid"` (update_id watermark), `"rtm"` (reply-target map blob).
New key: `"ulist"`. All three are distinct. No collision.
