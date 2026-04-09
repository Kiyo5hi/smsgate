#pragma once

#include <Arduino.h>
#include <vector>

#include "ibot_client.h"

// Scripted fake Telegram client. `sendMessage` returns whatever was
// queued (default: true) and records every message the handler tried
// to send so tests can assert content / ordering.
class FakeBotClient : public IBotClient
{
public:
    // Queue the return value for the next sendMessage() call. If the
    // queue is empty when sendMessage() is called, `defaultReturn` is
    // used (initialized to true).
    void queueResult(bool ok) { results_.push_back(ok); }

    // Set the return value for all calls with no queued result.
    void setDefault(bool ok) { defaultReturn_ = ok; }

    bool sendMessage(const String &text) override
    {
        sent_.push_back(text);
        if (!results_.empty())
        {
            bool r = results_.front();
            results_.erase(results_.begin());
            return r;
        }
        return defaultReturn_;
    }

    const std::vector<String> &sentMessages() const { return sent_; }

    size_t callCount() const { return sent_.size(); }

private:
    std::vector<bool> results_;
    bool defaultReturn_ = true;
    std::vector<String> sent_;
};
