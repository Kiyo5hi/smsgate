#pragma once

#include <Arduino.h>
#include <functional>
#include <stdint.h>

#include "ibot_client.h"
#include "ipersist.h"
#include "reply_target_map.h"
#include "sms_sender.h"

class SmsDebugLog;

// Stateful Telegram -> SMS poller (RFC-0003 §1, §2, §4, §5).
//
// Wires together:
//   - IBotClient::pollUpdates(...)        — fetch new updates
//   - ReplyTargetMap                       — map reply_to_message_id to phone
//   - ISmsSender                           — send the actual SMS
//   - IPersist                             — persist last update_id watermark
//
// State machine: dead simple. `tick()` is called from main.cpp's loop().
// At most once every kPollIntervalMs we issue a getUpdates request and
// process the response. Each update goes through:
//
//   1. Authorization gate: ignore if `from.id` (or `chat.id` fallback)
//      isn't in the allow list. Update_id still advances.
//   2. Reply-to lookup: read `reply_to_message_id` from the update,
//      look up the phone number in the ring buffer. If the slot is
//      empty or the stored msg_id doesn't match, post an error
//      reply via the bot (so the user knows their reply went into
//      the void) and advance.
//   3. Send via SmsSender. On failure, post the SmsSender error
//      message back to the user (so they know what happened) and
//      advance.
//   4. On success, post a tiny confirmation reply.
//   5. update_id advances regardless of outcome (fail-closed).
//
// Implementation note (RFC-0003 first cut): we use SHORT polling
// (kPollTimeoutSec = 0) rather than long polling, with a 3-second
// poll interval. Long polling would block the IModem URC drain
// while the request is in flight (up to 25s), which would silently
// drop +CMTI / RING / +CLIP URCs. The right fix is a non-blocking
// HTTP state machine, but that's a meaningful chunk of work; the
// short-poll approach gets us the user-visible feature now and the
// data-cap cost is negligible (~30 KB/day at 3s poll interval).
// Tracked as a follow-up RFC.
//
// Tests: drive a virtual clock + a FakeBotClient + a FakePersist
// and assert which SmsSender calls / which bot replies happen.
class TelegramPoller
{
public:
    // How long between successive getUpdates requests, in
    // milliseconds. The first poll happens immediately on the
    // first tick after construction.
    static constexpr uint32_t kPollIntervalMs = 3000;

    // Telegram-side timeout in seconds, passed as `?timeout=<n>`.
    // 0 = short polling (returns immediately if no updates).
    static constexpr int32_t kPollTimeoutSec = 0;

    // Auth callback: returns true iff the given Telegram from-id is
    // allowed to send SMS. Production passes a lambda comparing
    // against the parsed TELEGRAM_CHAT_ID; tests pass any predicate.
    // Nested in the class to avoid colliding with SmsHandler::ClockFn
    // and CallHandler::ClockFn at namespace scope.
    using ClockFn = std::function<uint32_t()>;
    using AuthFn = std::function<bool(int64_t fromId)>;

    // Status callback: returns a formatted device-health string shown
    // by the /status bot command. Production passes a lambda that
    // closes over cached modem CSQ/registration values and counters
    // from SmsHandler, TelegramPoller, ReplyTargetMap, etc. Tests
    // pass nullptr (fallback: "(status not configured)") or a simple
    // lambda returning a canned string.
    using StatusFn = std::function<String()>;

    // Called by /adduser, /removeuser, and /listusers.
    // Signature: (callerId, cmd, targetId, reason&) -> bool
    //   - cmd is "add", "remove", or "list"
    //   - targetId is the user ID to add/remove (0 for "list")
    //   - On success: returns true; for "list", reason contains the reply text
    //   - On failure: returns false; reason contains the user-facing error message
    // The lambda is responsible for the admin check (callerId in compile-time list?).
    // nullptr means user management commands are disabled.
    using ListMutatorFn = std::function<bool(int64_t callerId, const String &cmd,
                                             int64_t targetId, String &reason)>;

    // Called by /block, /unblock, and /blocklist (RFC-0021).
    // Signature: (callerId, cmd, number, reason&) -> bool
    //   - cmd is "block", "unblock", or "list"
    //   - number is the phone number string (empty for "list")
    //   - On success: returns true; for "list", reason contains the reply text
    //   - On failure: returns false; reason contains the user-facing error message
    // The lambda is responsible for the admin check (callerId in compile-time list?).
    // nullptr means SMS block list bot commands are disabled.
    using SmsBlockMutatorFn = std::function<bool(int64_t callerId, const String &cmd,
                                                  const String &number, String &reason)>;

    // NOTE: TelegramPoller takes SmsSender& (the concrete type, not ISmsSender&)
    // so it can call enqueue() and drainQueue() which are on SmsSender only —
    // not on the ISmsSender interface (RFC-0012 Option A). This permanently
    // forecloses using an ISmsSender-only test double for TelegramPoller; any
    // test double must be a concrete SmsSender constructed with a FakeModem.
    // TelegramPoller holds a reference only — it does NOT own the SmsSender,
    // so lambdas capturing `this` stored in queue entries are safe as long as
    // both objects have process lifetime (which they do in main.cpp).
    TelegramPoller(IBotClient &bot,
                   SmsSender &smsSender,
                   ReplyTargetMap &replyTargets,
                   IPersist &persist,
                   ClockFn clock,
                   AuthFn auth,
                   StatusFn status = nullptr,
                   ListMutatorFn mutator = nullptr,
                   SmsBlockMutatorFn smsBlockMutator = nullptr);

    // Load the watermark from persist. Call once at startup, after
    // ReplyTargetMap::load(). Idempotent.
    void begin();

    // Drive the poller. Cheap if it's not yet time to poll. Issues
    // one getUpdates per kPollIntervalMs.
    void tick();

    // Optional: attach a diagnostic log. When set, non-reply messages
    // containing "debug" or "/debug" dump the log instead of the
    // generic help message.
    void setDebugLog(SmsDebugLog *log) { debugLog_ = log; }

    // Test introspection.
    int32_t lastUpdateId() const { return lastUpdateId_; }
    int pollAttempts() const { return pollAttempts_; }

private:
    // Process one parsed update. Returns the (possibly updated)
    // watermark to advance to. On any error path, still advances
    // past `u.updateId`.
    void processUpdate(const TelegramUpdate &u);

    // Send an error reply via the bot to the given chat ID. Best-effort;
    // failures are logged but not retried (the user-visible failure has
    // already been logged).
    void sendErrorReply(int64_t chatId, const String &reason);

    IBotClient &bot_;
    SmsSender &smsSender_;
    ReplyTargetMap &replyTargets_;
    IPersist &persist_;
    ClockFn clock_;
    AuthFn auth_;
    StatusFn statusFn_;
    ListMutatorFn mutator_;
    SmsBlockMutatorFn smsBlockMutator_;

    SmsDebugLog *debugLog_ = nullptr;
    int32_t lastUpdateId_ = 0;
    uint32_t lastPollMs_ = 0;
    bool firstPollDone_ = false;
    int pollAttempts_ = 0;
};
