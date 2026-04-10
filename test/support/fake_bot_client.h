#pragma once

#include <Arduino.h>
#include <vector>

#include "ibot_client.h"

// Scripted fake Telegram client. `sendMessage` returns whatever was
// queued (default: true) and records every message the handler tried
// to send so tests can assert content / ordering.
//
// `sendMessageReturningId` returns a monotonically increasing fake
// message id starting at 1000 (so tests can hard-code expected ids
// without colliding with default ints), or 0 on failure.
//
// `pollUpdates` returns batches of TelegramUpdate scripts queued via
// `queueUpdateBatch`. Each call drains one batch.
class FakeBotClient : public IBotClient
{
public:
    // Per-send metadata so tests can assert both content and the target chat.
    // chatId == 0 is the sentinel for admin-targeted sends (sendMessage /
    // sendMessageReturningId), which don't carry an explicit chat id.
    struct SentMessage
    {
        int64_t chatId; // 0 = admin-targeted sentinel
        String text;
    };

    // Queue the return value for the next sendMessage() call. If the
    // queue is empty when sendMessage() is called, `defaultReturn` is
    // used (initialized to true).
    void queueResult(bool ok) { results_.push_back(ok); }

    // Set the return value for all calls with no queued result.
    void setDefault(bool ok) { defaultReturn_ = ok; }

    bool sendMessage(const String &text) override
    {
        sent_.push_back({0, text}); // 0 = admin-targeted sentinel
        if (!results_.empty())
        {
            bool r = results_.front();
            results_.erase(results_.begin());
            return r;
        }
        return defaultReturn_;
    }

    int32_t sendMessageReturningId(const String &text) override
    {
        sent_.push_back({0, text}); // 0 = admin-targeted sentinel
        bool ok;
        if (!results_.empty())
        {
            ok = results_.front();
            results_.erase(results_.begin());
        }
        else
        {
            ok = defaultReturn_;
        }
        if (!ok)
        {
            return 0;
        }
        return ++lastFakeMsgId_;
    }

    bool sendMessageTo(int64_t chatId, const String &text) override
    {
        sent_.push_back({chatId, text});
        if (!results_.empty())
        {
            bool r = results_.front();
            results_.erase(results_.begin());
            return r;
        }
        return defaultReturn_;
    }

    // RFC-0054: same as sendMessageTo but returns message_id.
    int32_t sendMessageToReturningId(int64_t chatId, const String &text) override
    {
        sent_.push_back({chatId, text});
        if (!results_.empty())
        {
            bool r = results_.front();
            results_.erase(results_.begin());
            if (!r) return 0;
        }
        else if (!defaultReturn_)
        {
            return 0;
        }
        return ++lastFakeMsgId_;
    }

    // ---- pollUpdates (RFC-0003) ----

    struct PollResult
    {
        bool ok = true;
        std::vector<TelegramUpdate> updates;
    };

    void queueUpdateBatch(std::vector<TelegramUpdate> updates)
    {
        PollResult r;
        r.ok = true;
        r.updates = std::move(updates);
        pollResults_.push_back(std::move(r));
    }

    void queuePollFailure()
    {
        PollResult r;
        r.ok = false;
        pollResults_.push_back(std::move(r));
    }

    bool pollUpdates(int32_t sinceUpdateId, int32_t /*timeoutSec*/,
                     std::vector<TelegramUpdate> &out) override
    {
        pollCallCount_++;
        lastPollOffset_ = sinceUpdateId;
        out.clear();
        if (pollResults_.empty())
        {
            // Default: empty success
            return true;
        }
        PollResult r = std::move(pollResults_.front());
        pollResults_.erase(pollResults_.begin());
        if (!r.ok)
        {
            return false;
        }
        out = std::move(r.updates);
        return true;
    }

    int pollCallCount() const { return pollCallCount_; }
    int32_t lastPollOffset() const { return lastPollOffset_; }

    // Returns the text of all sent messages (regardless of target chat).
    // Kept returning std::vector<String> to avoid breaking the 13+ existing
    // call sites in test files that iterate over plain String values.
    std::vector<String> sentMessages() const
    {
        std::vector<String> texts;
        texts.reserve(sent_.size());
        for (const auto &m : sent_)
        {
            texts.push_back(m.text);
        }
        return texts;
    }

    // Returns all sent messages with their target chat IDs. Use this in
    // new tests that need to assert routing (RFC-0016).
    const std::vector<SentMessage> &sentMessagesWithTarget() const { return sent_; }

    // Returns the total number of send calls across all three send methods.
    size_t callCount() const { return sent_.size(); }

    // Clear accumulated sent messages. Useful in loop-driven tests.
    void clearMessages() { sent_.clear(); }

    int32_t lastIssuedMessageId() const { return lastFakeMsgId_; }

private:
    std::vector<bool> results_;
    bool defaultReturn_ = true;
    std::vector<SentMessage> sent_;
    int32_t lastFakeMsgId_ = 1000;
    std::vector<PollResult> pollResults_;
    int pollCallCount_ = 0;
    int32_t lastPollOffset_ = -1;
};
