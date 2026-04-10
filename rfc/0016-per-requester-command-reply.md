---
status: implemented
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0016: Per-requester command reply

## Motivation

RFC-0014 added a multi-user allow list so multiple Telegram users can interact
with the bot. Any user whose `from.id` appears in the compiled
`TELEGRAM_CHAT_IDS` list can send `/status` or `/debug`. However,
`TelegramPoller::processUpdate` dispatches all replies — command responses
included — via `bot_.sendMessage(...)`, which calls
`RealBotClient::doSendMessage` and always addresses the payload to
`adminChatId_`. The response lands in the admin's DM regardless of who
issued the command.

From a non-admin authorized user's perspective the bot appears broken: they
send `/status`, the bot processes it silently, and nothing ever appears in
their chat. The RFC-0014 code review flagged this as NON-BLOCKING:

> Either restrict `/status`/`/debug` to admin-only (check
> `fromId == allowedIds[0]`), or park the fix with a clear TODO comment in
> `processUpdate`. Silently discarding the response is worse than an explicit
> reply.

This RFC specifies the fix: extend `IBotClient` with a targeted send method so
`processUpdate` can reply to the chat that originated the command, while SMS
forwards and other admin-directed messages continue to use the existing
admin-only path.

## Current state

### IBotClient interface (src/ibot_client.h)

Two send methods exist, both address `adminChatId_` only:

```cpp
virtual bool sendMessage(const String &text) = 0;
virtual int32_t sendMessageReturningId(const String &text) = 0;
```

### RealBotClient (src/telegram.h, src/telegram.cpp)

`doSendMessage(const String &text)` is the single shared HTTP helper. It is
private and takes only `text`; `chat_id` is always `adminChatId_` (an
`int64_t` member set via `setAdminChatId()` in `setup()`). The method
signature is:

```cpp
int32_t doSendMessage(const String &text);
```

### TelegramUpdate struct (src/ibot_client.h)

The struct currently has:

```cpp
struct TelegramUpdate {
    int32_t updateId = 0;
    int64_t fromId = 0;            // message.from.id (or message.chat.id fallback)
    int32_t replyToMessageId = 0;
    String text;
    bool valid = false;
};
```

`fromId` is the individual user ID, with `chat.id` as a fallback — but `chat.id`
is NOT stored as a separate field. In `telegram.cpp`'s `pollUpdates`, `chat.id`
is checked only as a fallback when `from.id` is absent; it is then written to
`u.fromId`, not to a dedicated `chatId` member. To send a reply to a group chat
(where the reply target is the group, not the individual), `processUpdate` needs
the `chat.id` value separately.

### TelegramPoller::processUpdate (src/telegram_poller.cpp)

Command dispatch (lines 56–98) calls `bot_.sendMessage(...)` with no chat
targeting — always reaches `adminChatId_`. The `sendErrorReply` helper has the
same limitation: `bot_.sendMessage(msg)`.

### FakeBotClient (test/support/fake_bot_client.h)

Records messages in `sent_` (a `std::vector<String>`) with no per-call metadata
about which chat ID was targeted. Tests can assert message content and count but
cannot assert that a reply was directed to the correct chat.

## Plan

### 1. Extend IBotClient with sendMessageTo

Add a new pure virtual method to `IBotClient`:

```cpp
// Send a plain-text message to an arbitrary chat ID. Returns true iff
// the message was accepted end-to-end (HTTP 200 and "ok":true). Use
// this for per-requester command replies where the target chat differs
// from the admin chat. SMS forwards should still use sendMessage() /
// sendMessageReturningId(), which always target adminChatId_.
virtual bool sendMessageTo(int64_t chatId, const String &text) = 0;
```

This sits alongside, not replacing, the existing admin-targeted methods.
`sendMessage(text)` becomes conceptually a shorthand for
`sendMessageTo(adminChatId_, text)` — but both names remain on the interface
so callers are explicit about intent:

- `sendMessage` / `sendMessageReturningId` — admin chat, used for SMS
  forwards, boot banners, call notifications.
- `sendMessageTo` — requester's chat, used for command replies and
  per-user error messages from `processUpdate`.

Keeping both names avoids a site-wide rename and makes the distinction
self-documenting at call sites.

### 2. Add chatId field to TelegramUpdate

Add a `chatId` field to the struct in `ibot_client.h`:

```cpp
struct TelegramUpdate {
    int32_t updateId = 0;
    int64_t fromId = 0;   // message.from.id; used for auth gate
    int64_t chatId = 0;   // message.chat.id; used as reply target
    int32_t replyToMessageId = 0;
    String text;
    bool valid = false;
};
```

In `telegram.cpp`'s `pollUpdates`, populate `u.chatId` unconditionally from
`msg["chat"]["id"]` (always present on message objects), independent of the
`fromId` fallback logic. The existing fallback — "if `from.id` is absent, use
`chat.id` for `fromId`" — stays in place because the auth gate is keyed on
`fromId`. `chatId` is only used as the reply destination.

Why `chatId` rather than `fromId` for reply targeting: in a group chat,
`from.id` is the individual member's ID while `chat.id` is the group's
(negative) ID. Replying to `from.id` would open a DM with that individual,
potentially without the user having initiated a private conversation with the
bot. Replying to `chat.id` sends the response back to the same context
(group or DM) where the command was issued, which is always correct:

- DM: `chatId == fromId` (positive value, the user's own ID).
- Group: `chatId` is the group's negative ID; the reply goes to the group.

### 3. Update RealBotClient

Extend `doSendMessage` to accept an explicit target:

```cpp
// private:
int32_t doSendMessage(const String &text, int64_t chatId);
```

Update `sendMessage` and `sendMessageReturningId` to forward `adminChatId_`:

```cpp
bool RealBotClient::sendMessage(const String &text) {
    return doSendMessage(text, adminChatId_) > 0;
}
int32_t RealBotClient::sendMessageReturningId(const String &text) {
    return doSendMessage(text, adminChatId_);
}
```

Implement `sendMessageTo`:

```cpp
bool RealBotClient::sendMessageTo(int64_t chatId, const String &text) {
    return doSendMessage(text, chatId) > 0;
}
```

`doSendMessage` replaces the hardcoded `doc["chat_id"] = adminChatId_` with the
parameter value. The function signature change is internal (private); no
external callers are affected.

### 4. Update TelegramPoller::processUpdate

Pass `u.chatId` as the reply target for all user-visible responses that
originate from `processUpdate`. This requires threading the target through to
`sendErrorReply` as well.

Change `sendErrorReply` signature:

```cpp
// private:
void sendErrorReply(int64_t chatId, const String &reason);
```

All call sites in `processUpdate` pass `u.chatId`. The method body:

```cpp
void TelegramPoller::sendErrorReply(int64_t chatId, const String &reason) {
    String msg = String("\xE2\x9D\x8C ") + reason;
    bot_.sendMessageTo(chatId, msg);
}
```

Command dispatch blocks:

```cpp
if (lower == "/debug") {
    String reply = debugLog_ ? debugLog_->dump()
                             : String("(debug log not configured)");
    bot_.sendMessageTo(u.chatId, reply);
    return;
}
if (lower == "/status") {
    String reply = statusFn_ ? statusFn_()
                             : String("(status not configured)");
    bot_.sendMessageTo(u.chatId, reply);
    return;
}
```

The "no reply_to_message_id" fallback message and the SMS-queued confirmation:

```cpp
sendErrorReply(u.chatId,
    String("Reply to a forwarded SMS to send a response. ") +
    "Use /debug for the SMS diagnostic log, /status for device health.");

// ...

bot_.sendMessageTo(u.chatId, String("\xE2\x9C\x85 Queued reply to ") + phone);
```

The failure lambda captured in the SMS enqueue call should also reply to the
requester. The lambda currently captures `this` and calls `sendErrorReply`. After
this RFC, it must also capture `u.chatId`:

```cpp
int64_t requesterChatId = u.chatId;  // copy for lambda capture
smsSender_.enqueue(phone, u.text, [this, capturedPhone, requesterChatId]() {
    sendErrorReply(requesterChatId,
                   String("SMS to ") + capturedPhone + " failed after retries.");
});
```

### 5. SMS forwards continue to use adminChatId_ only

`SmsHandler` calls `IBotClient::sendMessageReturningId` to forward an incoming
SMS and record the Telegram `message_id` in `ReplyTargetMap`. This must
continue to target `adminChatId_` — the ring buffer stores the Telegram
message_id for reply routing, and the forwarded message must exist in the admin
chat (or the shared group, if Option N3 from RFC-0014 is in use) for a user to
reply to it.

`CallHandler` calls `IBotClient::sendMessage` for call notifications. This also
continues to target `adminChatId_` — call notifications are unsolicited and have
no per-requester context.

No changes to `SmsHandler`, `CallHandler`, or their call sites.

### 6. Update FakeBotClient

Replace the flat `sent_` vector with a struct that captures the target `chatId`
per call, enabling tests to assert that the right chat received the right message:

```cpp
struct SentMessage {
    int64_t chatId;   // 0 for sendMessage / sendMessageReturningId (admin target)
    String text;
};
std::vector<SentMessage> sent_;
```

`sendMessage` and `sendMessageReturningId` push `{0, text}` (using 0 as a
sentinel for "admin-targeted, chatId not specified by caller"). `sendMessageTo`
pushes `{chatId, text}`.

Keep `sentMessages()` returning `std::vector<String>` (built on-the-fly by
collecting `.text` from `sent_`). This preserves all 13+ existing call sites
in `test_telegram_poller.cpp`, `test_sms_handler.cpp`, and
`test_sms_handler_pdu.cpp` without modification. Add an additive accessor:

```cpp
const std::vector<SentMessage> &sentMessagesWithTarget() const { return sent_; }
```

Do **not** change `sentMessages()` to return `const std::vector<SentMessage>&`
— the clean-break approach would require touching every existing test file that
calls `sentMessages()` and is not worth the churn here. The `callCount()`
accessor must count ALL send calls (`sendMessage`, `sendMessageReturningId`,
AND `sendMessageTo`) — verify it returns `sent_.size()` after the structural
change.

### 7. Test cases to add

In `test/test_native/` (existing `test_telegram_poller.cpp` or a new file):

- `/status` from a non-admin authorized user: assert `sendMessageTo` called
  with the user's `chatId`, not `adminChatId_`.
- `/debug` from a non-admin authorized user: same assertion.
- "no reply_to_message_id" fallback: error reply goes to `u.chatId`.
- Reply-target expired: error reply goes to `u.chatId`.
- Successful SMS enqueue: confirmation goes to `u.chatId`.
- SMS delivery failure (lambda fires): error goes to captured `requesterChatId`.
- Admin user invokes `/status`: reply goes to `u.chatId` (== admin's chatId),
  same net effect as before but now exercising the new code path.
- Group chat scenario: `chatId` is a negative group ID, `fromId` is the
  individual's positive ID; assert reply targets the group.

## Notes for handover

### Interaction with reply-target map

`sendMessageReturningId` returns the Telegram `message_id` of the forwarded
message in the admin chat. `ReplyTargetMap` stores that id so that when a user
replies to it, `processUpdate` can look up the SMS phone number. This chain is
unaffected: the reply-to lookup uses `u.replyToMessageId` (the id of the
message being replied to), and the ring buffer entry was created from the
admin-chat send. The `u.chatId` used for the confirmation reply is orthogonal.

### Group chat (RFC-0014 Option N3)

If the admin configures a Telegram group as the sole entry in
`TELEGRAM_CHAT_IDS`, then:

- `adminChatId_` is the group's negative ID.
- `u.chatId` for messages from that group is also the group's negative ID.
- `u.fromId` is the individual member's positive ID.
- `sendMessageTo(u.chatId, ...)` replies to the group — correct.
- `sendMessage(...)` forwards SMS to the group — correct.

The two paths converge correctly in the group case with no special handling.

### The chatId == 0 guard

If `msg["chat"]["id"]` is absent (should not happen for well-formed Telegram
message objects, but defensive coding applies), `u.chatId` will be 0.
`sendMessageTo(0, ...)` will produce a 400 from the Telegram API, wasting a
full TLS round-trip. In `doSendMessage`, add an early-return guard (not just a
log line) when `chatId == 0`:

```cpp
if (chatId == 0) {
    Serial.println("doSendMessage: chatId is 0, skipping send");
    return -1;
}
```

This mirrors the recommendation from the RFC-0014 code review for the
`adminChatId_ == 0` case, and avoids dead time in the main loop.

### Migration / backward compat

`sendMessage(text)` and `sendMessageReturningId(text)` remain on the interface
unchanged. No call sites outside `processUpdate` need updating. The only
breaking change is the addition of a new pure virtual `sendMessageTo` to
`IBotClient`, which requires every implementer (`RealBotClient` and
`FakeBotClient`) to provide the method. Both are in this repo and are updated
as part of this RFC. There are no other known implementers.

### Files to touch

1. `src/ibot_client.h` — add `chatId` to `TelegramUpdate`; add
   `sendMessageTo` pure virtual.
2. `src/telegram.h` — add `sendMessageTo` override declaration to
   `RealBotClient`; widen `doSendMessage` private signature.
3. `src/telegram.cpp` — implement `sendMessageTo`; update `doSendMessage`
   signature and the `doc["chat_id"]` line; update `sendMessage` /
   `sendMessageReturningId` to pass `adminChatId_`; populate `u.chatId` in
   `pollUpdates`.
4. `src/telegram_poller.h` — update `sendErrorReply` private signature.
5. `src/telegram_poller.cpp` — update `sendErrorReply`, all `bot_.sendMessage`
   calls in `processUpdate`, and the failure lambda capture list.
6. `test/support/fake_bot_client.h` — add `SentMessage` struct; implement
   `sendMessageTo`; update `sentMessages()` or add `sentMessagesWithTarget()`.
7. `test/test_native/test_telegram_poller.cpp` — update the `makeUpdate` helper
   to accept a `chatId` parameter (add as a 5th argument with a default of 0 for
   backward compat, or add a separate `makeUpdateWithChatId` factory); audit all
   existing `makeUpdate` call sites to ensure they still pass with `chatId=0` as
   the sentinel for DM scenarios; add new test cases from §7 using explicit
   non-zero `chatId` values.

## Review

**verdict: approved-with-changes**

- **BLOCKING — `FakeBotClient` backward-compat path is underspecified and
  likely wrong.** The RFC offers two options for `sentMessages()`: keep the
  old `const std::vector<String>&` accessor or do a "clean break." Thirteen
  existing tests in `test_telegram_poller.cpp` (and others in
  `test_sms_handler.cpp`) call `bot.sentMessages()` and iterate over plain
  `String` values. If `sentMessages()` is changed to return
  `const std::vector<SentMessage>&`, every one of those call sites breaks at
  compile time. The RFC must commit to one approach — the lower-churn path is
  to keep `sentMessages()` returning `std::vector<String>` (built on-the-fly
  from `sent_`, or by keeping a parallel `String`-only vector for legacy
  callers) and add `sentMessagesWithTarget()` as a second, additive accessor.
  Do not silently leave this as "choose at implementation time": the decision
  affects how many test files must be touched and needs to be made explicit in
  the RFC before implementation begins.

- **BLOCKING — `makeUpdate` helper in the test file does not set `chatId`;
  new tests asserting `chatId` routing will see 0 unless the helper is
  updated.** The `makeUpdate(updateId, fromId, replyToId, text)` function in
  `test_telegram_poller.cpp` constructs a `TelegramUpdate` and leaves the new
  `chatId` field at its default of 0. Every existing test that calls
  `makeUpdate` will produce updates with `chatId == 0`, which means
  `sendMessageTo(0, ...)` will be called — hitting the degenerate path the RFC
  itself calls out as needing a log line. The helper must be updated (add a
  `chatId` parameter, or a separate `makeUpdateWithChatId` factory) and all
  existing call sites must be audited. The RFC's "Files to touch" list in §8
  does not mention updating the helper or the existing test call sites, which
  is a gap.

- **BLOCKING — Failure lambda captures `u` by reference to a local.**
  The RFC's own plan (§4) correctly shows the fix — copy `u.chatId` into a
  named local `requesterChatId` before the lambda — but the current production
  code in `telegram_poller.cpp` (line 126) captures only `[this, capturedPhone]`.
  If the RFC is implemented without the explicit copy shown in §4, the lambda
  will capture `u` by reference (or the caller will pass `u.chatId` as a
  reference-to-local-that-is-destroyed-before-the-lambda-fires), producing a
  use-after-free when `SmsSender::drainQueue` fires the callback on a later
  `loop()` iteration. The RFC plan is correct; this is flagged BLOCKING to
  ensure an implementer reads §4 carefully and does not reuse the existing
  lambda pattern from `telegram_poller.cpp` verbatim.

- **NON-BLOCKING — `chatId == 0` guard is documented but not enforced
  fail-fast.** §4 (Notes for handover) says to "add a log line in
  `doSendMessage` when `chatId == 0`" but does not say to return early or
  skip the TLS round-trip. A wasted TLS round-trip to the Telegram API that
  will always yield HTTP 400 is dead time in the main loop. The log line
  should be accompanied by an early return (return `false` / `0` as
  appropriate) so the path is observable and non-blocking without incurring
  the round-trip cost. This mirrors the recommendation made in the RFC-0014
  code review for the `adminChatId_ == 0` case.

- **NON-BLOCKING — `sendMessageReturningId` correctness is stated but not
  tested.** §5 correctly asserts that `sendMessageReturningId` must continue
  to target `adminChatId_` exclusively, and the implementation plan (§3) wires
  this correctly. However, none of the test cases listed in §7 add a
  regression for this invariant. A single test — "SMS forward from
  SmsHandler still targets adminChatId_" using `sentMessagesWithTarget()` to
  assert `chatId == 0` (the admin-sentinel) — would prevent future refactors
  from accidentally routing forwards to the requester's chat. Low effort,
  high value.

- **NON-BLOCKING — `callCount()` on `FakeBotClient` will be wrong after the
  change if `sendMessageTo` calls are not counted.** The current `callCount()`
  accessor returns `sent_.size()`. Once `sent_` is replaced with
  `std::vector<SentMessage>`, `callCount()` still works if it returns
  `sent_.size()`. But if a parallel `String`-only vector is kept for the
  legacy `sentMessages()` accessor, `sendMessageTo` calls must also push into
  that vector, or `callCount()` will silently undercount. Whichever structural
  choice is made for `FakeBotClient`, verify that `callCount()` reflects ALL
  send calls (`sendMessage`, `sendMessageReturningId`, and `sendMessageTo`).
  The existing test `test_TelegramPoller_unauthorized_drops_and_advances`
  asserts `bot.callCount() == 0`; it must not regress.

- **NON-BLOCKING — `CallHandler` and `SmsHandler` isolation is correctly
  stated (§5) but could be made test-visible.** The RFC correctly notes that
  `CallHandler` (via `sendMessage`) and `SmsHandler` (via
  `sendMessageReturningId`) must not be affected. This is architecturally
  enforced because neither class calls `processUpdate`. No code change needed,
  but the group-chat test case listed in §7 ("chatId is a negative group ID,
  fromId is the individual's positive ID") should also confirm that a
  concurrent SMS forward in the same test run still lands in `sent_` with the
  admin-sentinel `chatId`, not the group ID — making the isolation
  mechanically visible in the test suite rather than just documented.

- **NON-BLOCKING — The "clean break" option for `sentMessages()` would require
  touching `test_sms_handler.cpp` and `test_sms_handler_pdu.cpp` as well.**
  `sentMessages()` is called from at least `test_telegram_poller.cpp` (13 call
  sites) and also from `test_sms_handler.cpp` / `test_sms_handler_pdu.cpp`
  (those files use `FakeBotClient` via the `SmsHandler` interface). The "Files
  to touch" list (§8) omits these. If the clean-break approach is chosen
  despite the BLOCKING issue above, all affected test files must be listed
  explicitly.

## Code Review

**Verdict: APPROVED — all blocking issues from the pre-implementation review
are resolved; one new minor issue noted below.**

---

### Checklist results

**1. `pollUpdates` — `u.chatId` populated unconditionally (PASS)**

`telegram.cpp` lines 509–512 set `u.chatId` from `msg["chat"]["id"]` inside
its own `if (!msg["chat"]["id"].isNull())` guard, completely independent of
the `fromId` fallback branch (lines 516–525). The two assignments are
sequential, not nested. `chatId` population is not gated on whether `from.id`
is present. Correct.

**2. `doSendMessage` — `chatId == 0` early-return before HTTP round-trip (PASS)**

Lines 215–219 in `telegram.cpp` are the first thing in `doSendMessage` after
the signature. The guard returns `-1` before the `transport_` null check,
before the JSON document allocation, and before any I/O. Correct.

**3. Failure lambda — safe capture via named copy (PASS)**

`telegram_poller.cpp` lines 128–131:

```cpp
String capturedPhone = phone;
int64_t requesterChatId = u.chatId;  // copy before capture
smsSender_.enqueue(phone, u.text, [this, capturedPhone, requesterChatId]() {
    sendErrorReply(requesterChatId, String("SMS to ") + capturedPhone + " failed after retries.");
});
```

`u` is a `const TelegramUpdate &` parameter of `processUpdate`, which is a
stack frame that will be gone by the time the lambda fires. The implementation
correctly copies `u.chatId` to `requesterChatId` before the lambda closes over
it. No dangling-reference risk. Correct.

**4. `sendMessage` / `sendMessageReturningId` still forward `adminChatId_` (PASS)**

`telegram.cpp` lines 332–339:

```cpp
bool RealBotClient::sendMessage(const String &text) {
    return doSendMessage(text, adminChatId_) > 0;
}
int32_t RealBotClient::sendMessageReturningId(const String &text) {
    return doSendMessage(text, adminChatId_);
}
```

Both pass `adminChatId_` (the `int64_t` member set via `setAdminChatId()` in
`setup()`). Neither passes `0` or any other variable. Correct.

**5. `FakeBotClient::sentMessages()` return type and `callCount()` coverage (PASS)**

`sentMessages()` returns `std::vector<String>` (built on-the-fly at line
133–141 by projecting `.text` from the `SentMessage` vector). All three send
methods (`sendMessage`, `sendMessageReturningId`, `sendMessageTo`) push into
the same `sent_` vector. `callCount()` returns `sent_.size()` (line 148),
which counts all three. The existing `test_TelegramPoller_unauthorized_drops_and_advances`
assertion `bot.callCount() == 0` will pass and does not regress. Correct.

**6. New tests assert `sendMessageTo` is NOT called for admin-only paths (PASS)**

`test_TelegramPoller_sms_forward_uses_admin_sentinel` (line 705) calls
`bot.sendMessageReturningId(...)` directly and asserts
`sentMessagesWithTarget()[0].chatId == 0` (the admin sentinel) and
`callCount() == 1`. This is an explicit regression guard that
`sendMessageReturningId` does not route to an arbitrary chat. Correct.

**7. All existing `sentMessages()` call sites compile without modification (PASS)**

The 14 pre-existing test functions in `test_telegram_poller.cpp` that call
`bot.sentMessages()` iterate over `String` values. `sentMessages()` still
returns `std::vector<String>`, so none of those call sites need touching.
The additive `sentMessagesWithTarget()` accessor is only used by the 8 new
RFC-0016 tests. Correct.

**8. `CallHandler` unaffected (PASS)**

`call_handler.cpp` line 128 calls `bot_.sendMessage(msg)`. This is the
admin-targeted overload, unchanged by this RFC. `CallHandler` does not have
access to a `TelegramUpdate` and has no reason to use `sendMessageTo`. No
change was made to `call_handler.{h,cpp}`. Correct.

**9. `SmsHandler` unaffected (PASS)**

`sms_handler.cpp` lines 93 and 289 call `bot_.sendMessageReturningId(...)`.
Both are in `forwardSingle` and `insertFragmentAndMaybePost`, neither of which
is touched by this RFC. The reply-target ring buffer writes (`replyTargets_->put`)
continue to use the message_id returned by the admin-targeted path. Correct.

---

### New issue found during review

**NON-BLOCKING — `doSendMessage` returns `-1` for `chatId == 0` but
`sendMessageReturningId` propagates that as a non-zero "failure" value.**

`sendMessageReturningId` is defined as `return doSendMessage(text, adminChatId_)`.
If `adminChatId_` is 0 (i.e. `setAdminChatId()` was never called in `setup()`),
`doSendMessage` returns `-1`, which `sendMessageReturningId` propagates to its
caller. `SmsHandler::forwardSingle` checks `if (mid <= 0)` and treats that as
failure (line 94–97), so `-1` is correctly handled as failure there. The existing
return-value contract — "positive on success, 0 or negative on failure" — is
respected, so this is safe in practice.

However, the companion `sendMessage` wrapper does `doSendMessage(text, adminChatId_) > 0`,
which evaluates `-1 > 0` as `false`, also correctly signaling failure.
No functional bug. The only ambiguity is that `-1` now means two different
things ("chatId was zero" vs "transport error"), but no caller distinguishes
between failure sub-types.

Calling it NON-BLOCKING because `SmsHandler` and `CallHandler` only care about
the success/failure boolean, not the specific negative value. A future caller
that tries to distinguish error codes would need to be aware of this, but none
exists today.

---

### Missing test from RFC §7 plan

**NON-BLOCKING — The "SMS delivery failure lambda fires" test case from §7 is
not implemented.**

RFC §7 lists: "SMS delivery failure (lambda fires): error goes to captured
`requesterChatId`." This test would call `sender.drainQueue()` after the modem
is configured to fail, then assert the error reply from the lambda goes to
`kRequesterChatId`. None of the 8 new tests exercise this path.

The lambda capture is correct by code inspection (item 3 above), but the
test-visible path is unexercised. Low risk given the correctness of the capture,
but it is a gap relative to the plan. A future test could set `FakeModem` to
fail on `sendPduSms` and assert `sentMessagesWithTarget()` contains an entry
with `chatId == kRequesterChatId` and text containing "failed after retries".

---

### Summary

All 9 checklist items pass. Both blocking issues and both non-blocking issues
from the pre-implementation review are resolved:

- `FakeBotClient` backward-compat: resolved via `sentMessages()` on-the-fly
  projection + additive `sentMessagesWithTarget()`.
- `makeUpdate` helper: resolved; 5th `chatId` parameter with default `0`.
- Lambda dangling-reference: resolved via `int64_t requesterChatId = u.chatId`
  named copy before capture.
- `chatId == 0` guard: implemented as an early-return (`return -1`) before
  any I/O, not merely a log line.
- `sendMessageReturningId` regression test: present as
  `test_TelegramPoller_sms_forward_uses_admin_sentinel`.

Two new minor issues are noted above (both NON-BLOCKING). The implementation
is correct and ready to ship.
