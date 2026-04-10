#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <vector>

// One inbound Telegram update, decoded from a `getUpdates` response.
// We only extract the fields RFC-0003's TG -> SMS pipeline needs,
// and only fields the JSON parser succeeded on are populated. The
// `valid` flag tells the poller whether to forward this update to
// SmsSender or just advance past it (fail-closed parsing — see
// rfc/0003-bidirectional-telegram-to-sms.md §5).
//
// Lives on IBotClient because both the production RealBotClient
// (which parses real JSON via ArduinoJson) and the test FakeBotClient
// (which constructs them by hand) need to produce them.
struct TelegramUpdate
{
    int32_t updateId = 0;          // Telegram's monotonic update id
    int64_t fromId = 0;            // message.from.id (or message.chat.id fallback); used for auth gate
    int64_t chatId = 0;            // message.chat.id; used as reply target (group or DM)
    int32_t replyToMessageId = 0;  // message.reply_to_message.message_id; 0 if absent
    String text;                   // message.text; empty if absent
    bool valid = false;            // true iff the update parsed cleanly enough to act on
};

// Narrow interface for talking to the Telegram bot. The real
// implementation lives in `telegram.cpp` (`RealBotClient`) and owns a
// `WiFiClientSecure`. A fake lives under `test/support/fake_bot_client.h`
// and lets tests assert exactly which messages were sent and simulate
// failures.
class IBotClient
{
public:
    virtual ~IBotClient() = default;

    // Send a plain-text message to the configured chat. Returns true
    // iff the message was accepted end-to-end (HTTP 200 and the API
    // echoed `"ok":true`). False on any transport or parse failure —
    // the caller is responsible for the retry / reboot policy.
    virtual bool sendMessage(const String &text) = 0;

    // Same as sendMessage, but returns the new Telegram `message_id`
    // on success (always > 0) or 0 on any failure. Used by SmsHandler
    // so the reply-target ring buffer (RFC-0003) can map a future
    // user reply back to the original SMS sender phone number.
    //
    // Implementations that don't need the id (CallHandler, the boot
    // banner) can keep using the bool sendMessage(...) overload.
    virtual int32_t sendMessageReturningId(const String &text) = 0;

    // Send a plain-text message to an arbitrary chat ID. Returns true iff
    // the message was accepted end-to-end (HTTP 200 and "ok":true). Use
    // this for per-requester command replies where the target chat differs
    // from the admin chat. SMS forwards should still use sendMessage() /
    // sendMessageReturningId(), which always target adminChatId_.
    virtual bool sendMessageTo(int64_t chatId, const String &text) = 0;

    // Long-poll the Telegram getUpdates endpoint. Sends `offset =
    // sinceUpdateId + 1` if sinceUpdateId > 0, otherwise no offset.
    // `timeoutSec` is forwarded to Telegram as the `timeout` query
    // parameter — pass 0 for short polling, > 0 to long-poll. Note
    // the call BLOCKS for up to `timeoutSec` seconds, so the caller
    // is responsible for keeping the loop responsive (RFC-0003
    // implementation note: first cut uses a short timeout).
    //
    // Fills `out` with one TelegramUpdate per item in the response's
    // `result` array, even if individual messages fail to parse — in
    // that case the entry has `valid = false` but `updateId` is still
    // populated so the poller can advance past it.
    //
    // Returns true iff the HTTP request itself succeeded and the
    // top-level JSON envelope was parseable. If the call returns
    // false the caller should NOT advance its update_id watermark
    // (the request failed end-to-end and we'll retry next tick).
    virtual bool pollUpdates(int32_t sinceUpdateId, int32_t timeoutSec,
                             std::vector<TelegramUpdate> &out) = 0;
};
